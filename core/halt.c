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
    static volatile int in_panic = 0;
    char msg[192];
    va_list ap;
    hype_gop_console_t *gop;

    /*
     * A panic must survive faulting inside its own handler.
     *
     * Everything below this point can fault: the GOP paint writes to a
     * framebuffer that may be exactly what is broken, and the flush hook calls
     * firmware. Any such fault lands in the ISR, which calls hype_fatal()
     * again, which paints again... An Intel i5-13420H showed the full shape of
     * it: one real GP fault, then ~120 identical page faults at the same rip
     * with interrupts already masked, terminating in a Double Fault as the
     * stack ran out. The genuine first cause scrolled off the screen and ate
     * the whole 16KB NV tail, so every diagnostic channel returned noise --
     * which is precisely when a panic handler has to be at its most boring.
     *
     * Second and later entrants therefore say nothing and halt: the first
     * panic's message is already out on serial + logbuf, and that is the one
     * that matters. Interrupts are masked first so nothing interleaves.
     */
    __asm__ volatile("cli"); /* inline, like hype_halt_forever's hlt -- keeps the
                              * panic path free of any dependency that could
                              * itself be the thing that is broken. */
    if (in_panic) {
        hype_halt_forever();
    }
    in_panic = 1;

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

    /*
     * Persist BEFORE painting. Ordering here is load-bearing, and getting it
     * wrong cost two real-hardware boots.
     *
     * The GOP paint is the least reliable step in this function -- it is MMIO to
     * a framebuffer that may be exactly what is broken -- and the flush hook is
     * the most valuable, being the only thing that survives to the next boot on
     * a machine with no serial port. With the paint first, a faulting paint took
     * the hook down with it: the nested fault re-enters, sees the latch above,
     * and halts, so the tail was never written. (Before the latch existed the
     * storm accidentally papered over this -- of ~120 re-entries, some got past
     * the paint and did reach the hook, which is the only reason earlier
     * captures existed at all. Adding the latch removed that accident and with
     * it the capture, which is how this was found.)
     *
     * Cheapest and safest first (serial + logbuf, already done above), then the
     * hook, then the paint as a best-effort extra.
     */
    {
        hype_flush_hook_t flush = hype_fatal_get_flush_hook();
        if (flush != 0) {
            flush();
        }
    }

    gop = hype_fatal_get_gop();
    if (gop != 0) {
        hype_gop_print(gop, "PANIC: %s\n", msg);
        hype_gop_flush(hype_fatal_get_gop_protocol(), gop, hype_fatal_get_real_fb());
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

/* Rendering isolation: once the guest dispatch loop's terminal/dashboard renderer
 * owns the GOP framebuffer, hype_debug_print() (used from EVERY core to relay each
 * VM's serial console + hype diagnostics to the log) must NOT also paint the shared
 * GOP shadow -- otherwise one VM's output bleeds onto the focused view/dashboard for
 * a frame (and races the shadow across cores). When disabled, prints still go to the
 * serial port + logbuf (\HYPEFULL.LOG); only the GOP tee is suppressed. hype_fatal()
 * paints the GOP directly, so panics are never suppressed. */
static int g_gop_enabled = 1;

void hype_debug_set_gop_deferred(int deferred) {
    g_gop_deferred = deferred;
}

void hype_debug_set_gop_enabled(int enabled) {
    g_gop_enabled = enabled;
}

void hype_debug_flush_gop(void) {
    hype_gop_console_t *gop = hype_fatal_get_gop();
    if (gop != 0) {
        hype_gop_flush(hype_fatal_get_gop_protocol(), gop, hype_fatal_get_real_fb());
    }
}

void hype_debug_print(const char *fmt, ...) {
    /*
     * #238 mechanism 1: at 192 this cut every record over 191 chars
     * mid-sentence AND ate its trailing newline, so the next record merged
     * onto the same line -- 14 such merges in one real-hardware run, always
     * on the most information-dense lines (the microtests' byte-for-byte
     * verification messages, TIMERHIST/INTDIAG). The longest real record
     * measured is 272 chars; 512 doubles that. Raised HERE ALONE, per the
     * ticket: an earlier attempt that raised three buffers at once (plus a
     * serial tee) produced a 0-byte \HYPEFULL.LOG on real AMD hardware and
     * never reproduced under QEMU, so each site gets its own
     * hardware-validated step. Anything still longer is marked, never
     * silently cut (hype_format_mark_truncated).
     */
    char msg[512];
    int n;
    va_list ap;
    hype_gop_console_t *gop;

    va_start(ap, fmt);
    n = hype_vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    (void)hype_format_mark_truncated(msg, sizeof(msg), n);

    hype_serial_print("%s", msg);
    /* Tee into the in-memory capture so boot/main.c can flush the whole
     * console to a file on the boot volume before ExitBootServices --
     * the serial-less real-hardware debug path (core/logbuf.h). */
    hype_logbuf_append(msg);

    gop = hype_fatal_get_gop();
    if (g_gop_enabled && gop != 0) {
        hype_gop_print(gop, "%s", msg);
        if (!g_gop_deferred) {
            hype_gop_flush(hype_fatal_get_gop_protocol(), gop, hype_fatal_get_real_fb());
        }
    }
}
