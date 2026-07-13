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

int main(void) {
    test_divisor_for_baud();
    test_write_via_plain();
    test_write_via_expands_newline();
    test_write_via_empty();
    test_print_via();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
