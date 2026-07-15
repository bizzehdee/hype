#ifndef HYPE_DEVICES_PS2_MOUSE_H
#define HYPE_DEVICES_PS2_MOUSE_H

#include <stdint.h>

/*
 * Guest-visible PS/2 mouse, the i8042 controller's "auxiliary" device
 * channel (INPUT-2, plan.md §6c: "emulate a PS/2 keyboard ... and a
 * PS/2 mouse in the device model"). Shares the SAME controller ports
 * (0x60 data, 0x64 status/command) devices/ps2_keyboard.h already
 * owns -- routing a given 0x60 byte to this module instead of the
 * keyboard is the controller's own job (INPUT-1's `hype_ps2_kbd_t`
 * itself, extended with a "next data write targets AUX" flag set by
 * the 0xD4 controller command; see that header's own comment), not
 * this module's.
 *
 * Real hardware convention: mouse responses/data are read back one
 * byte at a time via the SAME port 0x60 a keyboard scancode would use
 * (distinguished on real hardware by status register bit 5, "auxiliary
 * output buffer full" -- this project's own exempt glue decides which
 * channel's byte is currently ready to read, matching how it already
 * decides which channel a write targets). Unlike the keyboard's own
 * single-pending-byte scope, several real mouse responses are
 * multi-byte (RESET's own ACK+self-test-pass+device-ID, and every
 * movement report's own 3-byte packet) and must be read back in
 * order, so this module keeps a small FIFO rather than a single
 * buffered byte.
 */

#define HYPE_PS2_MOUSE_CMD_RESET 0xFFu
#define HYPE_PS2_MOUSE_CMD_SET_DEFAULTS 0xF6u
#define HYPE_PS2_MOUSE_CMD_ENABLE_REPORTING 0xF4u
#define HYPE_PS2_MOUSE_CMD_DISABLE_REPORTING 0xF5u
#define HYPE_PS2_MOUSE_CMD_GET_DEVICE_ID 0xF2u

#define HYPE_PS2_MOUSE_ACK 0xFAu
#define HYPE_PS2_MOUSE_SELF_TEST_PASSED 0xAAu
#define HYPE_PS2_MOUSE_DEVICE_ID_STANDARD 0x00u

/* Enough for RESET's own 3-byte response, or one full 3-byte movement
 * packet, with room to spare -- this project's own scope never queues
 * more than one such response at a time. */
#define HYPE_PS2_MOUSE_QUEUE_SIZE 8u

typedef struct {
    uint8_t queue[HYPE_PS2_MOUSE_QUEUE_SIZE];
    unsigned int head;
    unsigned int count;
    int reporting_enabled;
} hype_ps2_mouse_t;

/* Resets to power-on state: empty queue, reporting disabled (a real
 * mouse doesn't stream movement until told to) -- call on every
 * (re)start, same convention as every other device model here. */
void hype_ps2_mouse_reset(hype_ps2_mouse_t *mouse);

/* True if a byte is queued and ready for the next port 0x60 read. */
int hype_ps2_mouse_has_pending_byte(const hype_ps2_mouse_t *mouse);

/* Pops and returns the front of the queue -- 0 if nothing is queued
 * (callers should check hype_ps2_mouse_has_pending_byte() first, same
 * as every other "don't call me with nothing ready" convention this
 * project already follows elsewhere). */
uint8_t hype_ps2_mouse_read_byte(hype_ps2_mouse_t *mouse);

/*
 * A command byte routed here by the controller (the 0x60 write that
 * followed a 0xD4 on 0x64). Queues whatever response(s) a real mouse
 * would send: RESET queues ACK+self-test-pass+device-ID and disables
 * reporting (matching real power-on-reset semantics); ENABLE/DISABLE
 * REPORTING and GET_DEVICE_ID queue their own real responses;
 * anything else this stub doesn't implement more specifically is
 * still generically ACKed, matching devices/ps2_keyboard.h's own
 * tolerance for commands a given implementation doesn't support.
 */
void hype_ps2_mouse_write_command(hype_ps2_mouse_t *mouse, uint8_t command);

/*
 * Host-facing: a real mouse movement/button event arrives as the
 * standard PS/2 3-byte packet (status byte -- button state plus sign/
 * overflow bits -- then X and Y movement deltas). Queued only if
 * reporting is currently enabled (a real mouse that hasn't been told
 * to start streaming doesn't send unsolicited packets); silently
 * dropped otherwise, matching real hardware. Does not raise IRQ12
 * itself -- that needs a vcpu context this pure module never touches
 * (the exempt glue's job, matching devices/ps2_keyboard.h's own
 * precedent for scancodes/IRQ1).
 */
void hype_ps2_mouse_enqueue_movement(hype_ps2_mouse_t *mouse, uint8_t status, uint8_t dx, uint8_t dy);

#endif /* HYPE_DEVICES_PS2_MOUSE_H */
