#include "scancode.h"

#define SC_LSHIFT 0x2Au

/*
 * ASCII -> (Set-1 make code, needs shift).
 *
 * A 128-entry table rather than a chain of range checks: the US layout is not
 * derivable from ASCII values (the digit row's shifted symbols are !@#$%^&*() in
 * keyboard order, and the punctuation keys are scattered across the matrix), so any
 * "clever" arithmetic version would be wrong for most of the punctuation an installer
 * or a drive serial needs.
 *
 * Entry 0 means unmapped. Deliberately covers exactly what a script needs to type:
 * printable ASCII, tab, newline and backspace. Control characters other than those
 * three have no single-key representation and are refused rather than approximated.
 */
typedef struct {
    uint8_t code;
    uint8_t shift;
} sc_entry_t;

static const sc_entry_t g_ascii_to_set1[128] = {
    ['\b'] = {0x0E, 0}, ['\t'] = {0x0F, 0}, ['\n'] = {0x1C, 0}, ['\r'] = {0x1C, 0},
    [' '] = {0x39, 0},
    /* Digit row, unshifted and shifted. */
    ['1'] = {0x02, 0}, ['!'] = {0x02, 1},
    ['2'] = {0x03, 0}, ['@'] = {0x03, 1},
    ['3'] = {0x04, 0}, ['#'] = {0x04, 1},
    ['4'] = {0x05, 0}, ['$'] = {0x05, 1},
    ['5'] = {0x06, 0}, ['%'] = {0x06, 1},
    ['6'] = {0x07, 0}, ['^'] = {0x07, 1},
    ['7'] = {0x08, 0}, ['&'] = {0x08, 1},
    ['8'] = {0x09, 0}, ['*'] = {0x09, 1},
    ['9'] = {0x0A, 0}, ['('] = {0x0A, 1},
    ['0'] = {0x0B, 0}, [')'] = {0x0B, 1},
    ['-'] = {0x0C, 0}, ['_'] = {0x0C, 1},
    ['='] = {0x0D, 0}, ['+'] = {0x0D, 1},
    /* Letters: same key, shift decides case. */
    ['q'] = {0x10, 0}, ['Q'] = {0x10, 1}, ['w'] = {0x11, 0}, ['W'] = {0x11, 1},
    ['e'] = {0x12, 0}, ['E'] = {0x12, 1}, ['r'] = {0x13, 0}, ['R'] = {0x13, 1},
    ['t'] = {0x14, 0}, ['T'] = {0x14, 1}, ['y'] = {0x15, 0}, ['Y'] = {0x15, 1},
    ['u'] = {0x16, 0}, ['U'] = {0x16, 1}, ['i'] = {0x17, 0}, ['I'] = {0x17, 1},
    ['o'] = {0x18, 0}, ['O'] = {0x18, 1}, ['p'] = {0x19, 0}, ['P'] = {0x19, 1},
    ['['] = {0x1A, 0}, ['{'] = {0x1A, 1}, [']'] = {0x1B, 0}, ['}'] = {0x1B, 1},
    ['a'] = {0x1E, 0}, ['A'] = {0x1E, 1}, ['s'] = {0x1F, 0}, ['S'] = {0x1F, 1},
    ['d'] = {0x20, 0}, ['D'] = {0x20, 1}, ['f'] = {0x21, 0}, ['F'] = {0x21, 1},
    ['g'] = {0x22, 0}, ['G'] = {0x22, 1}, ['h'] = {0x23, 0}, ['H'] = {0x23, 1},
    ['j'] = {0x24, 0}, ['J'] = {0x24, 1}, ['k'] = {0x25, 0}, ['K'] = {0x25, 1},
    ['l'] = {0x26, 0}, ['L'] = {0x26, 1},
    [';'] = {0x27, 0}, [':'] = {0x27, 1},
    ['\''] = {0x28, 0}, ['"'] = {0x28, 1},
    ['`'] = {0x29, 0}, ['~'] = {0x29, 1},
    ['\\'] = {0x2B, 0}, ['|'] = {0x2B, 1},
    ['z'] = {0x2C, 0}, ['Z'] = {0x2C, 1}, ['x'] = {0x2D, 0}, ['X'] = {0x2D, 1},
    ['c'] = {0x2E, 0}, ['C'] = {0x2E, 1}, ['v'] = {0x2F, 0}, ['V'] = {0x2F, 1},
    ['b'] = {0x30, 0}, ['B'] = {0x30, 1}, ['n'] = {0x31, 0}, ['N'] = {0x31, 1},
    ['m'] = {0x32, 0}, ['M'] = {0x32, 1},
    [','] = {0x33, 0}, ['<'] = {0x33, 1},
    ['.'] = {0x34, 0}, ['>'] = {0x34, 1},
    ['/'] = {0x35, 0}, ['?'] = {0x35, 1},
};

unsigned int hype_ascii_to_set1(char ch, uint8_t *out, unsigned int out_cap) {
    unsigned char uc = (unsigned char)ch;
    sc_entry_t e;
    unsigned int need;
    unsigned int n = 0;

    if (out == (uint8_t *)0 || uc >= 128u) {
        return 0;
    }
    e = g_ascii_to_set1[uc];
    if (e.code == 0u) {
        return 0; /* unmapped -- cannot type this, and must not pretend otherwise */
    }
    need = e.shift ? 4u : 2u;
    /* All-or-nothing: emitting a make without its break would leave the key -- or
     * worse, shift -- held down for everything that follows. */
    if (out_cap < need) {
        return 0;
    }
    if (e.shift) {
        out[n++] = SC_LSHIFT;
    }
    out[n++] = e.code;
    out[n++] = (uint8_t)(e.code | 0x80u);
    if (e.shift) {
        out[n++] = (uint8_t)(SC_LSHIFT | 0x80u);
    }
    return n;
}

unsigned int hype_ascii_string_to_set1(const char *s, uint8_t *out, unsigned int out_cap) {
    unsigned int n = 0;
    if (s == (const char *)0 || out == (uint8_t *)0) {
        return 0;
    }
    while (*s != '\0') {
        unsigned int got = hype_ascii_to_set1(*s, out + n, out_cap - n);
        if (got == 0u) {
            /* Either unmapped or no room. Stop rather than skip: silently dropping a
             * character from a typed command produces a different command, which is
             * worse than typing a short one the caller can notice. */
            break;
        }
        n += got;
        s++;
    }
    return n;
}
