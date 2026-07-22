#include <stdio.h>
#include "../kbd_decode.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* Feed one scancode; expect exactly one output byte and return it (-1 if the
 * count wasn't 1). */
static int one(hype_kbd_decode_t *d, uint8_t sc) {
    uint8_t out[HYPE_KBD_DECODE_MAX_OUT];
    unsigned n = hype_kbd_decode_feed(d, sc, out, sizeof(out));
    return (n == 1u) ? (int)out[0] : -1;
}

/* Feed one scancode; expect zero output bytes (modifier/break/unmapped). */
static int zero(hype_kbd_decode_t *d, uint8_t sc) {
    uint8_t out[HYPE_KBD_DECODE_MAX_OUT];
    return hype_kbd_decode_feed(d, sc, out, sizeof(out)) == 0u;
}

int main(void) {
    hype_kbd_decode_t d;
    hype_kbd_decode_reset(&d);

    /* Plain letters / digits. */
    CHECK_HEX("'a' (0x1E)", 'a', one(&d, 0x1E));
    CHECK_HEX("'1' (0x02)", '1', one(&d, 0x02));
    CHECK_HEX("space (0x39)", ' ', one(&d, 0x39));
    CHECK_HEX("Enter (0x1C) -> CR", '\r', one(&d, 0x1C));
    CHECK_HEX("Backspace (0x0E)", 0x08, one(&d, 0x0E));
    CHECK_HEX("Esc (0x01)", 0x1B, one(&d, 0x01));
    CHECK_HEX("Tab (0x0F)", '\t', one(&d, 0x0F));

    /* Shift: press (make 0x2A), type, release (break 0xAA). */
    CHECK_HEX("Shift make emits nothing", 1, zero(&d, 0x2A));
    CHECK_HEX("Shift+'a' -> 'A'", 'A', one(&d, 0x1E));
    CHECK_HEX("Shift+'1' -> '!'", '!', one(&d, 0x02));
    CHECK_HEX("Shift release emits nothing", 1, zero(&d, 0x2A | 0x80));
    CHECK_HEX("after release, 'a' -> 'a'", 'a', one(&d, 0x1E));

    /* Ctrl + letter -> control code; ctrl release restores. */
    CHECK_HEX("Ctrl make emits nothing", 1, zero(&d, 0x1D));
    CHECK_HEX("Ctrl+'c' (0x2E) -> 0x03", 0x03, one(&d, 0x2E));
    CHECK_HEX("Ctrl+Shift still control (press shift)", 1, zero(&d, 0x2A));
    CHECK_HEX("Ctrl+Shift+'c' -> 0x03", 0x03, one(&d, 0x2E));
    CHECK_HEX("release shift", 1, zero(&d, 0x2A | 0x80));
    CHECK_HEX("Ctrl release emits nothing", 1, zero(&d, 0x1D | 0x80));
    CHECK_HEX("after ctrl release, 'c' -> 'c'", 'c', one(&d, 0x2E));

    /* Ordinary key release emits nothing. */
    CHECK_HEX("'a' break (0x9E) emits nothing", 1, zero(&d, 0x1E | 0x80));

    /* Arrow keys: 0xE0 prefix then the extended make -> ESC [ <final>. */
    {
        uint8_t out[HYPE_KBD_DECODE_MAX_OUT];
        unsigned n;
        CHECK_HEX("0xE0 prefix emits nothing", 1, zero(&d, 0xE0));
        n = hype_kbd_decode_feed(&d, 0x48, out, sizeof(out)); /* Up */
        CHECK_HEX("Up arrow -> 3 bytes", 3u, n);
        CHECK_HEX("Up[0]=ESC", 0x1B, out[0]);
        CHECK_HEX("Up[1]='['", '[', out[1]);
        CHECK_HEX("Up[2]='A'", 'A', out[2]);
        /* Extended key release emits nothing. */
        CHECK_HEX("0xE0 prefix (release) emits nothing", 1, zero(&d, 0xE0));
        CHECK_HEX("Up break emits nothing", 1, zero(&d, 0x48 | 0x80));
    }
    {
        uint8_t out[HYPE_KBD_DECODE_MAX_OUT];
        hype_kbd_decode_feed(&d, 0xE0, out, sizeof(out));
        CHECK_HEX("Left arrow final 'D'", 'D',
                  (hype_kbd_decode_feed(&d, 0x4B, out, sizeof(out)) == 3u) ? out[2] : 0);
    }

    /* Unmapped (F1 = 0x3B) emits nothing. */
    CHECK_HEX("F1 (0x3B) emits nothing", 1, zero(&d, 0x3B));

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
