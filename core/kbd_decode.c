#include "kbd_decode.h"

/* PS/2 Set-1 make codes for the modifier / lock keys. */
#define SC_LSHIFT 0x2Au
#define SC_RSHIFT 0x36u
#define SC_LCTRL 0x1Du
#define SC_EXT_PREFIX 0xE0u
#define SC_BREAK_FLAG 0x80u

/* Extended (0xE0-prefixed) make codes for the arrow keys. */
#define SC_E0_UP 0x48u
#define SC_E0_DOWN 0x50u
#define SC_E0_LEFT 0x4Bu
#define SC_E0_RIGHT 0x4Du

/* Set-1 make code -> unshifted ASCII, indexes 0x00..0x39. 0 = no character
 * (modifier/lock/unmapped, handled separately). Values verified against the
 * standard US Set-1 layout. */
static const char base_map[0x3A] = {
    /*00*/ 0, 0x1B /*Esc*/, '1', '2', '3', '4', '5', '6',
    /*08*/ '7', '8', '9', '0', '-', '=', 0x08 /*BkSp*/, '\t',
    /*10*/ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
    /*18*/ 'o', 'p', '[', ']', '\r' /*Enter*/, 0 /*LCtrl*/, 'a', 's',
    /*20*/ 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    /*28*/ '\'', '`', 0 /*LShift*/, '\\', 'z', 'x', 'c', 'v',
    /*30*/ 'b', 'n', 'm', ',', '.', '/', 0 /*RShift*/, '*',
    /*38*/ 0 /*LAlt*/, ' '
};

/* Shifted ASCII for the same codes (only where it differs meaningfully). */
static const char shift_map[0x3A] = {
    /*00*/ 0, 0x1B, '!', '@', '#', '$', '%', '^',
    /*08*/ '&', '*', '(', ')', '_', '+', 0x08, '\t',
    /*10*/ 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    /*18*/ 'O', 'P', '{', '}', '\r', 0, 'A', 'S',
    /*20*/ 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    /*28*/ '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    /*30*/ 'B', 'N', 'M', '<', '>', '?', 0, '*',
    /*38*/ 0, ' '
};

void hype_kbd_decode_reset(hype_kbd_decode_t *d) {
    d->shift = 0;
    d->ctrl = 0;
    d->e0 = 0;
}

static unsigned emit_arrow(uint8_t final, uint8_t *out, unsigned out_cap) {
    if (out_cap < 3u) {
        return 0;
    }
    out[0] = 0x1B; /* ESC */
    out[1] = '[';
    out[2] = final; /* 'A' up / 'B' down / 'C' right / 'D' left */
    return 3u;
}

unsigned hype_kbd_decode_feed(hype_kbd_decode_t *d, uint8_t scancode, uint8_t *out,
                              unsigned out_cap) {
    uint8_t code;
    int is_break;
    char ch;

    if (scancode == SC_EXT_PREFIX) {
        d->e0 = 1; /* the next scancode is an extended key */
        return 0;
    }

    is_break = (scancode & SC_BREAK_FLAG) != 0;
    code = (uint8_t)(scancode & 0x7Fu);

    if (d->e0) {
        d->e0 = 0;
        if (is_break) {
            return 0; /* extended key release: nothing to emit */
        }
        switch (code) {
            case SC_E0_UP: return emit_arrow('A', out, out_cap);
            case SC_E0_DOWN: return emit_arrow('B', out, out_cap);
            case SC_E0_RIGHT: return emit_arrow('C', out, out_cap);
            case SC_E0_LEFT: return emit_arrow('D', out, out_cap);
            default: return 0; /* other extended keys (Home/End/...) not mapped yet */
        }
    }

    /* Modifier make/break: update state, emit nothing. */
    if (code == SC_LSHIFT || code == SC_RSHIFT) {
        d->shift = is_break ? 0u : 1u;
        return 0;
    }
    if (code == SC_LCTRL) {
        d->ctrl = is_break ? 0u : 1u;
        return 0;
    }
    if (is_break) {
        return 0; /* ordinary key release */
    }
    if (code >= 0x3Au) {
        return 0; /* beyond the mapped range (function keys, keypad, ...) */
    }

    ch = d->shift ? shift_map[code] : base_map[code];
    if (ch == 0) {
        return 0; /* unmapped in this state */
    }

    /* Ctrl + letter -> control code 0x01..0x1A (Ctrl-A..Ctrl-Z). Uses the
     * unshifted letter so Ctrl-Shift-C still yields 0x03. */
    if (d->ctrl) {
        char base = base_map[code];
        if (base >= 'a' && base <= 'z') {
            if (out_cap < 1u) {
                return 0;
            }
            out[0] = (uint8_t)(base - 'a' + 1);
            return 1u;
        }
    }

    if (out_cap < 1u) {
        return 0;
    }
    out[0] = (uint8_t)ch;
    return 1u;
}
