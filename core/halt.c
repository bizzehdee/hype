#include "halt.h"
#include "panic.h"

__attribute__((noreturn)) void hype_halt_forever(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

/*
 * hype_panic() lives here, not in panic.c: it never returns (calling it
 * in a test would hang the test binary rather than verify anything), so
 * it's the exempt half of the panic path. hype_panic_message() in
 * panic.c holds all the actual decision logic and is fully unit tested.
 */
__attribute__((noreturn)) void hype_panic(EFI_SYSTEM_TABLE *system_table, const char *msg) {
    hype_panic_message(system_table, msg);
    hype_halt_forever();
}
