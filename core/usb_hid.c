#include "usb_hid.h"

#include "xhci.h" /* USB descriptor type/class constants */

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
    /* Arrows. Real Set-1 sends these with an 0xE0 prefix; the single-byte forms are
     * used here because hype's own decoder (core/kbd_decode.c) treats them as plain
     * codes, and matching the decoder hype actually has matters more than matching a
     * PS/2 controller nothing here is emulating. */
    [0x4F] = 0x4D, /* Right */
    [0x50] = 0x4B, /* Left */
    [0x51] = 0x50, /* Down */
    [0x52] = 0x48, /* Up */
};

/* Modifier bit (report byte 0) -> Set-1 make code. Left/right Ctrl and Alt both map
 * to their left-hand code: hype's decoder has no side-specific behaviour, and folding
 * them keeps "was Ctrl held" a single question. */
static const uint8_t g_mod_to_set1[8] = {
    0x1D, /* bit0: Left Ctrl  */
    0x2A, /* bit1: Left Shift */
    0x38, /* bit2: Left Alt   */
    0x00, /* bit3: Left GUI   -- unmapped; no consumer */
    0x1D, /* bit4: Right Ctrl */
    0x36, /* bit5: Right Shift */
    0x38, /* bit6: Right Alt  */
    0x00, /* bit7: Right GUI  */
};

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
            if (n < out_cap) out[n++] = code;               /* pressed */
        } else if ((cmod & bit) == 0u && (pmod & bit) != 0u) {
            if (n < out_cap) out[n++] = (uint8_t)(code | 0x80u); /* released */
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
        if (code != 0u && n < out_cap) {
            out[n++] = (uint8_t)(code | 0x80u);
        }
    }
    for (i = 0; i < HYPE_USB_HID_MAX_KEYS; i++) {
        uint8_t usage = cur[2u + i];
        uint8_t code;
        if (usage == 0u || report_has_key(prev, usage)) {
            continue; /* held, not newly pressed -- see the header on auto-repeat */
        }
        code = g_usage_to_set1[usage];
        if (code != 0u && n < out_cap) {
            out[n++] = code;
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
