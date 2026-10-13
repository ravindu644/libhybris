/*
 * Copyright (c) 2022 Jolla Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

/* For RTLD_DEFAULT */
#define _GNU_SOURCE

#define VK_USE_PLATFORM_ANDROID_KHR 1
#define VK_USE_PLATFORM_WAYLAND_KHR 1
#define VK_ENABLE_BETA_EXTENSIONS

#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <hybris/common/binding.h>
#include <hybris/common/floating_point_abi.h>
#include "config.h"
#include "logging.h"
#include "ws.h"

static void *vulkan_handle = NULL;

/*
 * This generates a function that when first called overwrites it's plt entry with new address.
 * Subsequent calls jump directly at the target function in the android library.
 * This means effectively 0 call overhead after the first call.
 */

#define VULKAN_IDLOAD(sym) \
 __asm__ (".type " #sym ", %gnu_indirect_function"); \
typeof(sym) * sym ## _dispatch (void) __asm__ (#sym);\
typeof(sym) * sym ## _dispatch (void) \
{ \
    if (!vulkan_handle) _init_androidvulkan(); \
    return (void *) android_dlsym(vulkan_handle, #sym); \
}

static void _init_androidvulkan()
{
    vulkan_handle = (void *) android_dlopen(getenv("LIBVULKAN") ? getenv("LIBVULKAN") : "libvulkan.so", RTLD_LAZY);
}

static inline void hybris_vulkan_initialize()
{
    _init_androidvulkan();
}

static void * _android_vulkan_dlsym(const char *symbol)
{
    if (vulkan_handle == NULL)
        _init_androidvulkan();

    return android_dlsym(vulkan_handle, symbol);
}

struct ws_vulkan_interface hybris_vulkan_interface = {
    _android_vulkan_dlsym,
};

static PFN_vkVoidFunction (*_vkGetInstanceProcAddr)(VkInstance instance, const char* pName) = NULL;

/* Use IDLOAD approach also for float functions, since vulkan uses the aapcs-vfp calling convention even on android */

VkResult vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance)
{
    if (_vkGetInstanceProcAddr == NULL) {
        HYBRIS_DLSYSM(vulkan, &_vkGetInstanceProcAddr, "vkGetInstanceProcAddr");
    }
    ws_vkSetInstanceProcAddrFunc((PFN_vkVoidFunction)_vkGetInstanceProcAddr);

    return ws_vkCreateInstance(pCreateInfo, pAllocator, pInstance);
}

VULKAN_IDLOAD(vkDestroyInstance);
VULKAN_IDLOAD(vkEnumeratePhysicalDevices);
VULKAN_IDLOAD(vkGetPhysicalDeviceFeatures);
VULKAN_IDLOAD(vkGetPhysicalDeviceFormatProperties);
VULKAN_IDLOAD(vkGetPhysicalDeviceImageFormatProperties);
static void hybris_cap_api_version(VkPhysicalDeviceProperties *props)
{
    const char *v = getenv("HYBRIS_VK_API_VERSION");
    unsigned maj, min;

    if (v && sscanf(v, "%u.%u", &maj, &min) == 2 && props->apiVersion > VK_MAKE_VERSION(maj, min, 0))
        props->apiVersion = VK_MAKE_VERSION(maj, min, 0);
}

void vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties *props)
{
    static PFN_vkGetPhysicalDeviceProperties real;

    if (!real)
        real = (PFN_vkGetPhysicalDeviceProperties)_android_vulkan_dlsym("vkGetPhysicalDeviceProperties");
    real(physicalDevice, props);
    hybris_cap_api_version(props);
}
VULKAN_IDLOAD(vkGetPhysicalDeviceQueueFamilyProperties);
VULKAN_IDLOAD(vkGetPhysicalDeviceMemoryProperties);

VkResult vkEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties)
{
    if (_vkGetInstanceProcAddr == NULL) {
        HYBRIS_DLSYSM(vulkan, &_vkGetInstanceProcAddr, "vkGetInstanceProcAddr");
    }
    ws_vkSetInstanceProcAddrFunc((PFN_vkVoidFunction)_vkGetInstanceProcAddr);

    return ws_vkEnumerateInstanceExtensionProperties(pLayerName, pPropertyCount, pProperties);
}

#ifdef WANT_WAYLAND
VkResult vkCreateWaylandSurfaceKHR(VkInstance instance,
        const VkWaylandSurfaceCreateInfoKHR* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSurfaceKHR* pSurface)
{
    return ws_vkCreateWaylandSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
}

VkBool32 vkGetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, struct wl_display* display)
{
    return ws_vkGetPhysicalDeviceWaylandPresentationSupportKHR(physicalDevice, queueFamilyIndex, display);
}

void vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* pAllocator)
{
    ws_vkDestroySurfaceKHR(instance, surface, pAllocator);
}
#else
VULKAN_IDLOAD(vkDestroySurfaceKHR);
#endif

static PFN_vkVoidFunction hybris_dmabuf_hook(const char *pName);

PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char* pName)
{
    if (_vkGetInstanceProcAddr == NULL) {
        HYBRIS_DLSYSM(vulkan, &_vkGetInstanceProcAddr, "vkGetInstanceProcAddr");
    }

    if (!strcmp(pName, "vkEnumerateInstanceExtensionProperties")) {
        return (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties;
    } else if (!strcmp(pName, "vkCreateInstance")) {
        return (PFN_vkVoidFunction)vkCreateInstance;
    } else if (!strcmp(pName, "vkGetInstanceProcAddr")) {
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    } else if (!strcmp(pName, "vkEnumerateDeviceExtensionProperties")) {
        return (PFN_vkVoidFunction)vkEnumerateDeviceExtensionProperties;
    } else if (!strcmp(pName, "vkGetPhysicalDeviceProperties")) {
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties;
    } else if (!strcmp(pName, "vkGetPhysicalDeviceProperties2") ||
               !strcmp(pName, "vkGetPhysicalDeviceProperties2KHR")) {
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties2;
    } else if (!strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
#ifdef WANT_WAYLAND
    } else if (!strcmp(pName, "vkCreateWaylandSurfaceKHR")) {
        return (PFN_vkVoidFunction)vkCreateWaylandSurfaceKHR;
    } else if (!strcmp(pName, "vkGetPhysicalDeviceWaylandPresentationSupportKHR")) {
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceWaylandPresentationSupportKHR;
    } else if (!strcmp(pName, "vkDestroySurfaceKHR")) {
        return (PFN_vkVoidFunction)vkDestroySurfaceKHR;
#endif
    } else {
        PFN_vkVoidFunction hook = hybris_dmabuf_hook(pName);
        if (hook)
            return hook;
    }

    return (*_vkGetInstanceProcAddr)(instance, pName);
}

static PFN_vkCreateSwapchainKHR hybris_real_create_swapchain;
VkResult vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain);

static PFN_vkGetDeviceProcAddr hybris_real_gdpa;

PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
    static PFN_vkGetDeviceProcAddr real;
    PFN_vkVoidFunction hook;

    if (!real)
        hybris_real_gdpa = real = (PFN_vkGetDeviceProcAddr)_android_vulkan_dlsym("vkGetDeviceProcAddr");
    if (!strcmp(pName, "vkCreateSwapchainKHR")) {
        hybris_real_create_swapchain = (PFN_vkCreateSwapchainKHR)real(device, pName);
        return (PFN_vkVoidFunction)vkCreateSwapchainKHR;
    }
    if ((hook = hybris_dmabuf_hook(pName)))
        return hook;
    return real(device, pName);
}
VULKAN_IDLOAD(vkCreateDevice);
VULKAN_IDLOAD(vkDestroyDevice);
/* Debug knob for bisecting driver bugs from a layer-less stack (Khronos layers can't sit
 * on top of an Android loader). HYBRIS_VK_HIDE_EXTS=a,b hides device extensions. */
static int hybris_ext_hidden(const char *name)
{
    const char *list = getenv("HYBRIS_VK_HIDE_EXTS");
    size_t n = strlen(name);

    for (const char *p = list; p && *p; ) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !strncmp(p, name, n))
            return 1;
        p = e ? e + 1 : NULL;
    }
    return 0;
}

VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
    static PFN_vkEnumerateDeviceExtensionProperties real;
    VkExtensionProperties *all;
    uint32_t count = 0, kept = 0;
    VkResult ret;

    if (!real)
        real = (PFN_vkEnumerateDeviceExtensionProperties)_android_vulkan_dlsym("vkEnumerateDeviceExtensionProperties");
    if (!getenv("HYBRIS_VK_HIDE_EXTS"))
        return real(physicalDevice, pLayerName, pPropertyCount, pProperties);

    ret = real(physicalDevice, pLayerName, &count, NULL);
    if (ret != VK_SUCCESS)
        return ret;
    all = calloc(count, sizeof(*all));
    if (!all)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    real(physicalDevice, pLayerName, &count, all);
    for (uint32_t i = 0; i < count; i++) {
        if (hybris_ext_hidden(all[i].extensionName))
            continue;
        if (pProperties && kept < *pPropertyCount)
            pProperties[kept] = all[i];
        kept++;
    }
    free(all);
    if (!pProperties) {
        *pPropertyCount = kept;
        return VK_SUCCESS;
    }
    ret = kept > *pPropertyCount ? VK_INCOMPLETE : VK_SUCCESS;
    if (kept < *pPropertyCount)
        *pPropertyCount = kept;
    return ret;
}
VULKAN_IDLOAD(vkEnumerateInstanceLayerProperties);
VULKAN_IDLOAD(vkEnumerateDeviceLayerProperties);
VULKAN_IDLOAD(vkGetDeviceQueue);
VULKAN_IDLOAD(vkQueueSubmit);
VULKAN_IDLOAD(vkQueueWaitIdle);
VULKAN_IDLOAD(vkDeviceWaitIdle);
/* vkAllocateMemory: dma-buf export emulation */
/* vkFreeMemory: dma-buf export emulation */
VULKAN_IDLOAD(vkMapMemory);
VULKAN_IDLOAD(vkUnmapMemory);
VULKAN_IDLOAD(vkFlushMappedMemoryRanges);
VULKAN_IDLOAD(vkInvalidateMappedMemoryRanges);
VULKAN_IDLOAD(vkGetDeviceMemoryCommitment);
VULKAN_IDLOAD(vkBindBufferMemory);
VULKAN_IDLOAD(vkBindImageMemory);
VULKAN_IDLOAD(vkGetBufferMemoryRequirements);
VULKAN_IDLOAD(vkGetImageMemoryRequirements);
VULKAN_IDLOAD(vkGetImageSparseMemoryRequirements);
VULKAN_IDLOAD(vkGetPhysicalDeviceSparseImageFormatProperties);
VULKAN_IDLOAD(vkQueueBindSparse);
VULKAN_IDLOAD(vkCreateFence);
VULKAN_IDLOAD(vkDestroyFence);
VULKAN_IDLOAD(vkResetFences);
VULKAN_IDLOAD(vkGetFenceStatus);
VULKAN_IDLOAD(vkWaitForFences);
VULKAN_IDLOAD(vkCreateSemaphore);
VULKAN_IDLOAD(vkDestroySemaphore);
VULKAN_IDLOAD(vkCreateEvent);
VULKAN_IDLOAD(vkDestroyEvent);
VULKAN_IDLOAD(vkGetEventStatus);
VULKAN_IDLOAD(vkSetEvent);
VULKAN_IDLOAD(vkResetEvent);
VULKAN_IDLOAD(vkCreateQueryPool);
VULKAN_IDLOAD(vkDestroyQueryPool);
VULKAN_IDLOAD(vkGetQueryPoolResults);
/* vkCreateBuffer: dma-buf export emulation */
VULKAN_IDLOAD(vkDestroyBuffer);
VULKAN_IDLOAD(vkCreateBufferView);
VULKAN_IDLOAD(vkDestroyBufferView);
/* vkCreateImage: dma-buf export emulation */
VULKAN_IDLOAD(vkDestroyImage);
VULKAN_IDLOAD(vkGetImageSubresourceLayout);
VULKAN_IDLOAD(vkCreateImageView);
VULKAN_IDLOAD(vkDestroyImageView);
VULKAN_IDLOAD(vkCreateShaderModule);
VULKAN_IDLOAD(vkDestroyShaderModule);
VULKAN_IDLOAD(vkCreatePipelineCache);
VULKAN_IDLOAD(vkDestroyPipelineCache);
VULKAN_IDLOAD(vkGetPipelineCacheData);
VULKAN_IDLOAD(vkMergePipelineCaches);
VULKAN_IDLOAD(vkCreateGraphicsPipelines);
VULKAN_IDLOAD(vkCreateComputePipelines);
VULKAN_IDLOAD(vkDestroyPipeline);
VULKAN_IDLOAD(vkCreatePipelineLayout);
VULKAN_IDLOAD(vkDestroyPipelineLayout);
VULKAN_IDLOAD(vkCreateSampler);
VULKAN_IDLOAD(vkDestroySampler);
VULKAN_IDLOAD(vkCreateDescriptorSetLayout);
VULKAN_IDLOAD(vkDestroyDescriptorSetLayout);
VULKAN_IDLOAD(vkCreateDescriptorPool);
VULKAN_IDLOAD(vkDestroyDescriptorPool);
VULKAN_IDLOAD(vkResetDescriptorPool);
VULKAN_IDLOAD(vkAllocateDescriptorSets);
VULKAN_IDLOAD(vkFreeDescriptorSets);
VULKAN_IDLOAD(vkUpdateDescriptorSets);
VULKAN_IDLOAD(vkCreateFramebuffer);
VULKAN_IDLOAD(vkDestroyFramebuffer);
VULKAN_IDLOAD(vkCreateRenderPass);
VULKAN_IDLOAD(vkDestroyRenderPass);
VULKAN_IDLOAD(vkGetRenderAreaGranularity);
VULKAN_IDLOAD(vkCreateCommandPool);
VULKAN_IDLOAD(vkDestroyCommandPool);
VULKAN_IDLOAD(vkResetCommandPool);
VULKAN_IDLOAD(vkAllocateCommandBuffers);
VULKAN_IDLOAD(vkFreeCommandBuffers);
VULKAN_IDLOAD(vkBeginCommandBuffer);
VULKAN_IDLOAD(vkEndCommandBuffer);
VULKAN_IDLOAD(vkResetCommandBuffer);
VULKAN_IDLOAD(vkCmdBindPipeline);
VULKAN_IDLOAD(vkCmdSetViewport);
VULKAN_IDLOAD(vkCmdSetScissor);
VULKAN_IDLOAD(vkCmdSetLineWidth);
VULKAN_IDLOAD(vkCmdSetDepthBias);
VULKAN_IDLOAD(vkCmdSetBlendConstants);
VULKAN_IDLOAD(vkCmdSetDepthBounds);
VULKAN_IDLOAD(vkCmdSetStencilCompareMask);
VULKAN_IDLOAD(vkCmdSetStencilWriteMask);
VULKAN_IDLOAD(vkCmdSetStencilReference);
VULKAN_IDLOAD(vkCmdBindDescriptorSets);
VULKAN_IDLOAD(vkCmdBindIndexBuffer);
VULKAN_IDLOAD(vkCmdBindVertexBuffers);
VULKAN_IDLOAD(vkCmdDraw);
VULKAN_IDLOAD(vkCmdDrawIndexed);
VULKAN_IDLOAD(vkCmdDrawIndirect);
VULKAN_IDLOAD(vkCmdDrawIndexedIndirect);
VULKAN_IDLOAD(vkCmdDispatch);
VULKAN_IDLOAD(vkCmdDispatchIndirect);
VULKAN_IDLOAD(vkCmdCopyBuffer);
VULKAN_IDLOAD(vkCmdCopyImage);
VULKAN_IDLOAD(vkCmdBlitImage);
VULKAN_IDLOAD(vkCmdCopyBufferToImage);
VULKAN_IDLOAD(vkCmdCopyImageToBuffer);
VULKAN_IDLOAD(vkCmdUpdateBuffer);
VULKAN_IDLOAD(vkCmdFillBuffer);
VULKAN_IDLOAD(vkCmdClearColorImage);
VULKAN_IDLOAD(vkCmdClearDepthStencilImage);
VULKAN_IDLOAD(vkCmdClearAttachments);
VULKAN_IDLOAD(vkCmdResolveImage);
VULKAN_IDLOAD(vkCmdSetEvent);
VULKAN_IDLOAD(vkCmdResetEvent);
VULKAN_IDLOAD(vkCmdWaitEvents);
VULKAN_IDLOAD(vkCmdPipelineBarrier);
VULKAN_IDLOAD(vkCmdBeginQuery);
VULKAN_IDLOAD(vkCmdEndQuery);
VULKAN_IDLOAD(vkCmdResetQueryPool);
VULKAN_IDLOAD(vkCmdWriteTimestamp);
VULKAN_IDLOAD(vkCmdCopyQueryPoolResults);
VULKAN_IDLOAD(vkCmdPushConstants);
VULKAN_IDLOAD(vkCmdBeginRenderPass);
VULKAN_IDLOAD(vkCmdNextSubpass);
VULKAN_IDLOAD(vkCmdEndRenderPass);
VULKAN_IDLOAD(vkCmdExecuteCommands);
VULKAN_IDLOAD(vkEnumerateInstanceVersion);
VULKAN_IDLOAD(vkBindBufferMemory2);
VULKAN_IDLOAD(vkBindImageMemory2);
VULKAN_IDLOAD(vkGetDeviceGroupPeerMemoryFeatures);
VULKAN_IDLOAD(vkCmdSetDeviceMask);
VULKAN_IDLOAD(vkCmdDispatchBase);
VULKAN_IDLOAD(vkEnumeratePhysicalDeviceGroups);
VULKAN_IDLOAD(vkGetImageMemoryRequirements2);
VULKAN_IDLOAD(vkGetBufferMemoryRequirements2);
VULKAN_IDLOAD(vkGetImageSparseMemoryRequirements2);
VULKAN_IDLOAD(vkGetPhysicalDeviceFeatures2);
/* An Android driver has no DRM device, so it leaves VkPhysicalDeviceDrmPropertiesEXT
 * zeroed. Mesa's zink picks its Vulkan device by matching those numbers against the
 * DRM fd the compositor advertised, so on a phone with only a display node it never
 * found one, and fell back to copying every frame through wl_shm. HYBRIS_VK_DRM_NODE
 * names the node to report (e.g. /dev/dri/card0); unset, nothing changes. */
static void hybris_fill_drm_props(VkPhysicalDeviceProperties2 *props)
{
    static int have = -1;
    static struct stat node;
    const char *path;

    if (have < 0) {
        path = getenv("HYBRIS_VK_DRM_NODE");
        have = path && stat(path, &node) == 0 && S_ISCHR(node.st_mode);
    }
    if (!have)
        return;
    for (VkBaseOutStructure *p = (VkBaseOutStructure *)props->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT) {
            VkPhysicalDeviceDrmPropertiesEXT *drm = (VkPhysicalDeviceDrmPropertiesEXT *)p;
            drm->hasPrimary = drm->hasRender = VK_TRUE;
            drm->primaryMajor = drm->renderMajor = major(node.st_rdev);
            drm->primaryMinor = drm->renderMinor = minor(node.st_rdev);
        }
    }
}

void vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *props)
{
    static PFN_vkGetPhysicalDeviceProperties2 real;

    if (!real)
        real = (PFN_vkGetPhysicalDeviceProperties2)_android_vulkan_dlsym("vkGetPhysicalDeviceProperties2");
    real(physicalDevice, props);
    hybris_fill_drm_props(props);
    hybris_cap_api_version(&props->properties);
}
VULKAN_IDLOAD(vkGetPhysicalDeviceFormatProperties2);
/* vkGetPhysicalDeviceImageFormatProperties2: dma-buf export emulation */
VULKAN_IDLOAD(vkGetPhysicalDeviceQueueFamilyProperties2);
VULKAN_IDLOAD(vkGetPhysicalDeviceMemoryProperties2);
VULKAN_IDLOAD(vkGetPhysicalDeviceSparseImageFormatProperties2);
VULKAN_IDLOAD(vkTrimCommandPool);
VULKAN_IDLOAD(vkGetDeviceQueue2);
VULKAN_IDLOAD(vkCreateSamplerYcbcrConversion);
VULKAN_IDLOAD(vkDestroySamplerYcbcrConversion);
VULKAN_IDLOAD(vkCreateDescriptorUpdateTemplate);
VULKAN_IDLOAD(vkDestroyDescriptorUpdateTemplate);
VULKAN_IDLOAD(vkUpdateDescriptorSetWithTemplate);
/* vkGetPhysicalDeviceExternalBufferProperties: dma-buf export emulation */
VULKAN_IDLOAD(vkGetPhysicalDeviceExternalFenceProperties);
VULKAN_IDLOAD(vkGetPhysicalDeviceExternalSemaphoreProperties);
VULKAN_IDLOAD(vkGetDescriptorSetLayoutSupport);
VULKAN_IDLOAD(vkCmdDrawIndirectCount);
VULKAN_IDLOAD(vkCmdDrawIndexedIndirectCount);
VULKAN_IDLOAD(vkCreateRenderPass2);
VULKAN_IDLOAD(vkCmdBeginRenderPass2);
VULKAN_IDLOAD(vkCmdNextSubpass2);
VULKAN_IDLOAD(vkCmdEndRenderPass2);
VULKAN_IDLOAD(vkResetQueryPool);
VULKAN_IDLOAD(vkGetSemaphoreCounterValue);
VULKAN_IDLOAD(vkWaitSemaphores);
VULKAN_IDLOAD(vkSignalSemaphore);
VULKAN_IDLOAD(vkGetBufferDeviceAddress);
VULKAN_IDLOAD(vkGetBufferOpaqueCaptureAddress);
VULKAN_IDLOAD(vkGetDeviceMemoryOpaqueCaptureAddress);
#if VK_HEADER_VERSION >= 204
VULKAN_IDLOAD(vkGetPhysicalDeviceToolProperties);
VULKAN_IDLOAD(vkCreatePrivateDataSlot);
VULKAN_IDLOAD(vkDestroyPrivateDataSlot);
VULKAN_IDLOAD(vkSetPrivateData);
VULKAN_IDLOAD(vkGetPrivateData);
VULKAN_IDLOAD(vkCmdSetEvent2);
VULKAN_IDLOAD(vkCmdResetEvent2);
VULKAN_IDLOAD(vkCmdWaitEvents2);
VULKAN_IDLOAD(vkCmdPipelineBarrier2);
VULKAN_IDLOAD(vkCmdWriteTimestamp2);
VULKAN_IDLOAD(vkQueueSubmit2);
VULKAN_IDLOAD(vkCmdCopyBuffer2);
VULKAN_IDLOAD(vkCmdCopyImage2);
VULKAN_IDLOAD(vkCmdCopyBufferToImage2);
VULKAN_IDLOAD(vkCmdCopyImageToBuffer2);
VULKAN_IDLOAD(vkCmdBlitImage2);
VULKAN_IDLOAD(vkCmdResolveImage2);
VULKAN_IDLOAD(vkCmdBeginRendering);
VULKAN_IDLOAD(vkCmdEndRendering);
VULKAN_IDLOAD(vkCmdSetCullMode);
VULKAN_IDLOAD(vkCmdSetFrontFace);
VULKAN_IDLOAD(vkCmdSetPrimitiveTopology);
VULKAN_IDLOAD(vkCmdSetViewportWithCount);
VULKAN_IDLOAD(vkCmdSetScissorWithCount);
VULKAN_IDLOAD(vkCmdBindVertexBuffers2);
VULKAN_IDLOAD(vkCmdSetDepthTestEnable);
VULKAN_IDLOAD(vkCmdSetDepthWriteEnable);
VULKAN_IDLOAD(vkCmdSetDepthCompareOp);
VULKAN_IDLOAD(vkCmdSetDepthBoundsTestEnable);
VULKAN_IDLOAD(vkCmdSetStencilTestEnable);
VULKAN_IDLOAD(vkCmdSetStencilOp);
VULKAN_IDLOAD(vkCmdSetRasterizerDiscardEnable);
VULKAN_IDLOAD(vkCmdSetDepthBiasEnable);
VULKAN_IDLOAD(vkCmdSetPrimitiveRestartEnable);
VULKAN_IDLOAD(vkGetDeviceBufferMemoryRequirements);
VULKAN_IDLOAD(vkGetDeviceImageMemoryRequirements);
VULKAN_IDLOAD(vkGetDeviceImageSparseMemoryRequirements);
#endif
VULKAN_IDLOAD(vkGetPhysicalDeviceSurfaceSupportKHR);
/* The Wayland platform creates every window at a 1x1 placeholder, and Android's
 * WSI reports the window's size as the surface's fixed currentExtent, so clients
 * built 1x1 swapchains that compositors then left unmapped. On Wayland the client
 * picks the size: report the "any size" extent, and the swapchain's size reaches
 * the window through native_window_set_buffers_dimensions(). */
VkResult vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *caps)
{
    static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR real;
    VkResult ret;

    if (!real)
        real = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)_android_vulkan_dlsym("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    ret = real(physicalDevice, surface, caps);
    if (ret == VK_SUCCESS && caps->currentExtent.width == 1 && caps->currentExtent.height == 1)
        caps->currentExtent.width = caps->currentExtent.height = 0xFFFFFFFF;
    return ret;
}
VULKAN_IDLOAD(vkGetPhysicalDeviceSurfaceFormatsKHR);
VULKAN_IDLOAD(vkGetPhysicalDeviceSurfacePresentModesKHR);
VkResult vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
    if (!hybris_real_create_swapchain)
        hybris_real_create_swapchain = (PFN_vkCreateSwapchainKHR)_android_vulkan_dlsym("vkCreateSwapchainKHR");
    ws_set_swap_rb(pCreateInfo->imageFormat == VK_FORMAT_B8G8R8A8_UNORM ||
                   pCreateInfo->imageFormat == VK_FORMAT_B8G8R8A8_SRGB);
    return hybris_real_create_swapchain(device, pCreateInfo, pAllocator, pSwapchain);
}
VULKAN_IDLOAD(vkDestroySwapchainKHR);
VULKAN_IDLOAD(vkGetSwapchainImagesKHR);
VULKAN_IDLOAD(vkAcquireNextImageKHR);
VULKAN_IDLOAD(vkQueuePresentKHR);
VULKAN_IDLOAD(vkGetDeviceGroupPresentCapabilitiesKHR);
VULKAN_IDLOAD(vkGetDeviceGroupSurfacePresentModesKHR);
VULKAN_IDLOAD(vkGetPhysicalDevicePresentRectanglesKHR);
VULKAN_IDLOAD(vkAcquireNextImage2KHR);
VULKAN_IDLOAD(vkGetPhysicalDeviceDisplayPropertiesKHR);
VULKAN_IDLOAD(vkGetPhysicalDeviceDisplayPlanePropertiesKHR);
VULKAN_IDLOAD(vkGetDisplayPlaneSupportedDisplaysKHR);
VULKAN_IDLOAD(vkGetDisplayModePropertiesKHR);
VULKAN_IDLOAD(vkCreateDisplayModeKHR);
VULKAN_IDLOAD(vkGetDisplayPlaneCapabilitiesKHR);
VULKAN_IDLOAD(vkCreateDisplayPlaneSurfaceKHR);
VULKAN_IDLOAD(vkCreateSharedSwapchainsKHR);
#if VK_HEADER_VERSION >= 238
VULKAN_IDLOAD(vkGetPhysicalDeviceVideoCapabilitiesKHR);
VULKAN_IDLOAD(vkGetPhysicalDeviceVideoFormatPropertiesKHR);
VULKAN_IDLOAD(vkCreateVideoSessionKHR);
VULKAN_IDLOAD(vkDestroyVideoSessionKHR);
VULKAN_IDLOAD(vkGetVideoSessionMemoryRequirementsKHR);
VULKAN_IDLOAD(vkBindVideoSessionMemoryKHR);
VULKAN_IDLOAD(vkCreateVideoSessionParametersKHR);
VULKAN_IDLOAD(vkUpdateVideoSessionParametersKHR);
VULKAN_IDLOAD(vkDestroyVideoSessionParametersKHR);
VULKAN_IDLOAD(vkCmdBeginVideoCodingKHR);
VULKAN_IDLOAD(vkCmdEndVideoCodingKHR);
VULKAN_IDLOAD(vkCmdControlVideoCodingKHR);
VULKAN_IDLOAD(vkCmdDecodeVideoKHR);
#endif
#if VK_HEADER_VERSION >= 197
VULKAN_IDLOAD(vkCmdBeginRenderingKHR);
VULKAN_IDLOAD(vkCmdEndRenderingKHR);
#endif
VULKAN_IDLOAD(vkGetPhysicalDeviceFeatures2KHR);
VULKAN_IDLOAD(vkGetPhysicalDeviceProperties2KHR);
VULKAN_IDLOAD(vkGetPhysicalDeviceFormatProperties2KHR);
/* vkGetPhysicalDeviceImageFormatProperties2KHR: dma-buf export emulation */
VULKAN_IDLOAD(vkGetPhysicalDeviceQueueFamilyProperties2KHR);
VULKAN_IDLOAD(vkGetPhysicalDeviceMemoryProperties2KHR);
VULKAN_IDLOAD(vkGetPhysicalDeviceSparseImageFormatProperties2KHR);
VULKAN_IDLOAD(vkGetDeviceGroupPeerMemoryFeaturesKHR);
VULKAN_IDLOAD(vkCmdSetDeviceMaskKHR);
VULKAN_IDLOAD(vkCmdDispatchBaseKHR);
VULKAN_IDLOAD(vkTrimCommandPoolKHR);
VULKAN_IDLOAD(vkEnumeratePhysicalDeviceGroupsKHR);
/* vkGetPhysicalDeviceExternalBufferPropertiesKHR: dma-buf export emulation */
/* dma-buf export emulation.
 *
 * The Mali blob advertises VK_KHR_external_memory_fd and VK_EXT_external_memory_dma_buf,
 * but its external memory properties say dma-bufs can only be imported, and only into
 * buffers. vkGetMemoryFdKHR returns VK_SUCCESS without writing the fd. zink needs export
 * for every GBM buffer (Xwayland's glamor) and for GLX/EGL on X11 through DRI3, and Mesa
 * feeds the "opaque" fd straight to drmPrimeFDToHandle().
 *
 * Do what bionic-vulkan-wrapper does: allocate exportable memory ourselves from a
 * dma-heap and import it, which the Mali renders into fine (probes/vkheapimg.c), then
 * hand out duplicates of our fd. Imported dma-bufs are tracked too, because zink
 * re-exports them for implicit sync. Images and buffers lose the external-memory create
 * info the Mali rejects, and the capability queries report export and import.
 *
 * ponytail: the pNext unlinking below briefly edits the caller's const chain, so two
 * threads sharing one chain would race; Mesa builds chains on the stack. */
