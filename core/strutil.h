#ifndef HYPE_STRUTIL_H
#define HYPE_STRUTIL_H

/* Freestanding, no-libc string helpers shared by the config parser and
 * later modules that need the same small primitives. */

unsigned long long hype_strlen(const char *s);
int hype_streq(const char *a, const char *b);
int hype_strneq(const char *a, const char *b, unsigned long long n);

/* Copies at most dst_size-1 chars from src plus a NUL terminator into
 * dst. Returns the length of src (untruncated), snprintf-style, so
 * callers can detect truncation. dst_size==0 is a safe no-op. */
unsigned long long hype_strlcpy(char *dst, const char *src, unsigned long long dst_size);

/* True if c is an ASCII decimal digit / whitespace, respectively. */
int hype_is_digit(char c);
int hype_is_space(char c);

/* Parses an unsigned decimal integer from a NUL-terminated string.
 * Requires the *entire* string (after any leading/trailing whitespace)
 * to be consumed as digits -- no partial-parse success, no sign, no
 * leading "0x". Returns 0 and stores the value in *out on success,
 * non-zero on empty/invalid input or overflow. */
int hype_parse_uint(const char *s, unsigned long long *out);

/* Returns a pointer to the first non-whitespace character, and writes a
 * NUL over the first trailing whitespace run's start (in place),
 * trimming both ends of a mutable string. */
char *hype_str_trim(char *s);

#endif /* HYPE_STRUTIL_H */
