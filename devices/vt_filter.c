#include "vt_filter.h"

void hype_vt_filter_reset(hype_vt_filter_t *f) {
    f->state = HYPE_VT_STATE_NORMAL;
}

int hype_vt_filter(hype_vt_filter_t *f, uint8_t in, char *out) {
    switch (f->state) {
        case HYPE_VT_STATE_ESC:
            if (in == (uint8_t)'[') {
                f->state = HYPE_VT_STATE_CSI;
            } else {
                /* A two-character escape (e.g. ESC c) or anything else --
                 * the sequence is done; drop this byte too. */
                f->state = HYPE_VT_STATE_NORMAL;
            }
            return 0;
        case HYPE_VT_STATE_CSI:
            /* Consume parameter (0x30-0x3F) and intermediate (0x20-0x2F)
             * bytes until the final byte (0x40-0x7E), which ends it. */
            if (in >= 0x40u && in <= 0x7Eu) {
                f->state = HYPE_VT_STATE_NORMAL;
            }
            return 0;
        case HYPE_VT_STATE_NORMAL:
        default:
            if (in == 0x1Bu) { /* ESC */
                f->state = HYPE_VT_STATE_ESC;
                return 0;
            }
            if (in == (uint8_t)'\n') {
                *out = '\n';
                return 1;
            }
            if (in == (uint8_t)'\t') {
                *out = ' ';
                return 1;
            }
            if (in >= 0x20u && in <= 0x7Eu) {
                *out = (char)in;
                return 1;
            }
            /* '\r', backspace, bell, other control bytes: drop. */
            return 0;
    }
}
