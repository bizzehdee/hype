#ifndef HYPE_DEVICES_VT_FILTER_H
#define HYPE_DEVICES_VT_FILTER_H

#include <stdint.h>

/*
 * FW-1e: a tiny VT/ANSI escape-sequence stripper. OVMF's serial console
 * is TerminalDxe's VT-UTF8 terminal, which interleaves the shell's
 * visible text with escape sequences (ESC[...m colours, ESC[y;xH cursor
 * moves, screen clears). hype's GOP text console (core/gop_text.h) is a
 * plain glyph renderer with no escape interpretation, so forwarding raw
 * terminal bytes would show the escapes as literal garbage. This filter
 * passes visible text (and newlines) and drops CSI / two-char ESC
 * sequences, so the shell output is legible on screen. Full terminal
 * emulation (cursor addressing, colours) is the later TERM milestone's
 * job; this is just "make it readable".
 *
 * Byte-at-a-time state machine, pure -- no I/O.
 */

#define HYPE_VT_STATE_NORMAL 0
#define HYPE_VT_STATE_ESC 1 /* saw ESC (0x1B), awaiting '[' or a 2-char final */
#define HYPE_VT_STATE_CSI 2 /* inside ESC[ ... , consume until a final byte */

typedef struct {
    int state;
} hype_vt_filter_t;

void hype_vt_filter_reset(hype_vt_filter_t *f);

/*
 * Feed one input byte. Returns 1 and writes the byte to emit into *out
 * if it is visible text to display; returns 0 if the byte was consumed
 * as part of (or dropped by) an escape sequence / non-printable. Tab is
 * mapped to a space; '\n' passes through; '\r', backspace, other
 * control bytes and all CSI/ESC sequences are dropped.
 */
int hype_vt_filter(hype_vt_filter_t *f, uint8_t in, char *out);

#endif /* HYPE_DEVICES_VT_FILTER_H */
