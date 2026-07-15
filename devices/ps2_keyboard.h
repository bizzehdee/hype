#ifndef HYPE_DEVICES_PS2_KEYBOARD_H
#define HYPE_DEVICES_PS2_KEYBOARD_H

#include <stdint.h>

/*
 * Guest-visible i8042 PS/2 controller + keyboard channel (INPUT-1,
 * plan.md §6c: "emulate a PS/2 keyboard ... in the device model").
 * Port layout (real, decades-old PC convention, the same "stable spec
 * knowledge, no empirical verification needed" reasoning devices/pci.h's
 * own top comment already uses for PCI): 0x60 (data) and 0x64
 * (status on read, command on write).
 *
 * "Stub" scope, matching devices/pic.h's own precedent: enough of the
 * real controller-command surface for an actual OS driver's own
 * init/probe sequence to complete successfully (self-test, interface
 * test, enable/disable, read/write the configuration byte), plus the
 * single-pending-byte keyboard data path every scancode read/write
 * ultimately goes through. Only one scancode byte is ever buffered at
 * once (matching devices/cmos.h's own "single register at a time"
 * simplicity) -- a real 8042 has its own small internal FIFO, but
 * nothing in this project's own scope yet sends scancodes faster than
 * the guest can read them.
 *
 * Raising the actual IRQ1 a real key press signals is the exempt
 * glue's job (arch/x86_64/svm/svm_vcpu.c, via devices/pic.h's own
 * acknowledge-and-deliver path into INT-1/INT-2) -- this module only
 * ever touches its own buffered state, never a vcpu context.
 */

#define HYPE_PS2_PORT_DATA 0x60u
#define HYPE_PS2_PORT_STATUS_COMMAND 0x64u

/* Status register bits (port 0x64 read) this project actually models. */
#define HYPE_PS2_STATUS_OUTPUT_FULL (1u << 0) /* OBF -- a byte is waiting at port 0x60 */
#define HYPE_PS2_STATUS_SYSTEM_FLAG (1u << 2) /* set once POST/self-test has completed */
/* AUX_DATA (INPUT-2) -- real hardware's own bit distinguishing "the
 * byte currently waiting at port 0x60 came from the auxiliary (mouse)
 * channel, not the keyboard" -- this module doesn't know about the
 * mouse device at all (see this file's own top comment), so it never
 * sets this bit itself; only the exempt glue, which sees both device
 * instances, computes the fully combined status byte for a system
 * with a mouse actually attached. */
#define HYPE_PS2_STATUS_AUX_DATA (1u << 5)

/* Controller command byte (port 0x64 write) values this project
 * actually recognizes. */
#define HYPE_PS2_CMD_READ_CONFIG_BYTE 0x20u
#define HYPE_PS2_CMD_WRITE_CONFIG_BYTE 0x60u
#define HYPE_PS2_CMD_DISABLE_KEYBOARD_PORT 0xADu
#define HYPE_PS2_CMD_ENABLE_KEYBOARD_PORT 0xAEu
#define HYPE_PS2_CMD_SELF_TEST 0xAAu
#define HYPE_PS2_CMD_INTERFACE_TEST 0xABu
/* INPUT-2: auxiliary (mouse) port control -- 0xD4 is a one-shot prefix
 * consumed by hype_ps2_kbd_take_aux_data_write(): the very next 0x60
 * write should be routed to the mouse device instead of being
 * interpreted as a keyboard command. */
#define HYPE_PS2_CMD_DISABLE_AUX_PORT 0xA7u
#define HYPE_PS2_CMD_ENABLE_AUX_PORT 0xA8u
#define HYPE_PS2_CMD_TEST_AUX_PORT 0xA9u
#define HYPE_PS2_CMD_WRITE_TO_AUX 0xD4u

#define HYPE_PS2_AUX_TEST_PASSED 0x00u

#define HYPE_PS2_SELF_TEST_PASSED 0x55u
#define HYPE_PS2_INTERFACE_TEST_PASSED 0x00u

/* Keyboard-device (not controller) command bytes sent via port 0x60
 * while no controller command is awaiting its own data byte -- this
 * project acknowledges every one of these generically (HYPE_PS2_KBD_ACK),
 * matching a real keyboard's own behavior closely enough for a driver's
 * init sequence to proceed, without modeling scanning/LEDs/typematic
 * rate for real. */
#define HYPE_PS2_KBD_ACK 0xFAu

typedef struct {
    uint8_t data_buffer;
    int output_buffer_full; /* OBF -- data_buffer holds an unread byte */
    uint8_t config_byte;    /* controller configuration byte (0x20/0x60) */
    int awaiting_config_byte_write; /* 1 right after CMD_WRITE_CONFIG_BYTE, until the next 0x60 write */
    int keyboard_port_enabled;
    int aux_port_enabled;         /* INPUT-2: mouse port enable/disable (0xA8/0xA7) */
    int next_data_write_is_for_aux; /* INPUT-2: one-shot flag set by 0xD4, consumed by the next 0x60 write */
} hype_ps2_kbd_t;

/* Resets to power-on state: no pending byte, keyboard port enabled,
 * config byte 0 -- call on every (re)start, same convention as every
 * other device model here. */
void hype_ps2_kbd_reset(hype_ps2_kbd_t *kbd);

/*
 * Host-facing: a real key press/release event arrives as an
 * already-encoded scancode byte (the caller's job to encode Set 1/2/3
 * as appropriate -- this module is scancode-set-agnostic, it just
 * buffers whatever byte it's given). Overwrites any unread previous
 * byte (this project's own single-pending-byte scope) and sets OBF.
 * Does not raise IRQ1 itself -- that needs a vcpu context this pure
 * module never touches (the exempt glue's job, see this file's own
 * top comment).
 */
void hype_ps2_kbd_enqueue_scancode(hype_ps2_kbd_t *kbd, uint8_t scancode);

/*
 * Port 0x60 (data) and 0x64 (status/command) dispatch. Returns 0 if
 * `port` is one of these two (handled), non-zero otherwise -- the
 * same "always succeeds if it's mine" convention every other device
 * model here already follows.
 */
int hype_ps2_kbd_io_read(hype_ps2_kbd_t *kbd, uint16_t port, uint8_t *out_value);
int hype_ps2_kbd_io_write(hype_ps2_kbd_t *kbd, uint16_t port, uint8_t value);

/*
 * INPUT-2: true if THIS keyboard channel itself has an unread byte
 * waiting (mirrors devices/ps2_mouse.h's own
 * hype_ps2_mouse_has_pending_byte()) -- lets the exempt glue compute a
 * combined status byte across both channels when a real mouse is also
 * attached, instead of relying on hype_ps2_kbd_io_read()'s own
 * keyboard-only status computation.
 */
int hype_ps2_kbd_has_pending_byte(const hype_ps2_kbd_t *kbd);

/*
 * INPUT-2: returns 1 if the next 0x60 WRITE should be routed to the
 * AUX (mouse) device instead of being interpreted as a keyboard
 * command -- and clears the flag, since 0xD4's own real-hardware
 * convention is a one-shot prefix for exactly the next write. The
 * caller (exempt glue) must check this BEFORE calling
 * hype_ps2_kbd_io_write() for a 0x60 write.
 */
int hype_ps2_kbd_take_aux_data_write(hype_ps2_kbd_t *kbd);

#endif /* HYPE_DEVICES_PS2_KEYBOARD_H */