#include <linux/dma-heap.h>
#include <sys/ioctl.h>

#define HYBRIS_DMABUF_TYPES (VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT | \
                             VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)

struct hybris_dmabuf_mem {
    VkDeviceMemory mem;
    int fd;
    struct hybris_dmabuf_mem *next;
};
static struct hybris_dmabuf_mem *hybris_dmabuf_mems;
static pthread_mutex_t hybris_dmabuf_lock = PTHREAD_MUTEX_INITIALIZER;
static int hybris_heap_fd = -1;
static pthread_once_t hybris_heap_once = PTHREAD_ONCE_INIT;

static void hybris_heap_open(void)
{
    const char *heap = getenv("HYBRIS_VK_DMA_HEAP");
    hybris_heap_fd = open(heap ? heap : "/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
}

static int hybris_heap_alloc(VkDeviceSize size)
{
    pthread_once(&hybris_heap_once, hybris_heap_open);
    if (hybris_heap_fd < 0)
        return -1;
    struct dma_heap_allocation_data a = { .len = (size + 4095) & ~(VkDeviceSize)4095,
                                          .fd_flags = O_RDWR | O_CLOEXEC };
    return ioctl(hybris_heap_fd, DMA_HEAP_IOCTL_ALLOC, &a) < 0 ? -1 : (int)a.fd;
}

/* Takes the first struct of type t out of s's pNext chain; hybris_relink() puts it back. */
static VkBaseOutStructure *hybris_unlink(const void *s, VkStructureType t, VkBaseOutStructure **prev)
{
    for (VkBaseOutStructure *p = (VkBaseOutStructure *)s; p->pNext; p = p->pNext) {
        if (p->pNext->sType == t) {
            VkBaseOutStructure *node = p->pNext;
            p->pNext = node->pNext;
            *prev = p;
            return node;
        }
    }
    return NULL;
}

static void hybris_relink(VkBaseOutStructure *prev, VkBaseOutStructure *node)
{
    if (node)
        prev->pNext = node;
}

static const void *hybris_find(const void *s, VkStructureType t)
{
    for (const VkBaseInStructure *p = ((const VkBaseInStructure *)s)->pNext; p; p = p->pNext)
        if (p->sType == t)
            return p;
    return NULL;
}

static void hybris_dmabuf_track(VkDeviceMemory mem, int fd)
{
    struct hybris_dmabuf_mem *m = malloc(sizeof(*m));
    m->mem = mem;
    m->fd = fd;
    pthread_mutex_lock(&hybris_dmabuf_lock);
    m->next = hybris_dmabuf_mems;
    hybris_dmabuf_mems = m;
    pthread_mutex_unlock(&hybris_dmabuf_lock);
}

/* The fd behind memory we know, or -1. take removes the record. */
static int hybris_dmabuf_fd(VkDeviceMemory mem, int take)
{
    int fd = -1;
    pthread_mutex_lock(&hybris_dmabuf_lock);
    for (struct hybris_dmabuf_mem **p = &hybris_dmabuf_mems; *p; p = &(*p)->next) {
        if ((*p)->mem == mem) {
            fd = (*p)->fd;
            if (take) {
                struct hybris_dmabuf_mem *m = *p;
                *p = m->next;
                free(m);
            }
            break;
        }
    }
    pthread_mutex_unlock(&hybris_dmabuf_lock);
    return fd;
}

static PFN_vkVoidFunction hybris_dev_fn(VkDevice device, const char *name)
{
    if (!hybris_real_gdpa)
        hybris_real_gdpa = (PFN_vkGetDeviceProcAddr)_android_vulkan_dlsym("vkGetDeviceProcAddr");
    return hybris_real_gdpa(device, name);
}

VkResult vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
                          const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory)
{
    static PFN_vkAllocateMemory real;
    if (!real)
        real = (PFN_vkAllocateMemory)_android_vulkan_dlsym("vkAllocateMemory");

    const VkImportMemoryFdInfoKHR *imp =
        hybris_find(pAllocateInfo, VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR);
    if (imp) {
        /* A successful import consumes the fd, so keep our own copy for vkGetMemoryFdKHR */
        int fd = (imp->handleType & HYBRIS_DMABUF_TYPES) ? fcntl(imp->fd, F_DUPFD_CLOEXEC, 0) : -1;
        VkResult ret = real(device, pAllocateInfo, pAllocator, pMemory);
        if (ret == VK_SUCCESS && fd >= 0)
            hybris_dmabuf_track(*pMemory, fd);
        else if (fd >= 0)
            close(fd);
        return ret;
    }

    const VkExportMemoryAllocateInfo *exp =
        hybris_find(pAllocateInfo, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
    if (!exp || !(exp->handleTypes & HYBRIS_DMABUF_TYPES))
        return real(device, pAllocateInfo, pAllocator, pMemory);

    int fd = hybris_heap_alloc(pAllocateInfo->allocationSize);
    if (fd < 0) {
        fprintf(stderr, "hybris: dma-heap allocation of %llu bytes failed; this memory will not "
                "be exportable\n", (unsigned long long)pAllocateInfo->allocationSize);
        return real(device, pAllocateInfo, pAllocator, pMemory);
    }

    PFN_vkGetMemoryFdPropertiesKHR props_fn =
        (PFN_vkGetMemoryFdPropertiesKHR)hybris_dev_fn(device, "vkGetMemoryFdPropertiesKHR");
    VkMemoryFdPropertiesKHR props = { VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    if (props_fn)
        props_fn(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &props);

    VkBaseOutStructure *prev, *node = hybris_unlink(pAllocateInfo,
        VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &prev);
    VkImportMemoryFdInfoKHR import = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        pAllocateInfo->pNext, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        fcntl(fd, F_DUPFD_CLOEXEC, 0) };
    VkMemoryAllocateInfo info = *pAllocateInfo;
    info.pNext = &import;
    /* The import may only land in types the dma-buf allows */
    if (props.memoryTypeBits && !(props.memoryTypeBits & (1u << info.memoryTypeIndex)))
        info.memoryTypeIndex = __builtin_ctz(props.memoryTypeBits);
    VkResult ret = real(device, &info, pAllocator, pMemory);
    hybris_relink(prev, node);

    if (ret != VK_SUCCESS) {
        fprintf(stderr, "hybris: importing a %llu-byte dma-buf failed (%d, type %u, fd types 0x%x); "
                "this memory will not be exportable\n", (unsigned long long)info.allocationSize,
                ret, info.memoryTypeIndex, props.memoryTypeBits);
        close(import.fd);
        close(fd);
        return real(device, pAllocateInfo, pAllocator, pMemory);
    }
    hybris_dmabuf_track(*pMemory, fd);
    return VK_SUCCESS;
}

void vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *pAllocator)
{
    static PFN_vkFreeMemory real;
    if (!real)
        real = (PFN_vkFreeMemory)_android_vulkan_dlsym("vkFreeMemory");
    int fd = memory ? hybris_dmabuf_fd(memory, 1) : -1;
    real(device, memory, pAllocator);
    if (fd >= 0)
        close(fd);
}

