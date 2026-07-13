#include <stdarg.h>

#include "fatal.h"
#include "format.h"
#include "halt.h"
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

    gop = hype_fatal_get_gop();
    if (gop != 0) {
        hype_gop_print(gop, "PANIC: %s\n", msg);
    }

    hype_halt_forever();
}
