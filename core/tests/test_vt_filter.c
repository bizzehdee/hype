#include <stdio.h>
#include <string.h>
#include "../../devices/vt_filter.h"

static int failures = 0;

#define CHECK(desc, cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s\n", (desc)); \
            failures++; \
        } \
    } while (0)

/* Feed a byte string through the filter and collect the emitted chars. */
static void run(hype_vt_filter_t *f, const char *in, unsigned int n, char *out, unsigned int *out_len) {
    unsigned int i;
    *out_len = 0;
    for (i = 0; i < n; i++) {
        char c;
        if (hype_vt_filter(f, (uint8_t)in[i], &c)) {
            out[(*out_len)++] = c;
        }
    }
    out[*out_len] = '\0';
}

static void test_plain_text_passes(void) {
    hype_vt_filter_t f;
    char out[64];
    unsigned int len;
    hype_vt_filter_reset(&f);
    run(&f, "Shell> ", 7, out, &len);
    CHECK("plain text passes unchanged", strcmp(out, "Shell> ") == 0);
}

static void test_newline_tab_cr(void) {
    hype_vt_filter_t f;
    char out[64];
    unsigned int len;
    hype_vt_filter_reset(&f);
    run(&f, "a\tb\r\nc", 6, out, &len);
    /* tab -> space, CR dropped, LF kept */
    CHECK("tab->space, CR dropped, LF kept", strcmp(out, "a b\nc") == 0);
}

static void test_csi_sequence_stripped(void) {
    hype_vt_filter_t f;
    char out[64];
    unsigned int len;
    hype_vt_filter_reset(&f);
    /* ESC[2J ESC[01;01H "Hi" ESC[0m */
    run(&f, "\x1b[2J\x1b[01;01HHi\x1b[0m", 17, out, &len);
    CHECK("CSI sequences stripped, text kept", strcmp(out, "Hi") == 0);
}

static void test_two_char_escape_stripped(void) {
    hype_vt_filter_t f;
    char out[64];
    unsigned int len;
    hype_vt_filter_reset(&f);
    /* ESC c (reset) then text */
    run(&f, "\x1b" "cX", 3, out, &len);
    CHECK("two-char ESC stripped", strcmp(out, "X") == 0);
}

static void test_control_bytes_dropped(void) {
    hype_vt_filter_t f;
    char out[64];
    unsigned int len;
    hype_vt_filter_reset(&f);
    run(&f, "A\x07\x08\x01" "B", 5, out, &len); /* bell, backspace, SOH dropped */
    CHECK("control bytes dropped", strcmp(out, "AB") == 0);
}

static void test_high_bytes_dropped(void) {
    hype_vt_filter_t f;
    char out[64];
    unsigned int len;
    hype_vt_filter_reset(&f);
    /* Bytes >= 0x80 (UTF-8 multibyte lead/continuation) are not in the
     * printable-ASCII range and are dropped in both NORMAL and CSI. */
    run(&f, "a\x80\xffZ\x1b[\xc0mQ", 9, out, &len);
    CHECK("high bytes dropped in NORMAL and CSI", strcmp(out, "aZQ") == 0);
}

int main(void) {
    test_plain_text_passes();
    test_newline_tab_cr();
    test_csi_sequence_stripped();
    test_two_char_escape_stripped();
    test_control_bytes_dropped();
    test_high_bytes_dropped();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