VkResult vkGetMemoryFdKHR(VkDevice device, const VkMemoryGetFdInfoKHR *pGetFdInfo, int *pFd)
{
    int fd = (pGetFdInfo->handleType & HYBRIS_DMABUF_TYPES) ? hybris_dmabuf_fd(pGetFdInfo->memory, 0) : -1;
    if (fd >= 0) {
        *pFd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        return *pFd >= 0 ? VK_SUCCESS : VK_ERROR_TOO_MANY_OBJECTS;
    }
    /* The Mali's own export reports success without an fd; say so instead of lying */
    *pFd = -1;
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

VkResult vkCreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
    static PFN_vkCreateImage real;
    if (!real)
        real = (PFN_vkCreateImage)_android_vulkan_dlsym("vkCreateImage");
    const VkExternalMemoryImageCreateInfo *ext =
        hybris_find(pCreateInfo, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
    if (!ext || (ext->handleTypes & ~HYBRIS_DMABUF_TYPES))
        return real(device, pCreateInfo, pAllocator, pImage);
    VkBaseOutStructure *prev, *node = hybris_unlink(pCreateInfo,
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &prev);
    VkResult ret = real(device, pCreateInfo, pAllocator, pImage);
    hybris_relink(prev, node);
    return ret;
}

VkResult vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
    static PFN_vkCreateBuffer real;
    if (!real)
        real = (PFN_vkCreateBuffer)_android_vulkan_dlsym("vkCreateBuffer");
    const VkExternalMemoryBufferCreateInfo *ext =
        hybris_find(pCreateInfo, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO);
    if (!ext || (ext->handleTypes & ~HYBRIS_DMABUF_TYPES))
        return real(device, pCreateInfo, pAllocator, pBuffer);
    VkBaseOutStructure *prev, *node = hybris_unlink(pCreateInfo,
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, &prev);
    VkResult ret = real(device, pCreateInfo, pAllocator, pBuffer);
    hybris_relink(prev, node);
    return ret;
}

static const VkExternalMemoryProperties hybris_dmabuf_props = {
    VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
    HYBRIS_DMABUF_TYPES, HYBRIS_DMABUF_TYPES,
};

