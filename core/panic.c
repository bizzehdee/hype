#include "panic.h"
#include "console.h"

/* hype_panic() itself is implemented in halt.c, alongside
 * hype_halt_forever() -- see halt.h for why. */

void hype_panic_message(EFI_SYSTEM_TABLE *system_table, const char *msg) {
    hype_console_print(system_table, "PANIC: %s\n", msg);
}
