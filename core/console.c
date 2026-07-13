#include <stdarg.h>

#include "console.h"
#include "format.h"

#define HYPE_CONSOLE_BUF_MAX 512

unsigned long long hype_console_widen(const char *ascii, CHAR16 *out, unsigned long long out_count) {
    unsigned long long w = 0;
    unsigned long long i = 0;

    if (out_count == 0) {
        return 0;
    }

    while (ascii[i] != '\0') {
        char c = ascii[i];

        if (c == '\n') {
            if (w + 1 < out_count) {
                out[w] = (CHAR16)'\r';
            }
            w++;
            if (w + 1 < out_count) {
                out[w] = (CHAR16)'\n';
            }
            w++;
        } else {
            if (w + 1 < out_count) {
                out[w] = (CHAR16)(unsigned char)c;
            }
            w++;
        }
        i++;
    }

    if (w < out_count) {
        out[w] = 0;
    } else {
        out[out_count - 1] = 0;
    }
    return w;
}

void hype_console_print(EFI_SYSTEM_TABLE *system_table, const char *fmt, ...) {
    char ascii[HYPE_CONSOLE_BUF_MAX];
    CHAR16 wide[HYPE_CONSOLE_BUF_MAX];
    va_list ap;

    va_start(ap, fmt);
    hype_vsnprintf(ascii, sizeof(ascii), fmt, ap);
    va_end(ap);

    hype_console_widen(ascii, wide, HYPE_CONSOLE_BUF_MAX);
    system_table->ConOut->OutputString(system_table->ConOut, wide);
}
