#include "pic.h"
#include "ps2_host.h"

#define HYPE_PS2_HOST_PORT_DATA 0x60u
/* #218: the i8042 status port. Bit 0 (OBF) means a byte is waiting in the data port; bit 5
 * distinguishes a mouse byte from a keyboard byte on a controller with a second channel. */
#define HYPE_PS2_HOST_PORT_STATUS 0x64u
#define HYPE_PS2_HOST_STATUS_OBF 0x01u
#define HYPE_PS2_HOST_STATUS_AUX 0x20u

static hype_host_kbd_buffer_t g_host_kbd_buffer;
/*
 * #796: the polled drain is rate-limited. One inb(0x64) per BSP loop iteration cost 20 us on
 * the i5-13420H (the i8042 lives behind the embedded controller there) at 42,000 iterations
 * a second -- 84% of the BSP, measured as `BSPCOST input 82% mean=20us`. The desktop's i8042
 * answers in 1 us, which is why the same loop was harmless on the 5950X. A PS/2 byte takes
 * about 1 ms on the wire, so 4 kHz polling cannot miss one; the IRQ path fills the buffer
 * regardless of this gate. Ticks are TSC; 0 = no gate (bring-up callers before hz is known).
 */
static uint64_t g_kbd_poll_interval_ticks;
static uint64_t g_kbd_poll_last_tsc;
static unsigned long long g_kbd_status_reads;
static uint64_t g_kbd_status_read_max_ticks;

static inline uint64_t kbd_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void hype_host_kbd_set_poll_interval(uint64_t tsc_ticks) { g_kbd_poll_interval_ticks = tsc_ticks; }

void hype_host_kbd_poll_stats(unsigned long long *status_reads, uint64_t *max_read_ticks) {
    if (status_reads) *status_reads = g_kbd_status_reads;
    if (max_read_ticks) *max_read_ticks = g_kbd_status_read_max_ticks;
}

/*
 * #363: is the host keyboard interrupt still being SERVICED, and on which core?
 *
 * Two fixes to this ticket addressed the wrong layer -- rendering, then poll location -- because I
 * reasoned about where code runs instead of measuring where the interrupt lands. Polling is
 * non-blocking and does no I/O; it pops a buffer that only this ISR fills. So if the ISR stops
 * being entered, every core polls an empty buffer forever and the keyboard is dead machine-wide,
 * whichever core is doing the polling.
 *
 * The suspected mechanism is that a host IRQ arriving while an AP runs a guest is serviced ON that
 * AP, so a wedged AP leaves the 8259 without an EOI and no further keyboard interrupt is delivered
 * anywhere. That is a hypothesis. These counters test it: if `entries` stops advancing at the
 * moment the guest wedges, it is right and the fix is to route IRQ1 at the BSP; if it keeps
 * advancing, the fault is downstream and a routing change would be wasted work.
 *
 * `last_apic` names the core that most recently ran the ISR, which is the part that distinguishes
 * "serviced somewhere" from "serviced on the core that later died".
 */
static volatile unsigned long long g_kbd_isr_entries;
static volatile unsigned long long g_kbd_isr_eois;
static volatile unsigned int g_kbd_isr_last_apic;

static inline unsigned int kbd_this_apic(void) {
    return (*(volatile uint32_t *)(uintptr_t)0xFEE00020u) >> 24;
}

