pkglib_LTLIBRARIES = vulkanplatform_wayland.la

# The linux-dmabuf buffer path (DrmWaylandBuffer) lives behind WANT_LINDROID_DRM in the
# shared window code. Compositors without android_wlegl (KWin 6) need it, so build it
# here the way egl/platforms/lindroid_drm does.
WAYLAND_DMABUF_XML = $(top_srcdir)/platforms/wayland/protocols/linux-dmabuf-unstable-v1.xml
BUILT_SOURCES = linux-dmabuf-unstable-v1-client-protocol.h linux-dmabuf-unstable-v1-protocol.c
CLEANFILES = $(BUILT_SOURCES)
linux-dmabuf-unstable-v1-client-protocol.h: $(WAYLAND_DMABUF_XML)
	$(WAYLAND_SCANNER) client-header $< $@
linux-dmabuf-unstable-v1-protocol.c: $(WAYLAND_DMABUF_XML)
	$(WAYLAND_SCANNER) private-code $< $@
nodist_vulkanplatform_wayland_la_SOURCES = linux-dmabuf-unstable-v1-protocol.c

vulkanplatform_wayland_la_SOURCES = vulkanplatform_wayland.cpp wayland_window.cpp $(top_srcdir)/platforms/wayland/wayland_window_common.cpp
vulkanplatform_wayland_la_CXXFLAGS = \
	-DWANT_LINDROID_DRM \
	-I$(builddir) \
	$(LIBDRM_CFLAGS) \
	$(GBM_CFLAGS) \
	-I$(top_srcdir)/common \
	-I$(top_srcdir)/platforms/common \
	-I$(top_srcdir)/platforms/wayland \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/vulkan \
	-I$(top_srcdir)/vulkan/platforms/common \
	-I$(top_builddir)/vulkan/platforms/common \
	$(ANDROID_HEADERS_CFLAGS) \
	$(WAYLAND_CLIENT_CFLAGS) \
	$(WAYLAND_EGL_CFLAGS)

if WANT_DEBUG
vulkanplatform_wayland_la_CXXFLAGS += -I$(top_srcdir)/common
endif

if WANT_TRACE
vulkanplatform_wayland_la_CXXFLAGS += -DDEBUG
endif

if WANT_DEBUG
vulkanplatform_wayland_la_CXXFLAGS += -ggdb -O0
endif

if !WANT_WL_SERVERSIDE_BUFFERS
vulkanplatform_wayland_la_CXXFLAGS += -DHYBRIS_NO_SERVER_SIDE_BUFFERS
endif

vulkanplatform_wayland_la_LDFLAGS = \
	-avoid-version -module -shared -export-dynamic \
	$(top_builddir)/platforms/common/libhybris-platformcommon.la \
	$(top_builddir)/vulkan/platforms/common/libhybris-vulkanplatformcommon.la \
	$(top_builddir)/gralloc/libgralloc.la \
	$(WAYLAND_CLIENT_LIBS) \
	$(LIBDRM_LIBS) \
	$(GBM_LIBS) \
	$(WAYLAND_EGL_LIBS)

if HAS_ANDROID_4_2_0
vulkanplatform_wayland_la_LDFLAGS += $(top_builddir)/libsync/libsync.la
endif

if HAS_ANDROID_5_0_0
vulkanplatform_wayland_la_LDFLAGS += $(top_builddir)/libsync/libsync.la
endif

