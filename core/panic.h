#ifndef HYPE_PANIC_H
#define HYPE_PANIC_H

#include "efi_types.h"

/*
 * Formats and prints "PANIC: <msg>\n" via ConOut. All of the panic
 * path's actual decision logic lives here so it stays unit-testable
 * (mock ConOut, same technique as console.c's tests) -- see halt.h for
 * why hype_panic() itself is not.
 */
void hype_panic_message(EFI_SYSTEM_TABLE *system_table, const char *msg);

/*
 * Prints the panic message, then halts cleanly forever. Never returns.
 */
__attribute__((noreturn)) void hype_panic(EFI_SYSTEM_TABLE *system_table, const char *msg);

#endif /* HYPE_PANIC_H */
