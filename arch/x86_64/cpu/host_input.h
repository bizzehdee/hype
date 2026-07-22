#ifndef HYPE_CPU_HOST_INPUT_H
#define HYPE_CPU_HOST_INPUT_H

#include <stdint.h>
#include "leader_chord.h"
#include "../../../core/kbd_decode.h"

/*
 * TERM-4 (step 1): the routing core of the interactive per-VM terminal. Feeds a
 * stream of host PS/2 Set-1 scancodes and splits it two ways:
 *   - leader chords (Right-Ctrl+Right-Alt+key, INPUT-4) -> management actions
 *     (switch VM, toggle dashboard, ...), consumed and reported to the caller;
 *   - everything else -> typed bytes (kbd_decode) for the FOCUSED guest, which
 *     the caller enqueues into that guest's serial RX / PS/2.
 *
 * Composition detail: while the leader is engaged (both right modifiers held)
 * keys belong to the chord and are swallowed -- including the arrow keys, which
 * are guest input (ESC [ C/D) when the leader is NOT held but CYCLE_PREV/NEXT
 * when it is. The kbd_decode extended-prefix latch is cleared on any swallow so
 * a swallowed E0 sequence can't desync the next typed key.
 *
 * Pure logic (no hardware, no focus/enqueue policy) -- the SOURCE of scancodes
 * (PS/2 host controller now; a USB HID backend, TERM-5, later) and the routing
 * of the decoded bytes to a specific guest live in the caller, so USB slots in
 * behind this unchanged.
 */

typedef struct {
    hype_kbd_decode_t dec;
    hype_chord_state_t chord;
} hype_host_input_t;

void hype_host_input_reset(hype_host_input_t *hi);

/*
 * Feed one host scancode. Returns the leader-chord result (action ==
 * HYPE_CHORD_ACTION_NONE when the byte didn't complete a chord). When no chord
 * fired and the leader isn't engaged, the key is decoded to typed bytes for the
 * focused guest: up to `out_cap` bytes written to `out`, count in `*n_out`.
 * `*n_out` is 0 for a swallowed byte (leader engaged / chord in progress /
 * modifier / unmapped). `out_cap` should be >= HYPE_KBD_DECODE_MAX_OUT. Pure.
 */
hype_chord_result_t hype_host_input_feed(hype_host_input_t *hi, uint8_t scancode, uint8_t *out,
                                         unsigned out_cap, unsigned *n_out);

#endif /* HYPE_CPU_HOST_INPUT_H */
