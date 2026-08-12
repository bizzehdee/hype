#include "host_input.h"

void hype_host_input_reset(hype_host_input_t *hi) {
    hype_kbd_decode_reset(&hi->dec);
    hype_chord_state_reset(&hi->chord);
    hi->ps2_extended_pending = 0;
}

hype_chord_result_t hype_host_input_feed_routed(hype_host_input_t *hi, uint8_t scancode,
                                                uint8_t *out, unsigned out_cap,
                                                unsigned *n_out, uint8_t *ps2_out,
                                                unsigned ps2_cap, unsigned *n_ps2_out) {
    int leader_before = hi->chord.right_ctrl_held && hi->chord.right_alt_held;
    int was_extended = hi->ps2_extended_pending;
    hype_chord_result_t r = hype_chord_feed_scancode(&hi->chord, scancode);

    *n_out = 0;
    if (n_ps2_out != 0) {
        *n_ps2_out = 0;
    }

    if (scancode == HYPE_SCANCODE_EXTENDED_PREFIX) {
        hi->ps2_extended_pending = 1;
    } else {
        int reserved_modifier = was_extended &&
            (scancode == HYPE_SCANCODE_RIGHT_CTRL_MAKE ||
             scancode == HYPE_SCANCODE_RIGHT_CTRL_BREAK ||
             scancode == HYPE_SCANCODE_RIGHT_ALT_MAKE ||
             scancode == HYPE_SCANCODE_RIGHT_ALT_BREAK);
        int leader_after = hi->chord.right_ctrl_held && hi->chord.right_alt_held;

        hi->ps2_extended_pending = 0;
        if (n_ps2_out != 0 && ps2_out != 0 && !reserved_modifier &&
            !leader_before && !leader_after && r.action == HYPE_CHORD_ACTION_NONE) {
            if (was_extended) {
                if (ps2_cap >= 2u) {
                    ps2_out[0] = HYPE_SCANCODE_EXTENDED_PREFIX;
                    ps2_out[1] = scancode;
                    *n_ps2_out = 2u;
                }
            } else if (ps2_cap >= 1u) {
                ps2_out[0] = scancode;
                *n_ps2_out = 1u;
            }
        }
    }

    /* A completed chord: consumed as a management action. Drop any half-decoded
     * extended-prefix latch so the swallowed sequence can't taint the next key. */
    if (r.action != HYPE_CHORD_ACTION_NONE) {
        hi->dec.e0 = 0;
        return r;
    }
    /* Leader engaged (both right modifiers held): this byte belongs to the chord
     * machinery (incl. the arrow keys), not the guest -- swallow it. */
    if (hi->chord.right_ctrl_held && hi->chord.right_alt_held) {
        hi->dec.e0 = 0;
        return r;
    }
    /* Ordinary key: decode to the bytes the operator typed, for the focused guest. */
    *n_out = hype_kbd_decode_feed(&hi->dec, scancode, out, out_cap);
    return r;
}

hype_chord_result_t hype_host_input_feed(hype_host_input_t *hi, uint8_t scancode, uint8_t *out,
                                         unsigned out_cap, unsigned *n_out) {
    return hype_host_input_feed_routed(hi, scancode, out, out_cap, n_out, 0, 0u, 0);
}
