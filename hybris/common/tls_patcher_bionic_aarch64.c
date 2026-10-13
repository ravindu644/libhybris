/*
 * Stock bionic reads its own thread state through tpidr_el0: slot 1 is
 * pthread_internal_t (errno lives there) and slot -1 is bionic_tls (locale,
 * strerror buffers). Under glibc those words belong to glibc, so any bionic
 * function hybris does not hook crashes the first time it touches them, for
 * example __ctype_get_mb_cur_max from Android 14's libc++. Halium never hit
 * this because it ships a bionic built with hybris patches; we run the
 * device's stock /system.
 *
 * Fix: inside bionic itself, turn every "mrs xN, tpidr_el0" into
 * "adrp xN, fake_tcb", a page whose slot 1 and slot -1 point at zeroed
 * stand-ins. Everything else bionic reads from its TCB sees zeros.
 *
 * ponytail: one fake thread shared by every thread, so bionic-internal errno
 * and scratch buffers race between threads. Callers read errno through the
 * hooked __errno (glibc), so the race only loses values nobody reads. Per
 * thread state needs a slot in hybris static TLS, do that if a blob ever
 * depends on bionic_tls contents.
 */
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define BIONIC_FAKE_SIZE (64 * 1024)

static uint8_t bionic_fake_thread[BIONIC_FAKE_SIZE] __attribute__((aligned(64)));
static uint8_t bionic_fake_tls[BIONIC_FAKE_SIZE] __attribute__((aligned(64)));

/* Two pages, fake TCB at the start of the second, so slot -1 is the last word
 * of the first. adrp reaches only +-4 GiB, so map it near the library. */
static uintptr_t bionic_fake_tcb_near(uintptr_t pc)
{
    const uintptr_t span = 1UL << 31;
    for (uintptr_t hint = pc > span ? pc - span : 0x10000; hint < pc + span; hint += span / 8) {
        uint8_t *p = mmap((void *)hint, 2 * 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            continue;
        uintptr_t tcb = (uintptr_t)p + 4096;
        intptr_t d = (intptr_t)(tcb >> 12) - (intptr_t)(pc >> 12);
        if (d >= -(1L << 20) && d < (1L << 20)) {
            ((void **)tcb)[-1] = bionic_fake_tls;
            ((void **)tcb)[1] = bionic_fake_thread;
            return tcb;
        }
        munmap(p, 2 * 4096);
    }
    return 0;
}

static int is_bionic(const char *name)
{
    return !strcmp(name, "libc.so") || !strcmp(name, "libm.so") || !strcmp(name, "libdl.so");
}

static void hybris_patch_bionic_tp(void *segment_addr, size_t segment_size, const char *name)
{
    uint32_t *text = segment_addr;
    size_t count = segment_size / 4, patched = 0;
    uintptr_t tcb = bionic_fake_tcb_near((uintptr_t)segment_addr);

    if (!tcb) {
        HYBRIS_ERROR_LOG(HOOKS, "no fake TCB within adrp range of %s, bionic TLS left unpatched", name);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        /* mrs xN, tpidr_el0 is 0xd53bd040 | N */
        if ((text[i] & ~0x1fu) != 0xd53bd040u)
            continue;
        uintptr_t pc = (uintptr_t)&text[i];
        int64_t imm = (int64_t)(tcb >> 12) - (int64_t)(pc >> 12);
        text[i] = 0x90000000u | ((uint32_t)(imm & 3) << 29) |
                  ((uint32_t)((imm >> 2) & 0x7ffff) << 5) | (text[i] & 0x1f);
        patched++;
    }
    __builtin___clear_cache((char *)segment_addr, (char *)segment_addr + segment_size);
    HYBRIS_DEBUG_LOG(HOOKS, "redirected %zu tpidr_el0 reads in %s to fake TCB %p", patched, name, (void *)tcb);
}
