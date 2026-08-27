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

/*
 * TERM-8 (#445): Print Screen is not a simple `0xE0 <byte>` key like every other action key
 * here. UNMODIFIED, its make code is the FOUR-byte sequence `E0 2A E0 37` (break `E0 B7 E0 AA`).
 * The two E0-prefixed halves are `0x2A` then `0x37`; tracked via `printscreen_step` below since
 * the existing single-byte `pending_extended` lookahead cannot span a second E0-prefixed byte.
 *
 * #458: that sequence is NOT what this chord ever sees, and matching only it made the screenshot
 * hotkey impossible to trigger. The leading `E0 2A` is a "fake shift" the keyboard inserts only
 * when no Shift/Ctrl/Alt is held. With Alt held -- which it always is here, the leader being
 * Right-Ctrl+Right-Alt -- the key reports as SysRq instead: a single NON-extended `0x54`
 * (break `0xD4`). The two encodings are mutually exclusive by construction, so the decoder has
 * to accept the modified one.
 *
 * Measured, not assumed, with a raw scancode trace at hype's own host input poll. Leader held,
 * then Print Screen:
 *
 *     e0 1d  e0 38        RCtrl make, RAlt make
 *     e0 b8  e0 38        RAlt break, RAlt make  (the keyboard's own modifier dance)
 *     54     d4           <-- Print Screen: SysRq, non-extended
 *
 * and the same key with no modifiers, same keyboard, same run:
 *
 *     e0 2a  e0 37  e0 b7  e0 aa
 *
 * Both paths are kept: `0x54` is what this chord actually produces, and the four-byte form
 * still fires for any keyboard or emulator that emits it regardless of modifiers.
 */
#define HYPE_SCANCODE_PRINTSCREEN_MAKE_1 0x2Au  /* E0 2A, first half of the unmodified make */
#define HYPE_SCANCODE_PRINTSCREEN_MAKE_2 0x37u  /* E0 37, second half */
#define HYPE_SCANCODE_SYSRQ_MAKE 0x54u          /* Print Screen with Ctrl/Alt held (#458) */
#define HYPE_SCANCODE_SYSRQ_BREAK 0xD4u

typedef enum {
    HYPE_CHORD_ACTION_NONE = 0,
    HYPE_CHORD_ACTION_TOGGLE_DASHBOARD,   /* Right-Ctrl+Right-Alt+D */
    HYPE_CHORD_ACTION_JUMP_TO_VM,         /* Right-Ctrl+Right-Alt+1..9; see vm_index */
    HYPE_CHORD_ACTION_CYCLE_PREV,         /* Right-Ctrl+Right-Alt+Left */
    HYPE_CHORD_ACTION_CYCLE_NEXT,         /* Right-Ctrl+Right-Alt+Right */
    HYPE_CHORD_ACTION_RETURN_TO_DASHBOARD, /* Right-Ctrl+Right-Alt+Esc */
    HYPE_CHORD_ACTION_SCREENSHOT           /* Right-Ctrl+Right-Alt+Print Screen (TERM-8, #445) */
} hype_chord_action_t;

typedef struct {
    hype_chord_action_t action;
    uint8_t vm_index; /* 1-9, only meaningful when action == HYPE_CHORD_ACTION_JUMP_TO_VM */
} hype_chord_result_t;

typedef struct {
    int right_ctrl_held;
    int right_alt_held;
    int pending_extended; /* saw a bare 0xE0 prefix byte, next byte completes it */
    /* TERM-8 (#445): progress through Print Screen's 4-byte make sequence `E0 2A E0 37`.
     * 0 = nothing seen yet; 1 = just saw the `E0 2A` half, waiting for `E0 37`. Any other
     * extended byte in between resets this to 0 -- a genuine Print Screen press is never
     * interrupted by another key, since the make sequence is emitted atomically by real
     * hardware. */
    int printscreen_step;
    /*
     * #568: Print Screen / SysRq seen WITHOUT both modifiers held. Counted rather than acted on,
     * so a chord that was pressed and rejected is distinguishable from one that never arrived at
     * all. On the Intel laptop the operator pressed the screenshot chord repeatedly and there was
     * no trace of either case -- the two are different faults (a keyboard that does not emit the
     * key, versus modifiers the layout does not have) and need different fixes.
     */
    unsigned int screenshot_near_miss;
    /*
     * #734: the same idea as screenshot_near_miss, generalised to EVERY chord key, because
     * the same question came up again and the answer was again unavailable from the log.
     *
     * Counted only when at LEAST ONE of the two modifiers is held. Without that condition
     * every typed 'd' and every digit would be a near miss and the counter would be noise.
     * With it, the counter means something specific and useful: "you were holding one half
     * of the leader and pressed a chord key, and the other half never arrived." On a
     * keyboard whose right-hand Alt is an AltGr or an Fn-layer key that the firmware does
     * not report as usage 0xE6, that is exactly the trace needed -- and it distinguishes
     * that from a chord key that never reached hype at all, which leaves both counters at
     * zero.
     *
     * near_miss_mods records which half WAS held at the last near miss: bit 0 right Ctrl,
     * bit 1 right Alt. That names the missing key rather than just saying one is missing.
     */
    unsigned int chord_near_miss;
    uint8_t near_miss_mods;
} hype_chord_state_t;

void hype_chord_state_reset(hype_chord_state_t *state);

/* Feed one raw scancode byte at a time (the same one-byte-at-a-time
 * shape hype_host_kbd_poll_scancode() naturally produces). Returns
 * HYPE_CHORD_ACTION_NONE for every byte that isn't a fully-formed
 * chord-plus-action-key sequence. */
hype_chord_result_t hype_chord_feed_scancode(hype_chord_state_t *state, uint8_t byte);

#endif
