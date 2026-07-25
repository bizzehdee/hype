#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../serial.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), (long long)(actual)); \
            failures++; \
        } \
    } while (0)

#define CHECK_STR(desc, expected, actual) \
    do { \
        if (strcmp((expected), (actual)) != 0) { \
            printf("FAIL: %s: expected \"%s\", got \"%s\"\n", (desc), (expected), (actual)); \
            failures++; \
        } \
    } while (0)

static void test_divisor_for_baud(void) {
    CHECK_INT("115200 baud -> divisor 1", 1, hype_serial_divisor_for_baud(115200));
    CHECK_INT("9600 baud -> divisor 12", 12, hype_serial_divisor_for_baud(9600));
    CHECK_INT("38400 baud -> divisor 3", 3, hype_serial_divisor_for_baud(38400));
    CHECK_INT("0 baud is invalid", 0, hype_serial_divisor_for_baud(0));
    CHECK_INT("1 baud would need a divisor > 65535, invalid", 0, hype_serial_divisor_for_baud(1));
    CHECK_INT("baud faster than 115200 rounds the divisor to 0, invalid",
              0, hype_serial_divisor_for_baud(200000));
}

static char g_captured[512];
static unsigned long long g_captured_len;

static void reset_capture(void) {
    g_captured[0] = '\0';
    g_captured_len = 0;
}

static void mock_putc(char c) {
    if (g_captured_len + 1 < sizeof(g_captured)) {
        g_captured[g_captured_len] = c;
        g_captured_len++;
        g_captured[g_captured_len] = '\0';
    }
}

static void test_write_via_plain(void) {
    reset_capture();
    hype_serial_write_via(mock_putc, "hype");
    CHECK_STR("write_via plain string", "hype", g_captured);
}

static void test_write_via_expands_newline(void) {
    reset_capture();
    hype_serial_write_via(mock_putc, "a\nb");
    CHECK_STR("write_via expands \\n to \\r\\n", "a\r\nb", g_captured);
}

static void test_write_via_empty(void) {
    reset_capture();
    hype_serial_write_via(mock_putc, "");
    CHECK_STR("write_via empty string writes nothing", "", g_captured);
}

static void test_print_via(void) {
    reset_capture();
    hype_serial_print_via(mock_putc, "%s=%d\n", "x", 7);
    CHECK_STR("print_via formats and expands newline", "x=7\r\n", g_captured);
}

/* hype_serial_format_record() takes a va_list; this is the varargs front door
 * the tests call, mirroring how hype_serial_print()/hype_debug_print() use it. */
static int fmt_record(char *buf, unsigned long long bufsz, const char *fmt, ...) {
    va_list ap;
    int truncated;

    va_start(ap, fmt);
    truncated = hype_serial_format_record(buf, bufsz, fmt, ap);
    va_end(ap);
    return truncated;
}

/* #238: a record that fits must be byte-identical to plain formatting -- no
 * marker, no padding, nothing appended. */
static void test_format_record_fits(void) {
    char buf[64];
    int truncated = fmt_record(buf, sizeof(buf), "abc=%d\n", 7);

    CHECK_INT("format_record reports not-truncated when it fits", 0, truncated);
    CHECK_STR("format_record leaves a fitting record untouched", "abc=7\n", buf);
}

/*
 * The bug #238 was actually about: an over-long record used to be cut wherever
 * the buffer ended, which (a) looked exactly like a short message and (b) ate
 * the trailing newline, so the NEXT record continued on the same line and two
 * records read as one. Both properties are asserted here.
 */
static void test_format_record_truncates_visibly(void) {
    char buf[24];
    int truncated = fmt_record(buf, sizeof(buf), "%s", "0123456789abcdefghijklmnopqrstuvwxyz");

    CHECK_INT("format_record reports truncation", 1, truncated);
    CHECK_INT("format_record still NUL-terminates within the buffer", 0, buf[sizeof(buf) - 1]);
    CHECK_INT("truncated record length is bufsz-1", (int)(sizeof(buf) - 1), (int)strlen(buf));
    CHECK_STR("truncated record is marked and still ends in a newline",
              "01234567" "...[TRUNCATED]\n", buf);
    CHECK_INT("truncated record ends in '\\n' so the next record starts a new line",
              '\n', buf[strlen(buf) - 1]);
}

/* Exactly-full is the boundary vsnprintf's return value makes easy to get
 * wrong: "would have written bufsz-1" fits, "would have written bufsz" does not. */
static void test_format_record_exact_fit(void) {
    char buf[8];
    int truncated = fmt_record(buf, sizeof(buf), "%s", "1234567"); /* 7 chars + NUL == 8 */

    CHECK_INT("a record filling the buffer exactly is not truncated", 0, truncated);
    CHECK_STR("exact-fit record is intact", "1234567", buf);
}

/* Degenerate buffers must not be written out of bounds. A buffer too small to
 * hold even the marker keeps plain truncation rather than corrupting memory. */
static void test_format_record_tiny_buffer(void) {
    char buf[4];
    int truncated = fmt_record(buf, sizeof(buf), "%s", "abcdefgh");

    CHECK_INT("tiny buffer still reports truncation", 1, truncated);
    CHECK_INT("tiny buffer stays NUL-terminated", 0, buf[sizeof(buf) - 1]);
    CHECK_INT("tiny buffer holds bufsz-1 chars", 3, (int)strlen(buf));

    /* bufsz == 0 must be a no-op, not a write to buf[-1]. */
    CHECK_INT("zero-size buffer reports not-truncated and writes nothing", 0,
              fmt_record(buf, 0, "%s", "x"));
    CHECK_INT("NULL buffer is refused", 0, fmt_record(0, 16, "%s", "x"));
}

int main(void) {
    test_divisor_for_baud();
    test_write_via_plain();
    test_write_via_expands_newline();
    test_write_via_empty();
    test_print_via();
    test_format_record_fits();
    test_format_record_truncates_visibly();
    test_format_record_exact_fit();
    test_format_record_tiny_buffer();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