VkResult vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo, VkImageFormatProperties2 *pImageFormatProperties)
{
    static PFN_vkGetPhysicalDeviceImageFormatProperties2 real;
    if (!real)
        real = (PFN_vkGetPhysicalDeviceImageFormatProperties2)_android_vulkan_dlsym("vkGetPhysicalDeviceImageFormatProperties2");
    const VkPhysicalDeviceExternalImageFormatInfo *ext =
        hybris_find(pImageFormatInfo, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
    if (!ext || !(ext->handleType & HYBRIS_DMABUF_TYPES))
        return real(physicalDevice, pImageFormatInfo, pImageFormatProperties);
    VkBaseOutStructure *prev, *node = hybris_unlink(pImageFormatInfo,
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, &prev);
    VkResult ret = real(physicalDevice, pImageFormatInfo, pImageFormatProperties);
    hybris_relink(prev, node);
    VkExternalImageFormatProperties *out = (VkExternalImageFormatProperties *)
        hybris_find(pImageFormatProperties, VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
    if (ret == VK_SUCCESS && out)
        out->externalMemoryProperties = hybris_dmabuf_props;
    return ret;
}

VkResult vkGetPhysicalDeviceImageFormatProperties2KHR(VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo, VkImageFormatProperties2 *pImageFormatProperties)
{
    return vkGetPhysicalDeviceImageFormatProperties2(physicalDevice, pImageFormatInfo, pImageFormatProperties);
}

void vkGetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalBufferInfo *pExternalBufferInfo, VkExternalBufferProperties *pExternalBufferProperties)
{
    static PFN_vkGetPhysicalDeviceExternalBufferProperties real;
    if (!real)
        real = (PFN_vkGetPhysicalDeviceExternalBufferProperties)_android_vulkan_dlsym("vkGetPhysicalDeviceExternalBufferProperties");
    real(physicalDevice, pExternalBufferInfo, pExternalBufferProperties);
    if (pExternalBufferInfo->handleType & HYBRIS_DMABUF_TYPES)
        pExternalBufferProperties->externalMemoryProperties = hybris_dmabuf_props;
}

void vkGetPhysicalDeviceExternalBufferPropertiesKHR(VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalBufferInfo *pExternalBufferInfo, VkExternalBufferProperties *pExternalBufferProperties)
{
    vkGetPhysicalDeviceExternalBufferProperties(physicalDevice, pExternalBufferInfo, pExternalBufferProperties);
}

static PFN_vkVoidFunction hybris_dmabuf_hook(const char *pName)
{
    static const struct { const char *name; PFN_vkVoidFunction fn; } hooks[] = {
        { "vkAllocateMemory", (PFN_vkVoidFunction)vkAllocateMemory },
        { "vkFreeMemory", (PFN_vkVoidFunction)vkFreeMemory },
        { "vkGetMemoryFdKHR", (PFN_vkVoidFunction)vkGetMemoryFdKHR },
        { "vkCreateImage", (PFN_vkVoidFunction)vkCreateImage },
        { "vkCreateBuffer", (PFN_vkVoidFunction)vkCreateBuffer },
        { "vkGetPhysicalDeviceImageFormatProperties2", (PFN_vkVoidFunction)vkGetPhysicalDeviceImageFormatProperties2 },
        { "vkGetPhysicalDeviceImageFormatProperties2KHR", (PFN_vkVoidFunction)vkGetPhysicalDeviceImageFormatProperties2KHR },
        { "vkGetPhysicalDeviceExternalBufferProperties", (PFN_vkVoidFunction)vkGetPhysicalDeviceExternalBufferProperties },
        { "vkGetPhysicalDeviceExternalBufferPropertiesKHR", (PFN_vkVoidFunction)vkGetPhysicalDeviceExternalBufferPropertiesKHR },
    };
    for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++)
        if (!strcmp(pName, hooks[i].name))
            return hooks[i].fn;
    return NULL;
}
/* end of dma-buf export emulation */
VULKAN_IDLOAD(vkGetMemoryFdPropertiesKHR);
VULKAN_IDLOAD(vkGetPhysicalDeviceExternalSemaphorePropertiesKHR);
VULKAN_IDLOAD(vkImportSemaphoreFdKHR);
VULKAN_IDLOAD(vkGetSemaphoreFdKHR);
VULKAN_IDLOAD(vkCmdPushDescriptorSetKHR);
VULKAN_IDLOAD(vkCmdPushDescriptorSetWithTemplateKHR);
VULKAN_IDLOAD(vkCreateDescriptorUpdateTemplateKHR);
VULKAN_IDLOAD(vkDestroyDescriptorUpdateTemplateKHR);
VULKAN_IDLOAD(vkUpdateDescriptorSetWithTemplateKHR);
VULKAN_IDLOAD(vkCreateRenderPass2KHR);
VULKAN_IDLOAD(vkCmdBeginRenderPass2KHR);
VULKAN_IDLOAD(vkCmdNextSubpass2KHR);
VULKAN_IDLOAD(vkCmdEndRenderPass2KHR);
VULKAN_IDLOAD(vkGetSwapchainStatusKHR);
VULKAN_IDLOAD(vkGetPhysicalDeviceExternalFencePropertiesKHR);
VULKAN_IDLOAD(vkImportFenceFdKHR);
VULKAN_IDLOAD(vkGetFenceFdKHR);
VULKAN_IDLOAD(vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR);
VULKAN_IDLOAD(vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR);
VULKAN_IDLOAD(vkAcquireProfilingLockKHR);
VULKAN_IDLOAD(vkReleaseProfilingLockKHR);
VULKAN_IDLOAD(vkGetPhysicalDeviceSurfaceCapabilities2KHR);
VULKAN_IDLOAD(vkGetPhysicalDeviceSurfaceFormats2KHR);
VULKAN_IDLOAD(vkGetPhysicalDeviceDisplayProperties2KHR);
VULKAN_IDLOAD(vkGetPhysicalDeviceDisplayPlaneProperties2KHR);
VULKAN_IDLOAD(vkGetDisplayModeProperties2KHR);
VULKAN_IDLOAD(vkGetDisplayPlaneCapabilities2KHR);
VULKAN_IDLOAD(vkGetImageMemoryRequirements2KHR);
VULKAN_IDLOAD(vkGetBufferMemoryRequirements2KHR);
VULKAN_IDLOAD(vkGetImageSparseMemoryRequirements2KHR);
VULKAN_IDLOAD(vkCreateSamplerYcbcrConversionKHR);
VULKAN_IDLOAD(vkDestroySamplerYcbcrConversionKHR);
VULKAN_IDLOAD(vkBindBufferMemory2KHR);
VULKAN_IDLOAD(vkBindImageMemory2KHR);
VULKAN_IDLOAD(vkGetDescriptorSetLayoutSupportKHR);
VULKAN_IDLOAD(vkCmdDrawIndirectCountKHR);
VULKAN_IDLOAD(vkCmdDrawIndexedIndirectCountKHR);
VULKAN_IDLOAD(vkGetSemaphoreCounterValueKHR);
VULKAN_IDLOAD(vkWaitSemaphoresKHR);
VULKAN_IDLOAD(vkSignalSemaphoreKHR);
VULKAN_IDLOAD(vkGetPhysicalDeviceFragmentShadingRatesKHR);
VULKAN_IDLOAD(vkCmdSetFragmentShadingRateKHR);
#if VK_HEADER_VERSION >= 276
VULKAN_IDLOAD(vkCmdSetRenderingAttachmentLocationsKHR);
VULKAN_IDLOAD(vkCmdSetRenderingInputAttachmentIndicesKHR);
#endif
#if VK_HEADER_VERSION >= 185
VULKAN_IDLOAD(vkWaitForPresentKHR);
#endif
VULKAN_IDLOAD(vkGetBufferDeviceAddressKHR);
VULKAN_IDLOAD(vkGetBufferOpaqueCaptureAddressKHR);
VULKAN_IDLOAD(vkGetDeviceMemoryOpaqueCaptureAddressKHR);
VULKAN_IDLOAD(vkCreateDeferredOperationKHR);
VULKAN_IDLOAD(vkDestroyDeferredOperationKHR);
VULKAN_IDLOAD(vkGetDeferredOperationMaxConcurrencyKHR);
VULKAN_IDLOAD(vkGetDeferredOperationResultKHR);
VULKAN_IDLOAD(vkDeferredOperationJoinKHR);
VULKAN_IDLOAD(vkGetPipelineExecutablePropertiesKHR);
VULKAN_IDLOAD(vkGetPipelineExecutableStatisticsKHR);
VULKAN_IDLOAD(vkGetPipelineExecutableInternalRepresentationsKHR);
#if VK_HEADER_VERSION >= 244
VULKAN_IDLOAD(vkMapMemory2KHR);
VULKAN_IDLOAD(vkUnmapMemory2KHR);
#endif
#if VK_HEADER_VERSION >= 274
VULKAN_IDLOAD(vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR);
VULKAN_IDLOAD(vkGetEncodedVideoSessionParametersKHR);
VULKAN_IDLOAD(vkCmdEncodeVideoKHR);
#endif
VULKAN_IDLOAD(vkCmdSetEvent2KHR);
VULKAN_IDLOAD(vkCmdResetEvent2KHR);
VULKAN_IDLOAD(vkCmdWaitEvents2KHR);
VULKAN_IDLOAD(vkCmdPipelineBarrier2KHR);
VULKAN_IDLOAD(vkCmdWriteTimestamp2KHR);
VULKAN_IDLOAD(vkQueueSubmit2KHR);
VULKAN_IDLOAD(vkCmdWriteBufferMarker2AMD);
VULKAN_IDLOAD(vkGetQueueCheckpointData2NV);
VULKAN_IDLOAD(vkCmdCopyBuffer2KHR);
VULKAN_IDLOAD(vkCmdCopyImage2KHR);
VULKAN_IDLOAD(vkCmdCopyBufferToImage2KHR);
VULKAN_IDLOAD(vkCmdCopyImageToBuffer2KHR);
VULKAN_IDLOAD(vkCmdBlitImage2KHR);
VULKAN_IDLOAD(vkCmdResolveImage2KHR);
#if VK_HEADER_VERSION >= 213
VULKAN_IDLOAD(vkCmdTraceRaysIndirect2KHR);
#endif
#if VK_HEADER_VERSION >= 195
VULKAN_IDLOAD(vkGetDeviceBufferMemoryRequirementsKHR);
VULKAN_IDLOAD(vkGetDeviceImageMemoryRequirementsKHR);
VULKAN_IDLOAD(vkGetDeviceImageSparseMemoryRequirementsKHR);
#endif
#if VK_HEADER_VERSION >= 260
VULKAN_IDLOAD(vkCmdBindIndexBuffer2KHR);
VULKAN_IDLOAD(vkGetRenderingAreaGranularityKHR);
VULKAN_IDLOAD(vkGetDeviceImageSubresourceLayoutKHR);
VULKAN_IDLOAD(vkGetImageSubresourceLayout2KHR);
#endif
#if VK_HEADER_VERSION >= 255
VULKAN_IDLOAD(vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR);
#endif
#if VK_HEADER_VERSION >= 276
VULKAN_IDLOAD(vkCmdSetLineStippleKHR);
#endif
#if VK_HEADER_VERSION >= 273
VULKAN_IDLOAD(vkGetPhysicalDeviceCalibrateableTimeDomainsKHR);
VULKAN_IDLOAD(vkGetCalibratedTimestampsKHR);
#endif
#if VK_HEADER_VERSION >= 274
VULKAN_IDLOAD(vkCmdBindDescriptorSets2KHR);
VULKAN_IDLOAD(vkCmdPushConstants2KHR);
VULKAN_IDLOAD(vkCmdPushDescriptorSet2KHR);
VULKAN_IDLOAD(vkCmdPushDescriptorSetWithTemplate2KHR);
VULKAN_IDLOAD(vkCmdSetDescriptorBufferOffsets2EXT);
VULKAN_IDLOAD(vkCmdBindDescriptorBufferEmbeddedSamplers2EXT);
#endif
VULKAN_IDLOAD(vkCreateDebugReportCallbackEXT);
VULKAN_IDLOAD(vkDestroyDebugReportCallbackEXT);
VULKAN_IDLOAD(vkDebugReportMessageEXT);
VULKAN_IDLOAD(vkDebugMarkerSetObjectTagEXT);
VULKAN_IDLOAD(vkDebugMarkerSetObjectNameEXT);
VULKAN_IDLOAD(vkCmdDebugMarkerBeginEXT);
VULKAN_IDLOAD(vkCmdDebugMarkerEndEXT);
VULKAN_IDLOAD(vkCmdDebugMarkerInsertEXT);
VULKAN_IDLOAD(vkCmdBindTransformFeedbackBuffersEXT);
VULKAN_IDLOAD(vkCmdBeginTransformFeedbackEXT);
VULKAN_IDLOAD(vkCmdEndTransformFeedbackEXT);
VULKAN_IDLOAD(vkCmdBeginQueryIndexedEXT);
VULKAN_IDLOAD(vkCmdEndQueryIndexedEXT);
VULKAN_IDLOAD(vkCmdDrawIndirectByteCountEXT);
VULKAN_IDLOAD(vkCreateCuModuleNVX);
VULKAN_IDLOAD(vkCreateCuFunctionNVX);
VULKAN_IDLOAD(vkDestroyCuModuleNVX);
VULKAN_IDLOAD(vkDestroyCuFunctionNVX);
VULKAN_IDLOAD(vkCmdCuLaunchKernelNVX);
VULKAN_IDLOAD(vkGetImageViewHandleNVX);
VULKAN_IDLOAD(vkGetImageViewAddressNVX);
VULKAN_IDLOAD(vkCmdDrawIndirectCountAMD);
VULKAN_IDLOAD(vkCmdDrawIndexedIndirectCountAMD);
VULKAN_IDLOAD(vkGetShaderInfoAMD);
VULKAN_IDLOAD(vkGetPhysicalDeviceExternalImageFormatPropertiesNV);
VULKAN_IDLOAD(vkCmdBeginConditionalRenderingEXT);
VULKAN_IDLOAD(vkCmdEndConditionalRenderingEXT);
VULKAN_IDLOAD(vkCmdSetViewportWScalingNV);
VULKAN_IDLOAD(vkReleaseDisplayEXT);
VULKAN_IDLOAD(vkGetPhysicalDeviceSurfaceCapabilities2EXT);
VULKAN_IDLOAD(vkDisplayPowerControlEXT);
VULKAN_IDLOAD(vkRegisterDeviceEventEXT);
VULKAN_IDLOAD(vkRegisterDisplayEventEXT);
VULKAN_IDLOAD(vkGetSwapchainCounterEXT);
VULKAN_IDLOAD(vkGetRefreshCycleDurationGOOGLE);
VULKAN_IDLOAD(vkGetPastPresentationTimingGOOGLE);
VULKAN_IDLOAD(vkCmdSetDiscardRectangleEXT);
#if VK_HEADER_VERSION >= 241
VULKAN_IDLOAD(vkCmdSetDiscardRectangleEnableEXT);
VULKAN_IDLOAD(vkCmdSetDiscardRectangleModeEXT);
#endif
VULKAN_IDLOAD(vkSetHdrMetadataEXT);
VULKAN_IDLOAD(vkSetDebugUtilsObjectNameEXT);
VULKAN_IDLOAD(vkSetDebugUtilsObjectTagEXT);
VULKAN_IDLOAD(vkQueueBeginDebugUtilsLabelEXT);
VULKAN_IDLOAD(vkQueueEndDebugUtilsLabelEXT);
VULKAN_IDLOAD(vkQueueInsertDebugUtilsLabelEXT);
VULKAN_IDLOAD(vkCmdBeginDebugUtilsLabelEXT);
VULKAN_IDLOAD(vkCmdEndDebugUtilsLabelEXT);
VULKAN_IDLOAD(vkCmdInsertDebugUtilsLabelEXT);
VULKAN_IDLOAD(vkCreateDebugUtilsMessengerEXT);
VULKAN_IDLOAD(vkDestroyDebugUtilsMessengerEXT);
VULKAN_IDLOAD(vkSubmitDebugUtilsMessageEXT);
VULKAN_IDLOAD(vkCmdSetSampleLocationsEXT);
VULKAN_IDLOAD(vkGetPhysicalDeviceMultisamplePropertiesEXT);
VULKAN_IDLOAD(vkGetImageDrmFormatModifierPropertiesEXT);
VULKAN_IDLOAD(vkCreateValidationCacheEXT);
VULKAN_IDLOAD(vkDestroyValidationCacheEXT);
VULKAN_IDLOAD(vkMergeValidationCachesEXT);
VULKAN_IDLOAD(vkGetValidationCacheDataEXT);
VULKAN_IDLOAD(vkCmdBindShadingRateImageNV);
VULKAN_IDLOAD(vkCmdSetViewportShadingRatePaletteNV);
VULKAN_IDLOAD(vkCmdSetCoarseSampleOrderNV);
VULKAN_IDLOAD(vkCreateAccelerationStructureNV);
VULKAN_IDLOAD(vkDestroyAccelerationStructureNV);
VULKAN_IDLOAD(vkGetAccelerationStructureMemoryRequirementsNV);
VULKAN_IDLOAD(vkBindAccelerationStructureMemoryNV);
VULKAN_IDLOAD(vkCmdBuildAccelerationStructureNV);
VULKAN_IDLOAD(vkCmdCopyAccelerationStructureNV);
VULKAN_IDLOAD(vkCmdTraceRaysNV);
VULKAN_IDLOAD(vkCreateRayTracingPipelinesNV);
VULKAN_IDLOAD(vkGetRayTracingShaderGroupHandlesKHR);
VULKAN_IDLOAD(vkGetRayTracingShaderGroupHandlesNV);
VULKAN_IDLOAD(vkGetAccelerationStructureHandleNV);
VULKAN_IDLOAD(vkCmdWriteAccelerationStructuresPropertiesNV);
VULKAN_IDLOAD(vkCompileDeferredNV);
VULKAN_IDLOAD(vkGetMemoryHostPointerPropertiesEXT);
VULKAN_IDLOAD(vkCmdWriteBufferMarkerAMD);
VULKAN_IDLOAD(vkGetPhysicalDeviceCalibrateableTimeDomainsEXT);
VULKAN_IDLOAD(vkGetCalibratedTimestampsEXT);
VULKAN_IDLOAD(vkCmdDrawMeshTasksNV);
VULKAN_IDLOAD(vkCmdDrawMeshTasksIndirectNV);
VULKAN_IDLOAD(vkCmdDrawMeshTasksIndirectCountNV);
#if VK_HEADER_VERSION >= 241
VULKAN_IDLOAD(vkCmdSetExclusiveScissorEnableNV);
#endif
VULKAN_IDLOAD(vkCmdSetExclusiveScissorNV);
VULKAN_IDLOAD(vkCmdSetCheckpointNV);
VULKAN_IDLOAD(vkGetQueueCheckpointDataNV);
VULKAN_IDLOAD(vkInitializePerformanceApiINTEL);
VULKAN_IDLOAD(vkUninitializePerformanceApiINTEL);
VULKAN_IDLOAD(vkCmdSetPerformanceMarkerINTEL);
VULKAN_IDLOAD(vkCmdSetPerformanceStreamMarkerINTEL);
VULKAN_IDLOAD(vkCmdSetPerformanceOverrideINTEL);
VULKAN_IDLOAD(vkAcquirePerformanceConfigurationINTEL);
VULKAN_IDLOAD(vkReleasePerformanceConfigurationINTEL);
VULKAN_IDLOAD(vkQueueSetPerformanceConfigurationINTEL);
VULKAN_IDLOAD(vkGetPerformanceParameterINTEL);
VULKAN_IDLOAD(vkSetLocalDimmingAMD);
VULKAN_IDLOAD(vkGetBufferDeviceAddressEXT);
VULKAN_IDLOAD(vkGetPhysicalDeviceToolPropertiesEXT);
VULKAN_IDLOAD(vkGetPhysicalDeviceCooperativeMatrixPropertiesNV);
VULKAN_IDLOAD(vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV);
VULKAN_IDLOAD(vkCreateHeadlessSurfaceEXT);
VULKAN_IDLOAD(vkCmdSetLineStippleEXT);
VULKAN_IDLOAD(vkResetQueryPoolEXT);
VULKAN_IDLOAD(vkCmdSetCullModeEXT);
VULKAN_IDLOAD(vkCmdSetFrontFaceEXT);
VULKAN_IDLOAD(vkCmdSetPrimitiveTopologyEXT);
VULKAN_IDLOAD(vkCmdSetViewportWithCountEXT);
VULKAN_IDLOAD(vkCmdSetScissorWithCountEXT);
VULKAN_IDLOAD(vkCmdBindVertexBuffers2EXT);
VULKAN_IDLOAD(vkCmdSetDepthTestEnableEXT);
VULKAN_IDLOAD(vkCmdSetDepthWriteEnableEXT);
VULKAN_IDLOAD(vkCmdSetDepthCompareOpEXT);
VULKAN_IDLOAD(vkCmdSetDepthBoundsTestEnableEXT);
VULKAN_IDLOAD(vkCmdSetStencilTestEnableEXT);
VULKAN_IDLOAD(vkCmdSetStencilOpEXT);
#if VK_HEADER_VERSION >= 258
VULKAN_IDLOAD(vkCopyMemoryToImageEXT);
VULKAN_IDLOAD(vkCopyImageToMemoryEXT);
VULKAN_IDLOAD(vkCopyImageToImageEXT);
VULKAN_IDLOAD(vkTransitionImageLayoutEXT);
#endif
#if VK_HEADER_VERSION >= 213
VULKAN_IDLOAD(vkGetImageSubresourceLayout2EXT);
#endif
#if VK_HEADER_VERSION >= 237
VULKAN_IDLOAD(vkReleaseSwapchainImagesEXT);
#endif
VULKAN_IDLOAD(vkGetGeneratedCommandsMemoryRequirementsNV);
VULKAN_IDLOAD(vkCmdPreprocessGeneratedCommandsNV);
VULKAN_IDLOAD(vkCmdExecuteGeneratedCommandsNV);
VULKAN_IDLOAD(vkCmdBindPipelineShaderGroupNV);
VULKAN_IDLOAD(vkCreateIndirectCommandsLayoutNV);
VULKAN_IDLOAD(vkDestroyIndirectCommandsLayoutNV);
#if VK_HEADER_VERSION >= 254
VULKAN_IDLOAD(vkCmdSetDepthBias2EXT);
#endif
VULKAN_IDLOAD(vkAcquireDrmDisplayEXT);
VULKAN_IDLOAD(vkGetDrmDisplayEXT);
VULKAN_IDLOAD(vkCreatePrivateDataSlotEXT);
VULKAN_IDLOAD(vkDestroyPrivateDataSlotEXT);
VULKAN_IDLOAD(vkSetPrivateDataEXT);
VULKAN_IDLOAD(vkGetPrivateDataEXT);
#if VK_HEADER_VERSION >= 269
VULKAN_IDLOAD(vkCreateCudaModuleNV);
VULKAN_IDLOAD(vkGetCudaModuleCacheNV);
VULKAN_IDLOAD(vkCreateCudaFunctionNV);
VULKAN_IDLOAD(vkDestroyCudaModuleNV);
VULKAN_IDLOAD(vkDestroyCudaFunctionNV);
VULKAN_IDLOAD(vkCmdCudaLaunchKernelNV);
#endif
#if VK_HEADER_VERSION >= 235
VULKAN_IDLOAD(vkGetDescriptorSetLayoutSizeEXT);
VULKAN_IDLOAD(vkGetDescriptorSetLayoutBindingOffsetEXT);
VULKAN_IDLOAD(vkGetDescriptorEXT);
VULKAN_IDLOAD(vkCmdBindDescriptorBuffersEXT);
VULKAN_IDLOAD(vkCmdSetDescriptorBufferOffsetsEXT);
VULKAN_IDLOAD(vkCmdBindDescriptorBufferEmbeddedSamplersEXT);
VULKAN_IDLOAD(vkGetBufferOpaqueCaptureDescriptorDataEXT);
VULKAN_IDLOAD(vkGetImageOpaqueCaptureDescriptorDataEXT);
VULKAN_IDLOAD(vkGetImageViewOpaqueCaptureDescriptorDataEXT);
VULKAN_IDLOAD(vkGetSamplerOpaqueCaptureDescriptorDataEXT);
VULKAN_IDLOAD(vkGetAccelerationStructureOpaqueCaptureDescriptorDataEXT);
#endif
VULKAN_IDLOAD(vkCmdSetFragmentShadingRateEnumNV);
#if VK_HEADER_VERSION >= 230
VULKAN_IDLOAD(vkGetDeviceFaultInfoEXT);
#endif
VULKAN_IDLOAD(vkCmdSetVertexInputEXT);
#if VK_HEADER_VERSION >= 184
VULKAN_IDLOAD(vkGetDeviceSubpassShadingMaxWorkgroupSizeHUAWEI);
#else
VULKAN_IDLOAD(vkGetSubpassShadingMaxWorkgroupSizeHUAWEI);
#endif
VULKAN_IDLOAD(vkCmdSubpassShadingHUAWEI);
#if VK_HEADER_VERSION >= 185
VULKAN_IDLOAD(vkCmdBindInvocationMaskHUAWEI);
#endif
#if VK_HEADER_VERSION >= 184
VULKAN_IDLOAD(vkGetMemoryRemoteAddressNV);
#endif
#if VK_HEADER_VERSION >= 213
VULKAN_IDLOAD(vkGetPipelinePropertiesEXT);
#endif
VULKAN_IDLOAD(vkCmdSetPatchControlPointsEXT);
VULKAN_IDLOAD(vkCmdSetRasterizerDiscardEnableEXT);
VULKAN_IDLOAD(vkCmdSetDepthBiasEnableEXT);
VULKAN_IDLOAD(vkCmdSetLogicOpEXT);
VULKAN_IDLOAD(vkCmdSetPrimitiveRestartEnableEXT);
VULKAN_IDLOAD(vkCmdSetColorWriteEnableEXT);
VULKAN_IDLOAD(vkCmdDrawMultiEXT);
VULKAN_IDLOAD(vkCmdDrawMultiIndexedEXT);
#if VK_HEADER_VERSION >= 230
VULKAN_IDLOAD(vkCreateMicromapEXT);
VULKAN_IDLOAD(vkDestroyMicromapEXT);
VULKAN_IDLOAD(vkCmdBuildMicromapsEXT);
VULKAN_IDLOAD(vkBuildMicromapsEXT);
VULKAN_IDLOAD(vkCopyMicromapEXT);
VULKAN_IDLOAD(vkCopyMicromapToMemoryEXT);
VULKAN_IDLOAD(vkCopyMemoryToMicromapEXT);
VULKAN_IDLOAD(vkWriteMicromapsPropertiesEXT);
VULKAN_IDLOAD(vkCmdCopyMicromapEXT);
VULKAN_IDLOAD(vkCmdCopyMicromapToMemoryEXT);
VULKAN_IDLOAD(vkCmdCopyMemoryToMicromapEXT);
VULKAN_IDLOAD(vkCmdWriteMicromapsPropertiesEXT);
VULKAN_IDLOAD(vkGetDeviceMicromapCompatibilityEXT);
VULKAN_IDLOAD(vkGetMicromapBuildSizesEXT);
#endif
#if VK_HEADER_VERSION >= 239
VULKAN_IDLOAD(vkCmdDrawClusterHUAWEI);
VULKAN_IDLOAD(vkCmdDrawClusterIndirectHUAWEI);
#endif
#if VK_HEADER_VERSION >= 191
VULKAN_IDLOAD(vkSetDeviceMemoryPriorityEXT);
#endif
#if VK_HEADER_VERSION >= 207
VULKAN_IDLOAD(vkGetDescriptorSetLayoutHostMappingInfoVALVE);
VULKAN_IDLOAD(vkGetDescriptorSetHostMappingVALVE);
#endif
#if VK_HEADER_VERSION >= 233
VULKAN_IDLOAD(vkCmdCopyMemoryIndirectNV);
VULKAN_IDLOAD(vkCmdCopyMemoryToImageIndirectNV);
VULKAN_IDLOAD(vkCmdDecompressMemoryNV);
VULKAN_IDLOAD(vkCmdDecompressMemoryIndirectCountNV);
#endif
#if VK_HEADER_VERSION >= 258
VULKAN_IDLOAD(vkGetPipelineIndirectMemoryRequirementsNV);
#endif
#if VK_HEADER_VERSION == 258
VULKAN_IDLOAD(vkCmdUpdatePipelineIndirectBuffer);
#endif
#if VK_HEADER_VERSION >= 259
VULKAN_IDLOAD(vkCmdUpdatePipelineIndirectBufferNV);
#endif
#if VK_HEADER_VERSION >= 258
VULKAN_IDLOAD(vkGetPipelineIndirectDeviceAddressNV);
#endif
#if VK_HEADER_VERSION >= 230
VULKAN_IDLOAD(vkCmdSetDepthClampEnableEXT);
VULKAN_IDLOAD(vkCmdSetPolygonModeEXT);
VULKAN_IDLOAD(vkCmdSetRasterizationSamplesEXT);
VULKAN_IDLOAD(vkCmdSetSampleMaskEXT);
VULKAN_IDLOAD(vkCmdSetAlphaToCoverageEnableEXT);
VULKAN_IDLOAD(vkCmdSetAlphaToOneEnableEXT);
VULKAN_IDLOAD(vkCmdSetLogicOpEnableEXT);
VULKAN_IDLOAD(vkCmdSetColorBlendEnableEXT);
VULKAN_IDLOAD(vkCmdSetColorBlendEquationEXT);
VULKAN_IDLOAD(vkCmdSetColorWriteMaskEXT);
VULKAN_IDLOAD(vkCmdSetTessellationDomainOriginEXT);
VULKAN_IDLOAD(vkCmdSetRasterizationStreamEXT);
VULKAN_IDLOAD(vkCmdSetConservativeRasterizationModeEXT);
VULKAN_IDLOAD(vkCmdSetExtraPrimitiveOverestimationSizeEXT);
VULKAN_IDLOAD(vkCmdSetDepthClipEnableEXT);
VULKAN_IDLOAD(vkCmdSetSampleLocationsEnableEXT);
VULKAN_IDLOAD(vkCmdSetColorBlendAdvancedEXT);
VULKAN_IDLOAD(vkCmdSetProvokingVertexModeEXT);
VULKAN_IDLOAD(vkCmdSetLineRasterizationModeEXT);
VULKAN_IDLOAD(vkCmdSetLineStippleEnableEXT);
VULKAN_IDLOAD(vkCmdSetDepthClipNegativeOneToOneEXT);
VULKAN_IDLOAD(vkCmdSetViewportWScalingEnableNV);
VULKAN_IDLOAD(vkCmdSetViewportSwizzleNV);
VULKAN_IDLOAD(vkCmdSetCoverageToColorEnableNV);
VULKAN_IDLOAD(vkCmdSetCoverageToColorLocationNV);
VULKAN_IDLOAD(vkCmdSetCoverageModulationModeNV);
VULKAN_IDLOAD(vkCmdSetCoverageModulationTableEnableNV);
VULKAN_IDLOAD(vkCmdSetCoverageModulationTableNV);
VULKAN_IDLOAD(vkCmdSetShadingRateImageEnableNV);
VULKAN_IDLOAD(vkCmdSetRepresentativeFragmentTestEnableNV);
VULKAN_IDLOAD(vkCmdSetCoverageReductionModeNV);
#endif
#if VK_HEADER_VERSION >= 219
VULKAN_IDLOAD(vkGetShaderModuleIdentifierEXT);
VULKAN_IDLOAD(vkGetShaderModuleCreateInfoIdentifierEXT);
#endif
#if VK_HEADER_VERSION >= 230
VULKAN_IDLOAD(vkGetPhysicalDeviceOpticalFlowImageFormatsNV);
VULKAN_IDLOAD(vkCreateOpticalFlowSessionNV);
VULKAN_IDLOAD(vkDestroyOpticalFlowSessionNV);
VULKAN_IDLOAD(vkBindOpticalFlowSessionImageNV);
VULKAN_IDLOAD(vkCmdOpticalFlowExecuteNV);
#endif
#if VK_HEADER_VERSION >= 246
VULKAN_IDLOAD(vkCreateShadersEXT);
VULKAN_IDLOAD(vkDestroyShaderEXT);
VULKAN_IDLOAD(vkGetShaderBinaryDataEXT);
VULKAN_IDLOAD(vkCmdBindShadersEXT);
#endif
#if VK_HEADER_VERSION >= 222
VULKAN_IDLOAD(vkGetFramebufferTilePropertiesQCOM);
VULKAN_IDLOAD(vkGetDynamicRenderingTilePropertiesQCOM);
#endif
#if VK_HEADER_VERSION >= 266
VULKAN_IDLOAD(vkSetLatencySleepModeNV);
VULKAN_IDLOAD(vkLatencySleepNV);
VULKAN_IDLOAD(vkSetLatencyMarkerNV);
VULKAN_IDLOAD(vkGetLatencyTimingsNV);
VULKAN_IDLOAD(vkQueueNotifyOutOfBandNV);
#endif
#if VK_HEADER_VERSION >= 250
VULKAN_IDLOAD(vkCmdSetAttachmentFeedbackLoopEnableEXT);
#endif
VULKAN_IDLOAD(vkCreateAccelerationStructureKHR);
VULKAN_IDLOAD(vkDestroyAccelerationStructureKHR);
VULKAN_IDLOAD(vkCmdBuildAccelerationStructuresKHR);
VULKAN_IDLOAD(vkCmdBuildAccelerationStructuresIndirectKHR);
VULKAN_IDLOAD(vkBuildAccelerationStructuresKHR);
VULKAN_IDLOAD(vkCopyAccelerationStructureKHR);
VULKAN_IDLOAD(vkCopyAccelerationStructureToMemoryKHR);
VULKAN_IDLOAD(vkCopyMemoryToAccelerationStructureKHR);
VULKAN_IDLOAD(vkWriteAccelerationStructuresPropertiesKHR);
VULKAN_IDLOAD(vkCmdCopyAccelerationStructureKHR);
VULKAN_IDLOAD(vkCmdCopyAccelerationStructureToMemoryKHR);
VULKAN_IDLOAD(vkCmdCopyMemoryToAccelerationStructureKHR);
VULKAN_IDLOAD(vkGetAccelerationStructureDeviceAddressKHR);
VULKAN_IDLOAD(vkCmdWriteAccelerationStructuresPropertiesKHR);
VULKAN_IDLOAD(vkGetDeviceAccelerationStructureCompatibilityKHR);
VULKAN_IDLOAD(vkGetAccelerationStructureBuildSizesKHR);
VULKAN_IDLOAD(vkCmdTraceRaysKHR);
VULKAN_IDLOAD(vkCreateRayTracingPipelinesKHR);
VULKAN_IDLOAD(vkGetRayTracingCaptureReplayShaderGroupHandlesKHR);
VULKAN_IDLOAD(vkCmdTraceRaysIndirectKHR);
VULKAN_IDLOAD(vkGetRayTracingShaderGroupStackSizeKHR);
VULKAN_IDLOAD(vkCmdSetRayTracingPipelineStackSizeKHR);
#if VK_HEADER_VERSION >= 226
VULKAN_IDLOAD(vkCmdDrawMeshTasksEXT);
VULKAN_IDLOAD(vkCmdDrawMeshTasksIndirectEXT);
VULKAN_IDLOAD(vkCmdDrawMeshTasksIndirectCountEXT);
#endif

// vim:ts=4:sw=4:noexpandtab
