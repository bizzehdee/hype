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

/* #218: how many bytes the IRQ-independent polling fallback has taken from the i8042. */
uint64_t hype_host_kbd_polled_bytes(void);

/* #796: gate the polled i8042 status read to one per `tsc_ticks` (0 = every call). The IRQ
 * path is unaffected. Set by the BSP once the TSC rate is known. */
void hype_host_kbd_set_poll_interval(uint64_t tsc_ticks);
/* #796: status reads issued and the slowest single read, in TSC ticks. */
void hype_host_kbd_poll_stats(unsigned long long *status_reads, uint64_t *max_read_ticks);

/*
 * #808: why the polled drain took no byte.
 *
 * `polled` advancing means input works; `polled` frozen while `ps2reads` climbs means the drain
 * is running and declining, and its four non-pushing exits are indistinguishable in `polled`
 * alone. Boot AMD-L0 run 5 lost all host keyboard input twice with hype's loop still running and
 * this was unanswerable from the log.
 *
 * `exit_empty` is the normal case -- OBF clear, nothing waiting -- so `exit_empty` climbing
 * alone while the operator types means the byte never reached the i8042 at all, and the fault is
 * below hype. Any of the other three climbing means hype is discarding it, and `last_status` /
 * `last_data` say what it saw.
 *
 * NOT to be confused with the `fw-1 KBDPOLL` log line, which is the GUEST's view of ITS virtual
 * 0x64/0x60 (the #436 breadcrumbs in svm_vcpu.c -- it carries a guest RIP) and says nothing about
 * the physical controller.
 */
typedef struct {
    unsigned long long calls;         /* drain entries */
    unsigned long long exit_floating; /* st == 0xFF: no controller answering; latches no_controller */
    unsigned long long exit_empty;    /* OBF clear: nothing waiting -- the normal exit */
    unsigned long long exit_data_ff;  /* OBF set, data == 0xFF: consumed and dropped */
    unsigned long long exit_aux;      /* mouse byte: consumed and dropped */
    unsigned char last_status;        /* host port 0x64 the drain last acted on */
    unsigned char last_data;          /* host port 0x60 it read, when it read one */
    unsigned char no_controller;      /* the sticky latch: once set, the drain never runs again */
    uint64_t last_push_tsc;           /* when a byte last reached the buffer */
} hype_host_kbd_drain_stats_t;

void hype_host_kbd_drain_stats(hype_host_kbd_drain_stats_t *out);

/*
 * USB-5 (#217): push a scancode into the SAME host queue the PS/2 ISR feeds.
 *
 * This is the whole integration point for USB HID keyboard input. A USB keyboard's
 * reports are translated to PS/2 Set-1 scancodes (core/usb_hid.c) and injected here,
 * so every existing consumer -- the leader-chord recognizer, the dashboard command
 * line, the #233 physical-write confirm gate -- works with no change at all and
 * cannot behave differently for a USB keyboard than for a PS/2 one.
 *
 * The alternative was a second poll call at every consumption site, which is where
 * the two paths would have drifted: a site that forgot the second call would silently
 * work for one keyboard type and not the other.
 */
void hype_host_kbd_inject_scancode(uint8_t scancode);

/*
 * #363: host keyboard ISR liveness. `entries` advancing proves the interrupt is still being
 * serviced; `last_apic` says by which core. Read this from a core that is NOT running a guest --
 * a counter printed from a wedged context reads as absence of the event, which has misled this
 * project three times.
 */
void hype_host_kbd_isr_stats(unsigned long long *entries, unsigned long long *eois,
                             unsigned int *last_apic);

#endif /* HYPE_ARCH_PS2_HOST_H */
