#include "usb_hid.h"

#include "xhci.h" /* USB descriptor type/class constants */

/* Set-1's extended-key prefix. Defined here rather than pulled from the arch-side
 * leader_chord.h -- core/ must not include arch/ -- exactly as core/kbd_decode.c
 * defines its own SC_EXT_PREFIX for the same byte. */
#define SC_EXT_PREFIX 0xE0u

/*
 * HID keyboard usage id -> PS/2 Set-1 make code.
 *
 * Sparse table indexed by usage id; 0 means "not mapped", which is the right default
 * for the ~200 usages a boot keyboard can report but hype has no use for (media keys,
 * international keys, the keypad). An unmapped key produces no scancode rather than a
 * wrong one -- a phantom keystroke in a destructive-write confirmation prompt is worse
 * than a missing one.
 *
 * Covers what an operator needs to type a drive serial and drive the dashboard:
 * letters, digits, Enter, Backspace, Space, Tab, Escape, the ASCII punctuation, the
 * modifiers, and the function/arrow keys the leader chord uses.
 */
static const uint8_t g_usage_to_set1[256] = {
    /* 0x04-0x1D: a..z (Set-1 order is NOT alphabetical -- it is the physical
     * keyboard matrix, so this is transcribed per key rather than computed). */
    [0x04] = 0x1E, /* a */ [0x05] = 0x30, /* b */ [0x06] = 0x2E, /* c */
    [0x07] = 0x20, /* d */ [0x08] = 0x12, /* e */ [0x09] = 0x21, /* f */
    [0x0A] = 0x22, /* g */ [0x0B] = 0x23, /* h */ [0x0C] = 0x17, /* i */
    [0x0D] = 0x24, /* j */ [0x0E] = 0x25, /* k */ [0x0F] = 0x26, /* l */
    [0x10] = 0x32, /* m */ [0x11] = 0x31, /* n */ [0x12] = 0x18, /* o */
    [0x13] = 0x19, /* p */ [0x14] = 0x10, /* q */ [0x15] = 0x13, /* r */
    [0x16] = 0x1F, /* s */ [0x17] = 0x14, /* t */ [0x18] = 0x16, /* u */
    [0x19] = 0x2F, /* v */ [0x1A] = 0x11, /* w */ [0x1B] = 0x2D, /* x */
    [0x1C] = 0x15, /* y */ [0x1D] = 0x2C, /* z */
    /* 0x1E-0x27: 1 2 3 4 5 6 7 8 9 0 */
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05, [0x22] = 0x06,
    [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09, [0x26] = 0x0A, [0x27] = 0x0B,
    [0x28] = 0x1C, /* Enter */
    [0x29] = 0x01, /* Escape */
    [0x2A] = 0x0E, /* Backspace */
    [0x2B] = 0x0F, /* Tab */
    [0x2C] = 0x39, /* Space */
    [0x2D] = 0x0C, /* - */
    [0x2E] = 0x0D, /* = */
    [0x2F] = 0x1A, /* [ */
    [0x30] = 0x1B, /* ] */
    [0x31] = 0x2B, /* backslash */
    [0x33] = 0x27, /* ; */
    [0x34] = 0x28, /* ' */
    [0x35] = 0x29, /* ` */
    [0x36] = 0x33, /* , */
    [0x37] = 0x34, /* . */
    [0x38] = 0x35, /* / */
    [0x39] = 0x3A, /* CapsLock */
    /* F1..F12 */
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E, [0x3E] = 0x3F,
    [0x3F] = 0x40, [0x40] = 0x41, [0x41] = 0x42, [0x42] = 0x43, [0x43] = 0x44,
    [0x44] = 0x57, [0x45] = 0x58,
    /*
     * Arrows -- EXTENDED, emitted as `E0 <code>`. See g_usage_is_ext below.
     *
     * This table used to emit the single-byte forms, on the reasoning that hype's own
     * decoder treats them as plain codes. That reasoning was simply wrong:
     * core/kbd_decode.c has handled the 0xE0 prefix since it was written (SC_E0_UP and
     * friends) and does NOT map the un-prefixed forms to arrows at all -- un-prefixed
     * 0x48 is the keypad, not Up.
     */
    [0x4F] = 0x4D, /* Right */
    [0x50] = 0x4B, /* Left */
    [0x51] = 0x50, /* Down */
    [0x52] = 0x48, /* Up */
};

/*
 * Which usages need the 0xE0 prefix that real Set-1 sends for them.
 *
 * Kept as a parallel table rather than a flag bit in the code, because the code byte is
 * what goes on the wire and packing a marker into it would have to be masked out again at
 * every use, including in hype_usb_hid_usage_to_scancode()'s callers.
 */
static const uint8_t g_usage_is_ext[256] = {
    [0x4F] = 1, [0x50] = 1, [0x51] = 1, [0x52] = 1, /* Right, Left, Down, Up */
};

/*
 * Modifier bit (report byte 0) -> Set-1 make code, and whether it is extended.
 *
 * #734: the RIGHT-hand Ctrl and Alt are `E0 1D` and `E0 38` on real Set-1, and they used
 * to be folded onto the left-hand single-byte codes here "because hype's decoder has no
 * side-specific behaviour". One consumer very much does: hype_chord_feed_scancode() sets
 * right_ctrl_held / right_alt_held ONLY from the extended forms, and every leader chord
 * requires both. Folding them meant no chord could ever fire from a USB keyboard --
 * measured on the 5950X as `scancodes=147 chords=0`, with the operator unable to get back
 * out of a guest because Right-Ctrl+Right-Alt+Esc could not be recognised.
 *
 * These bytes share a queue with the PS/2 ISR's raw output (hype_host_kbd_inject_scancode),
 * so real Set-1 is the queue's contract, not an aesthetic preference.
 */
static const uint8_t g_mod_to_set1[8] = {
    0x1D, /* bit0: Left Ctrl  */
    0x2A, /* bit1: Left Shift */
    0x38, /* bit2: Left Alt   */
    0x00, /* bit3: Left GUI   -- unmapped; no consumer */
    0x1D, /* bit4: Right Ctrl  -- E0 1D */
    0x36, /* bit5: Right Shift -- genuinely single-byte on Set-1 */
    0x38, /* bit6: Right Alt   -- E0 38 */
    0x00, /* bit7: Right GUI  */
};
static const uint8_t g_mod_is_ext[8] = { 0, 0, 0, 0, 1, 0, 1, 0 };

/* Append one Set-1 event: `E0` first when extended, then make or break. Returns the
 * new count; writes nothing at all if the pair would not fit, so a truncated buffer
 * never emits a lone 0xE0 that would re-prefix whatever byte follows it. */
static unsigned int emit_code(uint8_t *out, unsigned int n, unsigned int out_cap,
                              uint8_t code, int ext, int release) {
    unsigned int need = ext ? 2u : 1u;

    if (n + need > out_cap) {
        return n;
    }
    if (ext) {
        out[n++] = SC_EXT_PREFIX;
    }
    out[n++] = release ? (uint8_t)(code | 0x80u) : code;
    return n;
}

uint8_t hype_usb_hid_usage_to_scancode(uint8_t usage) {
    return g_usage_to_set1[usage];
}

/* Shared walker: find a boot-protocol HID interface with `protocol` and its interrupt-IN
 * endpoint. One implementation for keyboard and mouse -- the descriptor walk, the
 * malformed-length guard and the "skip the interrupt-OUT" rule are identical, and two
 * copies would be two places for them to drift. */
static int find_boot_hid(const uint8_t *cfg, unsigned int len, unsigned int protocol,
                         hype_usb_hid_kbd_t *out) {
    unsigned int off = 0;
    int in_kbd_iface = 0;
    unsigned int cfg_value = 0;

    if (out == (hype_usb_hid_kbd_t *)0) {
        return -1;
    }
    out->found = 0;
    out->interface_num = 0;
    out->config_value = 0;
    out->int_in_ep = 0;
    out->mps = 0;
    out->interval = 0;
    if (cfg == (const uint8_t *)0) {
        return -1;
    }

    while (off + 2u <= len) {
        unsigned int dlen = cfg[off];
        unsigned int dtype = cfg[off + 1u];

        /* Device-supplied buffer: a zero or overlong length would otherwise spin or
         * read past the end, so it terminates the walk rather than being trusted. */
        if (dlen < 2u || off + dlen > len) {
            return -1;
        }
        if (dtype == HYPE_USB_DESC_CONFIG && dlen >= 6u) {
            cfg_value = cfg[off + 5u];
        } else if (dtype == HYPE_USB_DESC_INTERFACE && dlen >= 9u) {
            /* Boot protocol only -- see the header for why a non-boot HID is refused
             * rather than guessed at. */
            in_kbd_iface = (cfg[off + 5u] == HYPE_USB_CLASS_HID &&
                            cfg[off + 6u] == HYPE_USB_SUBCLASS_BOOT &&
                            cfg[off + 7u] == protocol);
            if (in_kbd_iface) {
                out->interface_num = cfg[off + 2u];
            }
        } else if (dtype == HYPE_USB_DESC_ENDPOINT && dlen >= 7u && in_kbd_iface) {
            unsigned int addr = cfg[off + 2u];
            unsigned int attr = cfg[off + 3u];
            /* Interrupt (attr bits 1:0 == 3) and IN (address bit 7). A boot keyboard
             * may also expose an interrupt OUT for its LEDs; taking the first
             * interrupt endpoint regardless of direction would bind to that and then
             * never receive a report. */
            if ((attr & 0x03u) == 0x03u && (addr & 0x80u) != 0u) {
                out->int_in_ep = addr;
                out->mps = (unsigned int)cfg[off + 4u] | ((unsigned int)cfg[off + 5u] << 8);
                out->interval = cfg[off + 6u];
                out->config_value = cfg_value;
                out->found = 1;
                return 0;
            }
        }
        off += dlen;
    }
    return -1;
}

int hype_usb_hid_find_keyboard(const uint8_t *cfg, unsigned int len, hype_usb_hid_kbd_t *out) {
    return find_boot_hid(cfg, len, HYPE_USB_PROTO_KEYBOARD, out);
}

int hype_usb_hid_find_mouse(const uint8_t *cfg, unsigned int len, hype_usb_hid_kbd_t *out) {
    return find_boot_hid(cfg, len, HYPE_USB_PROTO_MOUSE, out);
}

/* Is `usage` present in a report's 6-key array? */
static int report_has_key(const uint8_t *rep, uint8_t usage) {
    unsigned int i;
    if (usage == 0u) {
        return 0; /* 0 = empty slot, not a key */
    }
    for (i = 0; i < HYPE_USB_HID_MAX_KEYS; i++) {
        if (rep[2u + i] == usage) {
            return 1;
        }
    }
    return 0;
}

unsigned int hype_usb_hid_report_to_scancodes(const uint8_t *prev, const uint8_t *cur,
                                              uint8_t *out, unsigned int out_cap) {
    static const uint8_t zero_report[HYPE_USB_HID_REPORT_LEN] = {0, 0, 0, 0, 0, 0, 0, 0};
    unsigned int n = 0;
    unsigned int i;
    uint8_t pmod, cmod;

    if (cur == (const uint8_t *)0 || out == (uint8_t *)0) {
        return 0;
    }
    if (prev == (const uint8_t *)0) {
        prev = zero_report;
    }
    pmod = prev[0];
    cmod = cur[0];

    /*
     * Modifier transitions first. Order matters: a Shift press must reach the decoder
     * BEFORE the key it modifies, or the shifted character decodes as unshifted --
     * which for a typed drive serial would silently produce the wrong string.
     */
    for (i = 0; i < 8u; i++) {
        uint8_t code = g_mod_to_set1[i];
        uint8_t bit = (uint8_t)(1u << i);
        if (code == 0u) {
            continue;
        }
        if ((cmod & bit) != 0u && (pmod & bit) == 0u) {
            n = emit_code(out, n, out_cap, code, g_mod_is_ext[i], 0); /* pressed */
        } else if ((cmod & bit) == 0u && (pmod & bit) != 0u) {
            n = emit_code(out, n, out_cap, code, g_mod_is_ext[i], 1); /* released */
        }
    }

    /* Releases before presses, so a report where one key replaces another cannot
     * momentarily look like both being held. */
    for (i = 0; i < HYPE_USB_HID_MAX_KEYS; i++) {
        uint8_t usage = prev[2u + i];
        uint8_t code;
        if (usage == 0u || report_has_key(cur, usage)) {
            continue;
        }
        code = g_usage_to_set1[usage];
        if (code != 0u) {
            n = emit_code(out, n, out_cap, code, g_usage_is_ext[usage], 1);
        }
    }
    for (i = 0; i < HYPE_USB_HID_MAX_KEYS; i++) {
        uint8_t usage = cur[2u + i];
        uint8_t code;
        if (usage == 0u || report_has_key(prev, usage)) {
            continue; /* held, not newly pressed -- see the header on auto-repeat */
        }
        code = g_usage_to_set1[usage];
        if (code != 0u) {
            n = emit_code(out, n, out_cap, code, g_usage_is_ext[usage], 0);
        }
    }
    return n;
}

/* Clamp a HID delta into the 8-bit signed range PS/2 carries. */
static int clamp_delta(int v) {
    if (v > 127) {
        return 127;
    }
    if (v < -127) {
        return -127;
    }
    return v;
}

unsigned int hype_usb_hid_mouse_report_to_ps2(const uint8_t *report, unsigned int len,
                                              uint8_t *out, unsigned int out_cap) {
    int dx, dy;
    uint8_t status;

    if (report == (const uint8_t *)0 || out == (uint8_t *)0 ||
        len < HYPE_USB_HID_MOUSE_REPORT_MIN || out_cap < HYPE_USB_HID_PS2_PACKET_LEN) {
        return 0;
    }
    dx = clamp_delta((int)(int8_t)report[1]);
    /* PS/2 +Y is UP, HID +Y is DOWN. Negating here is what keeps a pointer from moving
     * vertically backwards. -128 has no positive counterpart in 8-bit two's complement,
     * so clamp_delta's -127 floor is applied to the HID value first. */
    dy = -clamp_delta((int)(int8_t)report[2]);

    status = 0x08u; /* bit 3 always set (PS/2 mouse packet marker) */
    status |= (uint8_t)(report[0] & 0x07u); /* left/right/middle, same bit order */
    if (dx < 0) {
        status |= 0x10u;
    }
    if (dy < 0) {
        status |= 0x20u;
    }
    out[0] = status;
    out[1] = (uint8_t)dx;
    out[2] = (uint8_t)dy;
    return HYPE_USB_HID_PS2_PACKET_LEN;
}

/* ---------------------------------------------------------------- #774 typematic */

void hype_usb_hid_typematic_init(hype_usb_hid_typematic_t *t) {
    if (t == 0) return;
    t->usage = 0;
    t->code = 0;
    t->ext = 0;
    t->next_at_ms = 0;
    t->delay_ms = HYPE_USB_HID_TYPEMATIC_DELAY_MS;
    t->period_ms = HYPE_USB_HID_TYPEMATIC_PERIOD_MS;
    t->started = 0;
}

void hype_usb_hid_typematic_note(hype_usb_hid_typematic_t *t, const uint8_t report[8],
                                 uint64_t now_ms) {
    unsigned int i;
    uint8_t hold = 0;

    if (t == 0 || report == 0) return;

    /*
     * The LAST ordinary key in the report is the one that repeats. A HID boot report lists
     * held keys in bytes 2..7 in the order they were pressed, so the last non-zero entry is
     * the most recent -- which is the key a PS/2 keyboard would be repeating.
     */
    for (i = 2u; i < HYPE_USB_HID_REPORT_LEN; i++) {
        if (report[i] != 0u && report[i] != 0x01u /* ErrorRollOver */) {
            hold = report[i];
        }
    }

    if (hold == 0u) {
        t->usage = 0; /* modifiers alone do not repeat */
        t->started = 0;
        return;
    }
    if (hold == t->usage) {
        return; /* same key still down -- leave the schedule alone */
    }
    /* A different key took over: restart from the delay, as a PS/2 keyboard does. */
    t->usage = hold;
    t->code = g_usage_to_set1[hold];
    t->ext = g_usage_is_ext[hold] ? 1 : 0;
    t->started = 0;
    t->next_at_ms = now_ms + (uint64_t)t->delay_ms;
}

unsigned int hype_usb_hid_typematic_tick(hype_usb_hid_typematic_t *t, uint64_t now_ms,
                                         uint8_t *out, unsigned int out_cap) {
    unsigned int n = 0;

    if (t == 0 || out == 0 || t->usage == 0u || t->code == 0u) return 0;
    if (now_ms < t->next_at_ms) return 0;

    n = emit_code(out, 0u, out_cap, t->code, t->ext, 0 /* make */);
    if (n == 0u) return 0; /* no room for the whole sequence -- emit nothing, never a lone 0xE0 */
    t->started = 1;
    t->next_at_ms = now_ms + (uint64_t)t->period_ms;
    return n;
}

void hype_usb_hid_typematic_set_f3(hype_usb_hid_typematic_t *t, uint8_t param) {
    /*
     * 8042 Set Typematic Rate/Delay. Bits 6:5 select the delay and bits 4:0 the rate.
     *
     * The rate is not linear: it is (8 + B) * 2^A periods of 4.17 ms, where A is bits 4:3 and
     * B is bits 2:0. Computed rather than tabulated, so an unusual value a guest picks is
     * honoured instead of falling back to the default.
     */
    unsigned int a, b, ticks;

    if (t == 0) return;
    t->delay_ms = 250u * (unsigned int)(((param >> 5) & 0x3u) + 1u);
    a = ((unsigned int)param >> 3) & 0x3u;
    b = (unsigned int)param & 0x7u;
    ticks = (8u + b) << a;              /* in units of 4.17 ms */
    t->period_ms = (ticks * 417u) / 100u;
    if (t->period_ms == 0u) t->period_ms = 1u;
}
