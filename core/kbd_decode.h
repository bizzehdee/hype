#ifndef HYPE_CORE_KBD_DECODE_H
#define HYPE_CORE_KBD_DECODE_H

#include <stdint.h>

/*
 * TERM-4 (step 1 of the interactive per-VM terminal): decode a stream of host
 * PS/2 Set-1 scancodes into the character/control bytes an operator "typed",
 * ready to feed a guest's serial console (hype_guest_uart_rx_enqueue) or PS/2.
 *
 * This is the PS/2 backend's decoder. It is intentionally split from both the
 * host controller (INPUT-3, ps2_host*) and the routing/focus layer so that a
 * future USB HID backend (TERM-5) produces the SAME byte stream into the SAME
 * router with no rework -- the router consumes bytes, not device events. Pure
 * logic (modifier state machine + lookup tables), fully unit-tested.
 *
 * Scope: printable ASCII (with Shift), Enter (CR), Backspace, Tab, Esc, Space,
 * Ctrl-<letter> control codes, and the four arrow keys as ANSI escape sequences
 * (ESC [ A/B/C/D) -- enough to drive GRUB menus, a serial login, and TUIs.
 * Break codes update modifier state and emit nothing.
 */

/* Max bytes one scancode can produce (an arrow key = ESC '[' 'A' = 3). */
#define HYPE_KBD_DECODE_MAX_OUT 3u

typedef struct {
    uint8_t shift; /* either Shift currently held */
    uint8_t ctrl;  /* either Ctrl currently held */
    uint8_t e0;    /* an 0xE0 prefix was just seen; the next byte is extended */
} hype_kbd_decode_t;

/* Clear modifier/prefix state. Call at (re)start. */
void hype_kbd_decode_reset(hype_kbd_decode_t *d);

/*
 * Feed one Set-1 scancode byte. Writes up to `out_cap` produced bytes into
 * `out` and returns the count -- 0 when the byte only updated modifier/prefix
 * state, was a break (key-release) of a non-modifier, or was unmapped.
 * `out_cap` should be >= HYPE_KBD_DECODE_MAX_OUT. Pure -- no I/O.
 */
unsigned hype_kbd_decode_feed(hype_kbd_decode_t *d, uint8_t scancode, uint8_t *out,
                              unsigned out_cap);

#endif /* HYPE_CORE_KBD_DECODE_H */