void hype_host_kbd_isr_stats(unsigned long long *entries, unsigned long long *eois,
                             unsigned int *last_apic) {
    if (entries != 0) *entries = g_kbd_isr_entries;
    if (eois != 0) *eois = g_kbd_isr_eois;
    if (last_apic != 0) *last_apic = g_kbd_isr_last_apic;
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* #218: bytes taken by the polling fallback, so a hardware run can tell "the poll found
 * nothing" from "the poll never ran" -- the distinction that cost this bug a whole session. */
static uint64_t g_kbd_polled_bytes;
/* Set once the status port reads 0xFF: no i8042 is answering, so stop probing it entirely
 * rather than paying an I/O port read per BSP loop iteration forever. */
static uint8_t g_kbd_no_controller;
static uint8_t g_kbd_drain_busy;

void hype_host_kbd_init(void) {
    hype_host_kbd_buffer_reset(&g_host_kbd_buffer);
    hype_isr_register(HYPE_HOST_KBD_VECTOR, hype_host_kbd_isr);
    hype_pic_unmask_irq(HYPE_HOST_KBD_IRQ);
}

void hype_host_kbd_isr(const hype_isr_frame_t *frame) {
    /*
     * #490: check OBF before reading. Reading port 0x60 with the output buffer EMPTY returns
     * the PREVIOUS byte again -- and the buffer can legitimately be empty here, because the
     * #218 poll path races this ISR for each byte and sometimes wins. The unconditional read
     * pushed that stale repeat as a fresh scancode, doubling keystrokes on every host whose
     * IRQ1 works (measured: 'sseett  aa  llaabbeell' typed as 'set a label'). A status of
     * 0xFF is the floating bus, same as the poll path treats it.
     */
    uint8_t st = inb(HYPE_PS2_HOST_PORT_STATUS);
    (void)frame;
    g_kbd_isr_entries++;
    g_kbd_isr_last_apic = kbd_this_apic();
    if (st != 0xFFu && (st & HYPE_PS2_HOST_STATUS_OBF) != 0u) {
        uint8_t scancode = inb(HYPE_PS2_HOST_PORT_DATA);
        if ((st & HYPE_PS2_HOST_STATUS_AUX) == 0u) {
            hype_host_kbd_buffer_push(&g_host_kbd_buffer, scancode);
        } /* else: mouse byte -- consumed so it cannot wedge the keyboard stream, dropped */
    }
    hype_pic_send_eoi(HYPE_HOST_KBD_IRQ);
    g_kbd_isr_eois++;
}

/*
 * #218: drain the i8042 output buffer by POLLING, with no dependency on IRQ1.
 *
 * The host keyboard was interrupt-only: hype_host_kbd_isr() pushed into the buffer and
 * hype_host_kbd_poll_scancode() popped from it. On the operator's laptop IRQ1 never fires --
 * a full real-hardware run measured isr_entries=0 eois=0 -- so that buffer stayed empty and
 * no key ever reached hype. USB HID was the workaround, but the same run's inventory found no
 * HID device at all (a mass-storage stick, a card reader, Bluetooth and a camera), so with
 * both paths dead the machine had no keyboard input whatsoever.
 *
 * Polling the status port needs no interrupt routing, no PIC unmask and no IOAPIC entry, which
 * is what makes it work where the ISR does not. Bytes are pushed into the SAME buffer the ISR
 * uses, so ordering and the existing consumer are unchanged, and a machine where IRQ1 DOES
 * work simply finds the buffer already filled.
 *
 * Bounded per call: a stuck controller that always reports OBF must not spin here forever --
 * this runs on the BSP's render/input loop, which also drives the dashboard.
 */
static void host_kbd_drain_polled(void) {
    unsigned guard;
    for (guard = 0; guard < 8u; guard++) {
        uint8_t st;
        uint8_t data;
        /*
         * #490: the status+data pair must be atomic against hype_host_kbd_isr -- IRQ1 landing
         * BETWEEN them consumes the byte in the ISR, and the poll's data read then returns the
         * previous byte again, pushed as a duplicate. Measured as the FIRST keystroke of a run
         * doubling even after the ISR gained its own OBF check. Interrupts are masked only
         * around the two port reads.
         */
        unsigned long long flags;
        uint64_t t0;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags)::"memory");
        t0 = kbd_rdtsc();
        st = inb(HYPE_PS2_HOST_PORT_STATUS);
        {   /* #796: how long one status read takes on THIS machine, so the cost is a number */
            uint64_t dt = kbd_rdtsc() - t0;
            g_kbd_status_reads++;
            if (dt > g_kbd_status_read_max_ticks) g_kbd_status_read_max_ticks = dt;
        }
        /*
         * 0xFF is the floating bus -- no i8042 is answering at all. Bit 0 of 0xFF is SET, so a
         * naive OBF test reads "data is waiting" forever and returns 0xFF as a scancode every
         * time. That is not hypothetical: the first cut of this shipped without the check and
         * flooded the input path on real hardware, which doubled and tripled every keystroke,
         * starved the BSP's render loop so neither guest drew to the screen, and truncated
         * hype's own log mid-line because the flush never ran again.
         */
        if (st == 0xFFu) {
            g_kbd_no_controller = 1u;
            __asm__ volatile("pushq %0; popfq" ::"r"(flags) : "memory", "cc");
            return;
        }
        if ((st & HYPE_PS2_HOST_STATUS_OBF) == 0u) {
            __asm__ volatile("pushq %0; popfq" ::"r"(flags) : "memory", "cc");
            return; /* nothing waiting -- the normal exit */
        }
        data = inb(HYPE_PS2_HOST_PORT_DATA);
        __asm__ volatile("pushq %0; popfq" ::"r"(flags) : "memory", "cc");
        if (data == 0xFFu && (st & HYPE_PS2_HOST_STATUS_AUX) == 0u) {
            /* A keyboard never sends 0xFF as a make/break code; it is the floating bus again,
             * or a controller error byte. Consume it and stop rather than feed it upstream. */
            return;
        }
        if ((st & HYPE_PS2_HOST_STATUS_AUX) != 0u) {
            continue; /* mouse byte: consumed so it cannot block the keyboard, then dropped */
        }
        g_kbd_polled_bytes++;
        hype_host_kbd_buffer_push(&g_host_kbd_buffer, data);
    }
}

