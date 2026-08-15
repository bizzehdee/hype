#include "fatal.h"

/* hype_fatal() itself is implemented in halt.c, alongside
 * hype_halt_forever() -- see halt.h and this file's own header for
 * why. */

static hype_gop_console_t *g_gop_console = 0;
static EFI_GRAPHICS_OUTPUT_PROTOCOL *g_gop_protocol = 0;
static void *g_gop_real_fb = 0;

/*
 * #380: the debug tee and the dashboard renderer run on different cores.
 * Plain loads and stores made the old enable flag a C data race. Keep the
 * renderer ownership state here, outside halt.c's hardware-only exemption,
 * so its synchronization and write accounting remain unit-testable.
 */
static unsigned int g_debug_gop_enabled = 1u;
static unsigned long long g_debug_gop_writes;

/* #461: see fatal.h. Written by a dying core, read by the living ones -- atomics, not plain
 * loads and stores, for the same reason the debug-tee flag above uses them. */
static unsigned int g_core_panic_count;
static unsigned int g_core_panic_apic;

void hype_fatal_note_core_panic(unsigned int apic_id) {
    __atomic_store_n(&g_core_panic_apic, apic_id, __ATOMIC_RELEASE);
    (void)__atomic_add_fetch(&g_core_panic_count, 1u, __ATOMIC_ACQ_REL);
}

unsigned int hype_fatal_core_panic_count(void) {
    return __atomic_load_n(&g_core_panic_count, __ATOMIC_ACQUIRE);
}

unsigned int hype_fatal_core_panic_apic(void) {
    return __atomic_load_n(&g_core_panic_apic, __ATOMIC_ACQUIRE);
}

void hype_fatal_set_gop(hype_gop_console_t *con) {
    g_gop_console = con;
}

hype_gop_console_t *hype_fatal_get_gop(void) {
    return g_gop_console;
}

void hype_fatal_set_gop_protocol(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, void *real_fb) {
    g_gop_protocol = gop;
    g_gop_real_fb = real_fb;
}

EFI_GRAPHICS_OUTPUT_PROTOCOL *hype_fatal_get_gop_protocol(void) {
    return g_gop_protocol;
}

void *hype_fatal_get_real_fb(void) {
    return g_gop_real_fb;
}

void hype_debug_set_gop_enabled(int enabled) {
    __atomic_store_n(&g_debug_gop_enabled, enabled ? 1u : 0u, __ATOMIC_RELEASE);
}

int hype_debug_gop_is_enabled(void) {
    return __atomic_load_n(&g_debug_gop_enabled, __ATOMIC_ACQUIRE) != 0u;
}

void hype_debug_note_gop_write(void) {
    (void)__atomic_fetch_add(&g_debug_gop_writes, 1ull, __ATOMIC_RELAXED);
}

unsigned long long hype_debug_gop_write_count(void) {
    return __atomic_load_n(&g_debug_gop_writes, __ATOMIC_RELAXED);
}

/* Optional hook run by hype_fatal() just before it halts, so a panic
 * mid-run still flushes the captured console log (core/logbuf.h) to disk
 * for real-hardware debugging. Registered by boot/main.c; NULL by default
 * (e.g. in unit tests) so hype_fatal() stays self-contained. */
static hype_flush_hook_t g_flush_hook = 0;

void hype_fatal_set_flush_hook(hype_flush_hook_t hook) {
    g_flush_hook = hook;
}

hype_flush_hook_t hype_fatal_get_flush_hook(void) {
    return g_flush_hook;
}
