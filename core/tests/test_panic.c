#include <stdio.h>
#include <string.h>
#include "../panic.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((expected) != (actual)) { \
            printf("FAIL: %s: expected %d, got %d\n", (desc), (int)(expected), (int)(actual)); \
            failures++; \
        } \
    } while (0)

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
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL con_out;
    EFI_SYSTEM_TABLE st;
    const char *expect = "PANIC: out of memory\r\n";
    unsigned long long i;

    memset(&con_out, 0, sizeof(con_out));
    memset(&st, 0, sizeof(st));
    con_out.OutputString = mock_output_string;
    st.ConOut = &con_out;

    hype_panic_message(&st, "out of memory");

    CHECK_INT("hype_panic_message invokes ConOut->OutputString once", 1, output_string_calls);
    for (i = 0; expect[i]; i++) {
        if (captured[i] != (CHAR16)expect[i]) {
            printf("FAIL: panic message content mismatch at %llu\n", i);
            failures++;
            break;
        }
    }
    CHECK_INT("panic message is NUL-terminated at expected length", 0, (int)captured[i]);

    /*
     * hype_panic() itself (format + hlt-loop halt) is intentionally not
     * exercised here -- it never returns, so calling it would hang this
     * test binary rather than verify anything. See halt.h.
     */

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