int hype_host_kbd_poll_scancode(uint8_t *out_scancode) {
    if (hype_host_kbd_buffer_pop(&g_host_kbd_buffer, out_scancode)) {
        return 1;
    }
    /*
     * Drain at most once per caller-driven pass, and never from a controller that has already
     * been found absent. The caller is `while (hype_host_kbd_poll_scancode(&sc))` on the BSP's
     * tight render/input spin, so an unconditional drain here is called thousands of times a
     * second and re-probes the port every time -- which is how the floating-bus case became an
     * endless loop rather than a single bad read.
     */
    if (g_kbd_no_controller) {
        return 0;
    }
    /*
     * #582: THE POLL RUNS EVEN WHERE IRQ1 WORKS, and this used to be the opposite.
     *
     * #490 disabled polling the moment a single IRQ1 was taken -- "one taken IRQ1 proves the
     * interrupt path works; stop polling" -- to end a poll/ISR race in which the loser's read of
     * port 0x60 returned the previous byte again, doubling every keystroke.
     *
     * The race was real and the conclusion was too strong. "The interrupt path works" is not "the
     * interrupt path is SUFFICIENT". The i8042's output buffer holds ONE byte: if the BSP does not
     * take IRQ1 before the next key arrives, the controller drops it, and the BSP has plenty to be
     * busy with -- rendering, and a USB log flush that measurably falls behind. One taken interrupt
     * therefore disabled the safety net for the whole run.
     *
     * Measured on the QEMU rig at one-second key spacing: isr_entries=4, polled=4, against 18
     * make/break events sent. FOURTEEN OF EIGHTEEN KEYS LOST -- and a typed command missing 14 of
     * its 18 events never forms a word, so it reads as "typed commands do not work" rather than as
     * dropped input. That cost two runs on #177 before the spacing was suspected.
     *
     * Polling alongside a working ISR is safe now, and that is what changed: #490 also made the
     * status+data pair ATOMIC in host_kbd_drain_polled() by masking interrupts across it, and the
     * ISR does its own OBF check. Two readers, each testing OBF atomically with its read, cannot
     * both take the same byte -- which is what the doubling was. The early return was belt and
     * braces on top of a fix that had already removed the cause, and it cost correctness.
     */
    if (g_kbd_drain_busy) {
        return 0; /* re-entered from within the caller's own drain loop */
    }
    if (g_kbd_poll_interval_ticks != 0ull) { /* #796 */
        uint64_t now = kbd_rdtsc();
        if (now - g_kbd_poll_last_tsc < g_kbd_poll_interval_ticks) {
            return 0;
        }
        g_kbd_poll_last_tsc = now;
    }
    g_kbd_drain_busy = 1u;
    host_kbd_drain_polled();
    g_kbd_drain_busy = 0u;
    return hype_host_kbd_buffer_pop(&g_host_kbd_buffer, out_scancode);
}

uint64_t hype_host_kbd_polled_bytes(void) { return g_kbd_polled_bytes; }

void hype_host_kbd_inject_scancode(uint8_t scancode) {
    /* Same buffer the ISR pushes into -- see the header for why USB HID joins the
     * queue rather than adding a parallel path. */
    hype_host_kbd_buffer_push(&g_host_kbd_buffer, scancode);
}
