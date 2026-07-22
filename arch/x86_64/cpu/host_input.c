#include "host_input.h"

void hype_host_input_reset(hype_host_input_t *hi) {
    hype_kbd_decode_reset(&hi->dec);
    hype_chord_state_reset(&hi->chord);
}

hype_chord_result_t hype_host_input_feed(hype_host_input_t *hi, uint8_t scancode, uint8_t *out,
                                         unsigned out_cap, unsigned *n_out) {
    hype_chord_result_t r = hype_chord_feed_scancode(&hi->chord, scancode);

    *n_out = 0;

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
