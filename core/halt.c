#include <stdarg.h>

#include "fatal.h"
#include "format.h"
#include "gop.h"
#include "halt.h"
#include "logbuf.h"
#include "serial.h"

__attribute__((noreturn)) void hype_halt_forever(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void hype_wait_for_interrupt(void) {
    __asm__ volatile("hlt");
}

/*
 * hype_fatal() lives here, not in fatal.c: it never returns (calling it
 * in a test would hang the test binary rather than verify anything), so
 * it's the exempt half of the panic path -- same split as
 * gdt.h/gdt_load.c, idt.h/idt_load.c, etc. hype_fatal_set_gop()/
 * hype_fatal_get_gop() in fatal.c hold the only real state/logic here
 * and are fully unit tested.
 */
__attribute__((noreturn)) void hype_fatal(const char *fmt, ...) {
    char msg[192];
    va_list ap;
    hype_gop_console_t *gop;

    va_start(ap, fmt);
    hype_vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    hype_serial_print("PANIC: %s\n", msg);
    /* Capture the panic in the console log, then flush it to disk (if a
     * hook is registered) before halting -- so a mid-run panic on real
     * hardware still leaves \hype-log.txt ending with the cause. */
    hype_logbuf_append("PANIC: ");
    hype_logbuf_append(msg);
    hype_logbuf_append("\n");

    gop = hype_fatal_get_gop();
    if (gop != 0) {
        hype_gop_print(gop, "PANIC: %s\n", msg);
        hype_gop_flush(hype_fatal_get_gop_protocol(), gop, hype_fatal_get_real_fb());
    }

    {
        hype_flush_hook_t flush = hype_fatal_get_flush_hook();
        if (flush != 0) {
            flush();
        }
    }

    hype_halt_forever();
}

/*
 * RT-2c: GOP-flush deferral. When set, hype_debug_print() still renders text
 * into the console's shadow buffer (cheap RAM write, and the RT-1c dirty
 * range keeps accumulating), but does NOT push it to the real framebuffer --
 * the caller flushes on its own cadence via hype_debug_flush_gop(). On real
 * hardware the framebuffer is often uncached and a full-frame scroll memcpy
 * costs milliseconds; doing that per console line dominated the post-EBS
 * loop body. The FW-1 loop defers and flushes at ~60 Hz instead, so N lines
 * printed between flushes cost ONE framebuffer push, not N. hype_fatal()
 * flushes unconditionally (above), so a panic is never hidden by deferral.
 */
static int g_gop_deferred = 0;

void hype_debug_set_gop_deferred(int deferred) {
    g_gop_deferred = deferred;
}

void hype_debug_flush_gop(void) {
    hype_gop_console_t *gop = hype_fatal_get_gop();
    if (gop != 0) {
        hype_gop_flush(hype_fatal_get_gop_protocol(), gop, hype_fatal_get_real_fb());
    }
}

void hype_debug_print(const char *fmt, ...) {
    char msg[192];
    va_list ap;
    hype_gop_console_t *gop;

    va_start(ap, fmt);
    hype_vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    hype_serial_print("%s", msg);
    /* Tee into the in-memory capture so boot/main.c can flush the whole
     * console to a file on the boot volume before ExitBootServices --
     * the serial-less real-hardware debug path (core/logbuf.h). */
    hype_logbuf_append(msg);

    gop = hype_fatal_get_gop();
    if (gop != 0) {
        hype_gop_print(gop, "%s", msg);
        if (!g_gop_deferred) {
            hype_gop_flush(hype_fatal_get_gop_protocol(), gop, hype_fatal_get_real_fb());
        }
    }
}
