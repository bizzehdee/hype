#ifndef HYPE_FORMAT_H
#define HYPE_FORMAT_H

#include <stdarg.h>

/*
 * Freestanding, no-libc printf-equivalent core (SETUP-6). Supports
 * %s %c %d %u %x %p %% and the 64-bit variants %lld %llu %llx (no bare
 * %d/%u/%x is ever 64-bit -- always go through the ll length modifier
 * for addresses/page counts/anything else genuinely 64-bit). No field
 * widths/precision/flags -- add them only when a real caller needs them.
 *
 * Sizes are `unsigned long long`, not `unsigned long`: the UEFI target
 * (x86_64-unknown-uefi, PE/COFF, MS x64 ABI) is LLP64 -- `long` is only
 * 32 bits there, unlike on the Linux host this code's unit tests build
 * against. `unsigned long long` is 64 bits on both, so it's the type
 * that stays correct on either build.
 *
 * Writes at most bufsz-1 characters plus a NUL terminator into buf
 * (truncates safely if the formatted output doesn't fit). Returns the
 * number of characters that *would* have been written, excluding the
 * NUL, matching snprintf's return-value convention so callers can detect
 * truncation.
 */
int hype_vsnprintf(char *buf, unsigned long long bufsz, const char *fmt, va_list ap);
int hype_snprintf(char *buf, unsigned long long bufsz, const char *fmt, ...);

/*
 * #238: makes a truncated record VISIBLY truncated. When `written` (the
 * hype_vsnprintf return value) says the formatted output did not fit in
 * `bufsz`, the end of the buffer is overwritten with "...[TRUNCATED]\n" --
 * so a cut is never silent, and the record never loses its trailing
 * newline (an unterminated record makes the NEXT one merge onto the same
 * line, which is how #238's log lost its most information-dense lines).
 * Returns 1 if the marker was applied, 0 if the record fit (buffer
 * untouched). A buffer too small for even the marker is left alone.
 */
int hype_format_mark_truncated(char *buf, unsigned long long bufsz, int written);

#endif /* HYPE_FORMAT_H */
