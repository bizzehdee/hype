#ifndef HYPE_FATAL_H
#define HYPE_FATAL_H

#include "gop_text.h"

/*
 * M1-7: the one panic handler, via M1-5 (serial, always available from
 * shortly after boot onward) and M1-6 (GOP, if a console was found and
 * registered here). Supersedes the transitional ConOut-based
 * hype_panic() this project used before serial/GOP existed -- ConOut
 * needs firmware in a good state, which isn't guaranteed once Boot
 * Services are gone (see arch/x86_64/cpu/isr_entry.c's history for why
 * that distinction is load-bearing, not cosmetic), whereas serial works
 * identically before and after ExitBootServices().
 */

/* Registers the GOP console hype_fatal() also prints to, if any (call
 * once GOP is initialized in boot/main.c; leave unset if none was
 * found). hype_fatal_get_gop() exists for testing the roundtrip. */
void hype_fatal_set_gop(hype_gop_console_t *con);
hype_gop_console_t *hype_fatal_get_gop(void);

/*
 * Formats fmt/... as "PANIC: <message>", prints it via serial and (if
 * registered) the GOP console, then halts forever. Never returns. Not
 * unit tested: it ends in the noreturn hype_halt_forever(), so calling
 * it in a test would hang the test binary rather than verify anything
 * -- same reasoning as hype_halt_forever() itself (halt.h).
 */
__attribute__((noreturn)) void hype_fatal(const char *fmt, ...);

#endif /* HYPE_FATAL_H */
