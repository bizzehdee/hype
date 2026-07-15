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

/* Controller command byte (port 0x64 write) values this project
 * actually recognizes. */
#define HYPE_PS2_CMD_READ_CONFIG_BYTE 0x20u
#define HYPE_PS2_CMD_WRITE_CONFIG_BYTE 0x60u
#define HYPE_PS2_CMD_DISABLE_KEYBOARD_PORT 0xADu
#define HYPE_PS2_CMD_ENABLE_KEYBOARD_PORT 0xAEu
#define HYPE_PS2_CMD_SELF_TEST 0xAAu
#define HYPE_PS2_CMD_INTERFACE_TEST 0xABu

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

#endif /* HYPE_DEVICES_PS2_KEYBOARD_H */
