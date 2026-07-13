#ifndef HYPE_CONSOLE_H
#define HYPE_CONSOLE_H

#include "efi_types.h"

/*
 * Pre-GOP/pre-serial console output (SETUP-6): a printf-equivalent over
 * UEFI ConOut, for M0's own output and as a fallback until the real
 * serial driver (M1-5) and GOP renderer (M1-6) exist.
 */
void hype_console_print(EFI_SYSTEM_TABLE *system_table, const char *fmt, ...);

/*
 * Widens ASCII into UEFI's CHAR16 console string, expanding '\n' to
 * "\r\n" (ConOut needs the explicit CR to return the cursor). Truncates
 * safely and NUL-terminates if out_count is insufficient. Returns the
 * number of CHAR16 units the untruncated result would need, excluding
 * the terminator. Pure logic, no UEFI calls -- split out from
 * hype_console_print purely so it's directly unit-testable.
 */
unsigned long long hype_console_widen(const char *ascii, CHAR16 *out, unsigned long long out_count);

#endif /* HYPE_CONSOLE_H */
