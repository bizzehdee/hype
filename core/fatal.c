#include "fatal.h"

/* hype_fatal() itself is implemented in halt.c, alongside
 * hype_halt_forever() -- see halt.h and this file's own header for
 * why. */

static hype_gop_console_t *g_gop_console = 0;
static EFI_GRAPHICS_OUTPUT_PROTOCOL *g_gop_protocol = 0;
static void *g_gop_real_fb = 0;

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
