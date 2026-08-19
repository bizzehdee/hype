#include "leader_chord.h"

void hype_chord_state_reset(hype_chord_state_t *state) {
    state->right_ctrl_held = 0;
    state->right_alt_held = 0;
    state->pending_extended = 0;
    state->printscreen_step = 0;
    /*
     * #568: added with the field. This function sets every member by name rather than zeroing the
     * struct, so a new member is uninitialised until it is listed HERE -- the tests read a
     * constant 4 from uninitialised stack until this line existed, which is exactly the failure a
     * field-by-field reset invites.
     */
    state->screenshot_near_miss = 0u;
}

hype_chord_result_t hype_chord_feed_scancode(hype_chord_state_t *state, uint8_t byte) {
    hype_chord_result_t none = {HYPE_CHORD_ACTION_NONE, 0};
    hype_chord_result_t result;
    int extended;

    if (byte == HYPE_SCANCODE_EXTENDED_PREFIX) {
        state->pending_extended = 1;
        return none;
    }

    extended = state->pending_extended;
    state->pending_extended = 0;

    if (extended) {
        switch (byte) {
        case HYPE_SCANCODE_RIGHT_CTRL_MAKE:
            state->right_ctrl_held = 1;
            return none;
        case HYPE_SCANCODE_RIGHT_CTRL_BREAK:
            state->right_ctrl_held = 0;
            return none;
        case HYPE_SCANCODE_RIGHT_ALT_MAKE:
            state->right_alt_held = 1;
            return none;
        case HYPE_SCANCODE_RIGHT_ALT_BREAK:
            state->right_alt_held = 0;
            return none;
        case HYPE_SCANCODE_LEFT_ARROW_MAKE:
            if (state->right_ctrl_held && state->right_alt_held) {
                result.action = HYPE_CHORD_ACTION_CYCLE_PREV;
                result.vm_index = 0;
                return result;
            }
            return none;
        case HYPE_SCANCODE_RIGHT_ARROW_MAKE:
            if (state->right_ctrl_held && state->right_alt_held) {
                result.action = HYPE_CHORD_ACTION_CYCLE_NEXT;
                result.vm_index = 0;
                return result;
            }
            return none;
        case HYPE_SCANCODE_PRINTSCREEN_MAKE_1:
            /* First half of `E0 2A E0 37` -- arm the second-half check below,
             * regardless of any prior partial match (a fresh press always restarts it). */
            state->printscreen_step = 1;
            return none;
        case HYPE_SCANCODE_PRINTSCREEN_MAKE_2:
            if (state->printscreen_step == 1) {
                state->printscreen_step = 0;
                if (state->right_ctrl_held && state->right_alt_held) {
                    result.action = HYPE_CHORD_ACTION_SCREENSHOT;
                    result.vm_index = 0;
                    return result;
                }
                /*
                 * #568: Print Screen arrived WITHOUT both modifiers. Recorded so the caller can
                 * say so: on the Intel laptop the operator pressed this repeatedly and nothing
                 * happened, and with no trace at all a key that was never delivered looked
                 * identical to a chord that was seen and rejected. Those are different faults.
                 */
                state->screenshot_near_miss++;
                return none;
            }
            /* `E0 37` without the preceding `E0 2A` this same press -- not Print Screen at
             * all (0x37 alone, un-prefixed, is the keypad '*'; this is its extended cousin
             * only in the specific two-byte sequence). Fall through as unrelated. */
            state->printscreen_step = 0;
            return none;
        default:
            /* Every other extended-prefixed key is not part of the chord -- ignored, held-
             * state untouched. Also aborts a partial Print Screen match: the real sequence
             * is emitted atomically by hardware, so anything else arriving mid-sequence
             * means it was never Print Screen. */
            state->printscreen_step = 0;
            return none;
        }
    }

    /* A plain (non-extended) byte can only arrive here between the `E0 2A` and `E0 37`
     * halves if something other than Print Screen is actually happening -- real hardware
     * emits that sequence atomically. Abort any partial match rather than let a stray
     * later `E0 37` fire on stale state. */
    state->printscreen_step = 0;

    if (byte == HYPE_SCANCODE_D_MAKE) {
        if (state->right_ctrl_held && state->right_alt_held) {
            result.action = HYPE_CHORD_ACTION_TOGGLE_DASHBOARD;
            result.vm_index = 0;
            return result;
        }
        return none;
    }

    /*
     * #458: Print Screen with the leader held reports as SysRq -- a single non-extended 0x54.
     * This, not the four-byte `E0 2A E0 37` form above, is the encoding this chord actually
     * receives; see leader_chord.h for the trace. The break (0xD4) is swallowed rather than
     * left to fall through to the digit range below, which it does not overlap today but sits
     * close enough to that letting it drift there would be an unpleasant surprise.
     */
    if (byte == HYPE_SCANCODE_SYSRQ_MAKE) {
        if (state->right_ctrl_held && state->right_alt_held) {
            result.action = HYPE_CHORD_ACTION_SCREENSHOT;
            result.vm_index = 0;
            return result;
        }
        state->screenshot_near_miss++; /* #568: seen, but without both modifiers */
        return none;
    }
    if (byte == HYPE_SCANCODE_SYSRQ_BREAK) {
        return none;
    }

    if (byte == HYPE_SCANCODE_ESC_MAKE) {
        if (state->right_ctrl_held && state->right_alt_held) {
            result.action = HYPE_CHORD_ACTION_RETURN_TO_DASHBOARD;
            result.vm_index = 0;
            return result;
        }
        return none;
    }

    if (byte >= HYPE_SCANCODE_1_MAKE && byte <= HYPE_SCANCODE_9_MAKE) {
        if (state->right_ctrl_held && state->right_alt_held) {
            result.action = HYPE_CHORD_ACTION_JUMP_TO_VM;
            result.vm_index = (uint8_t)(byte - HYPE_SCANCODE_1_MAKE + 1u);
            return result;
        }
        return none;
    }

    /* Everything else -- including Left-Ctrl (0x1D)/Left-Alt (0x38)
     * make/break codes, which deliberately share a base byte with the
     * right-side keys but arrive with no 0xE0 prefix -- neither updates
     * held state nor produces an action. The chord is Right-Ctrl+
     * Right-Alt specifically, not either modifier side. */
    return none;
}
