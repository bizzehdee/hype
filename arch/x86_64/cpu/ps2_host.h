#ifndef HYPE_ARCH_PS2_HOST_H
#define HYPE_ARCH_PS2_HOST_H

#include <stdint.h>

#include "isr.h"
#include "timer.h"

/*
 * INPUT-3: host-level PS/2 keyboard ownership. Once M1-4's
 * ExitBootServices() has run, UEFI's own Simple Text Input Protocol is
 * gone for good -- this project's own host (the "only kernel" from
 * that point on, per M1-4's own scope note) must read the real i8042
 * controller directly for its own purposes (the dashboard leader
 * chord, INPUT-4), the same way arch/x86_64/cpu/pic.c/pit_hw.c already
 * own the real PIC/PIT for the host's own timer tick. This module owns
 * raw scancode *capture* only -- deciding what a scancode sequence
 * *means* (the leader chord) is INPUT-4's own job, layered on top,
 * matching how devices/ps2_keyboard.h (the guest-facing model) and
 * INPUT-4's own recognizer are also kept separate.
 *
 * IRQ1's vector: this project's own PIC remap (M1-8,
 * hype_pic_remap_and_mask_all(HYPE_TIMER_VECTOR)) already puts IRQ0-7
 * at vectors HYPE_TIMER_VECTOR..+7, so IRQ1 (keyboard) lands at
 * HYPE_TIMER_VECTOR+1 -- no second remap call (which would re-mask
 * every line, undoing the timer's own unmask).
 *
 * Split the same way arch/x86_64/cpu/pit.c/pit_hw.c already are: this
 * header's own ring-buffer logic (reset/push/pop) is pure and fully
 * unit tested (core/tests/test_ps2_host.c); the real hardware access
 * (reading port 0x60, registering the ISR, unmasking the IRQ) lives in
 * ps2_host_hw.c, exempt from unit testing per AGENTS.md, same
 * reasoning as every other _hw.c file here.
 */

#define HYPE_HOST_KBD_IRQ 1
#define HYPE_HOST_KBD_VECTOR (HYPE_TIMER_VECTOR + 1)

/* A real i8042 has its own small internal FIFO; this project's own
 * host-side buffer just needs to hold more than one scancode between
 * successive polls of the host's own main loop, not model that FIFO's
 * exact depth. */
#define HYPE_HOST_KBD_BUFFER_SIZE 16u

typedef struct {
    uint8_t buffer[HYPE_HOST_KBD_BUFFER_SIZE];
    unsigned int head;
    unsigned int count;
} hype_host_kbd_buffer_t;

/* Resets to empty -- call once at host startup, same convention as
 * every other device model here. */
void hype_host_kbd_buffer_reset(hype_host_kbd_buffer_t *buf);

/* Pushes a scancode onto the buffer. Silently drops it if the buffer
 * is already full (the host's own main loop isn't draining fast
 * enough) rather than overwrite unread data or corrupt state --
 * matching devices/ps2_mouse.h's own queue-full behavior. */
void hype_host_kbd_buffer_push(hype_host_kbd_buffer_t *buf, uint8_t scancode);

/* Pops the oldest buffered scancode into *out_scancode. Returns 1 if a
 * byte was popped, 0 if the buffer was empty (*out_scancode
 * untouched). */
int hype_host_kbd_buffer_pop(hype_host_kbd_buffer_t *buf, uint8_t *out_scancode);

/*
 * Registers this host's own keyboard ISR (HYPE_HOST_KBD_VECTOR) and
 * unmasks IRQ1 on the real PIC -- call once at host startup, strictly
 * after hype_pic_remap_and_mask_all() has already run for M1-8's own
 * timer setup (this function does NOT remap/mask-all itself; see this
 * header's own top comment for why). Exempt from unit testing --
 * reaches into hype_isr_register()/hype_pic_unmask_irq(), both already
 * exempt/tested at their own layer.
 */
void hype_host_kbd_init(void);

/*
 * The registered ISR for HYPE_HOST_KBD_VECTOR: reads the real
 * scancode from port 0x60, pushes it onto this module's own global
 * buffer, and sends the PIC an EOI -- matching hype_timer_isr()'s own
 * exact shape. Exempt from unit testing: real inb/hype_pic_send_eoi(),
 * nothing to observe without real hardware; hype_host_kbd_buffer_push()
 * itself is already fully tested in isolation.
 */
void hype_host_kbd_isr(const hype_isr_frame_t *frame);

/*
 * Host-facing: pops the next buffered scancode, if any -- the API the
 * dashboard/leader-chord recognizer (INPUT-4) polls from its own main
 * loop. Returns 1 if a scancode was popped, 0 if none was pending.
 * Exempt from unit testing -- thin wrapper around this module's own
 * global buffer instance; hype_host_kbd_buffer_pop() itself is already
 * fully tested.
 */
int hype_host_kbd_poll_scancode(uint8_t *out_scancode);

#endif /* HYPE_ARCH_PS2_HOST_H */
