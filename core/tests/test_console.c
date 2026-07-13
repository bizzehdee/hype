#include <stdio.h>
#include <string.h>
#include "../console.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((expected) != (actual)) { \
            printf("FAIL: %s: expected %d, got %d\n", (desc), (int)(expected), (int)(actual)); \
            failures++; \
        } \
    } while (0)

static void check_wide(const char *desc, const char *ascii, const CHAR16 *expected, unsigned long long expected_len) {
    CHAR16 out[64];
    unsigned long long r = hype_console_widen(ascii, out, 64);
    unsigned long long i;

    CHECK_INT(desc, (int)expected_len, (int)r);
    for (i = 0; i <= expected_len; i++) {
        if (out[i] != expected[i]) {
            printf("FAIL: %s: mismatch at index %llu: expected %u, got %u\n",
                   desc, i, (unsigned)expected[i], (unsigned)out[i]);
            failures++;
            break;
        }
    }
}

static CHAR16 captured[256];
static int output_string_calls = 0;

static EFI_STATUS EFIAPI mock_output_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this, CHAR16 *str) {
    unsigned long long i = 0;
    (void)this;
    while (str[i] != 0 && i < 255) {
        captured[i] = str[i];
        i++;
    }
    captured[i] = 0;
    output_string_calls++;
    return EFI_SUCCESS;
}

int main(void) {
    {
        CHAR16 expected[] = {'h', 'i', 0};
        check_wide("plain ascii, no newline", "hi", expected, 2);
    }
    {
        CHAR16 expected[] = {'a', '\r', '\n', 'b', 0};
        check_wide("newline expands to CRLF", "a\nb", expected, 4);
    }
    {
        CHAR16 out[3];
        unsigned long long r = hype_console_widen("hello", out, 3);
        CHECK_INT("truncated widen return value is untruncated length", 5, (int)r);
        CHECK_INT("truncated widen is NUL-terminated within bound", 0, (int)out[2]);
    }
    {
        CHAR16 out[4];
        unsigned long long r = hype_console_widen("", out, 4);
        CHECK_INT("empty string widens to zero length", 0, (int)r);
        CHECK_INT("empty string NUL-terminates immediately", 0, (int)out[0]);
    }
    {
        CHAR16 out[4];
        unsigned long long r = hype_console_widen("x", out, 0);
        CHECK_INT("out_count==0 does not crash, reports zero", 0, (int)r);
        (void)out;
    }
    {
        CHAR16 out[2];
        unsigned long long r = hype_console_widen("a\nbc", out, 2);
        CHECK_INT("truncation exactly at CRLF expansion is untruncated length", 5, (int)r);
        CHECK_INT("truncation exactly at CRLF expansion is NUL-terminated", 0, (int)out[1]);
    }

    {
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL con_out;
        EFI_SYSTEM_TABLE st;

        memset(&con_out, 0, sizeof(con_out));
        memset(&st, 0, sizeof(st));
        con_out.OutputString = mock_output_string;
        st.ConOut = &con_out;

        hype_console_print(&st, "%s=%d\n", "hype", 1);
        CHECK_INT("hype_console_print invokes ConOut->OutputString once", 1, output_string_calls);

        {
            const char *expect = "hype=1\r\n";
            unsigned long long i;
            for (i = 0; expect[i]; i++) {
                if (captured[i] != (CHAR16)expect[i]) {
                    printf("FAIL: hype_console_print output content mismatch at %llu\n", i);
                    failures++;
                    break;
                }
            }
            CHECK_INT("hype_console_print output is NUL-terminated at expected length",
                      0, (int)captured[i]);
        }
    }

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
