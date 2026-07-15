#ifndef HYPE_LEADER_CHORD_H
#define HYPE_LEADER_CHORD_H

#include <stdint.h>

/*
 * INPUT-4: leader-chord recognition (plan.md §6b) -- pure decode logic
 * over a stream of raw PS/2 Scan Code Set 1 bytes from the HOST's own
 * keyboard controller (arch/x86_64/cpu/ps2_host.h), not any guest-facing
 * device. No hardware access here; feed it bytes, get back actions.
 *
 * The leader is Right-Ctrl+Right-Alt HELD, plus a further action key.
 * Deliberately Right-side only -- Left-Ctrl/Left-Alt share the same
 * base scancode byte but lack the 0xE0 extended prefix, so they are
 * tracked and rejected separately (never confused with the right-side
 * keys this chord actually cares about).
 *
 * Scan Code Set 1 make/break byte values below were fetched and
 * confirmed against a real reference table (vetra.com's PS/2 scan code
 * translation table, cross-checked against OSDev's own documented set)
 * at implementation time, not reconstructed from memory -- same rigor
 * this project applies to every other hardware protocol constant.
 */

#define HYPE_SCANCODE_EXTENDED_PREFIX   0xE0u

#define HYPE_SCANCODE_RIGHT_CTRL_MAKE   0x1Du   /* E0 1D */
#define HYPE_SCANCODE_RIGHT_CTRL_BREAK  0x9Du   /* E0 9D */
#define HYPE_SCANCODE_RIGHT_ALT_MAKE    0x38u   /* E0 38 */
#define HYPE_SCANCODE_RIGHT_ALT_BREAK   0xB8u   /* E0 B8 */

#define HYPE_SCANCODE_LEFT_ARROW_MAKE   0x4Bu   /* E0 4B */
#define HYPE_SCANCODE_RIGHT_ARROW_MAKE  0x4Du   /* E0 4D */

#define HYPE_SCANCODE_ESC_MAKE          0x01u   /* no prefix */
#define HYPE_SCANCODE_D_MAKE            0x20u   /* no prefix */
#define HYPE_SCANCODE_1_MAKE            0x02u   /* no prefix; 1..9 == 0x02..0x0A */
#define HYPE_SCANCODE_9_MAKE            0x0Au

typedef enum {
    HYPE_CHORD_ACTION_NONE = 0,
    HYPE_CHORD_ACTION_TOGGLE_DASHBOARD,   /* Right-Ctrl+Right-Alt+D */
    HYPE_CHORD_ACTION_JUMP_TO_VM,         /* Right-Ctrl+Right-Alt+1..9; see vm_index */
    HYPE_CHORD_ACTION_CYCLE_PREV,         /* Right-Ctrl+Right-Alt+Left */
    HYPE_CHORD_ACTION_CYCLE_NEXT,         /* Right-Ctrl+Right-Alt+Right */
    HYPE_CHORD_ACTION_RETURN_TO_DASHBOARD /* Right-Ctrl+Right-Alt+Esc */
} hype_chord_action_t;

typedef struct {
    hype_chord_action_t action;
    uint8_t vm_index; /* 1-9, only meaningful when action == HYPE_CHORD_ACTION_JUMP_TO_VM */
} hype_chord_result_t;

typedef struct {
    int right_ctrl_held;
    int right_alt_held;
    int pending_extended; /* saw a bare 0xE0 prefix byte, next byte completes it */
} hype_chord_state_t;

void hype_chord_state_reset(hype_chord_state_t *state);

/* Feed one raw scancode byte at a time (the same one-byte-at-a-time
 * shape hype_host_kbd_poll_scancode() naturally produces). Returns
 * HYPE_CHORD_ACTION_NONE for every byte that isn't a fully-formed
 * chord-plus-action-key sequence. */
hype_chord_result_t hype_chord_feed_scancode(hype_chord_state_t *state, uint8_t byte);

#endif
