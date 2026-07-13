#include "fatal.h"

/* hype_fatal() itself is implemented in halt.c, alongside
 * hype_halt_forever() -- see halt.h and this file's own header for
 * why. */

static hype_gop_console_t *g_gop_console = 0;

void hype_fatal_set_gop(hype_gop_console_t *con) {
    g_gop_console = con;
}

hype_gop_console_t *hype_fatal_get_gop(void) {
    return g_gop_console;
}
