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
/*
 * #808: WHY the polled drain declined to push a byte.
 *
 * Boot AMD-L0 run 5 lost all host keyboard input twice (40 s, then the final 22.7 s) with hype's
 * loop still running -- `ps2reads` climbed 158,444 -> 243,916 while `polled` stayed frozen at
 * 112. The drain was running at full rate and taking nothing, and the counters could not say
 * which of its four non-pushing exits fired: they are indistinguishable in `polled` alone.
 *
 * Note what misled the first reading of that run: `fw-1 KBDPOLL` is the GUEST's view of ITS
 * virtual 0x64/0x60 (the #436 breadcrumbs in svm_vcpu.c, which is why it carries a guest RIP).
 * It says nothing about the physical controller. These counters are the host side, and they are
 * the ones that answer "why can the operator not type".
 */
static unsigned long long g_kbd_exit_floating;  /* st == 0xFF -- no controller answering */
static unsigned long long g_kbd_exit_empty;     /* OBF clear -- nothing waiting (normal) */
static unsigned long long g_kbd_exit_data_ff;   /* OBF set but data == 0xFF */
static unsigned long long g_kbd_exit_aux;       /* mouse byte: consumed and dropped */
static unsigned long long g_kbd_drain_calls;
static uint8_t g_kbd_last_st;                   /* the host status the drain last acted on */
static uint8_t g_kbd_last_data;                 /* ... and the byte it read, if it read one */
static uint64_t g_kbd_last_push_tsc;            /* when a byte last reached the buffer */
/*
 * #808, second probe. Run 7 settled that the drain discards nothing (floating/data_ff/aux all
 * zero over 330,553 calls, nocrl=0) and that IRQ1 carries ~95% of input on this machine -- 578
 * ISR entries against ps2polled=34. Input dies when the ISR stops, and the poll does not pick up
 * the slack, which is the one job it exists for.
 *
 * The remaining question is whether the input path is being STARVED rather than broken. The i8042
 * buffers exactly one byte, so any window in which neither the ISR runs nor the drain is called
 * loses every keystroke after the first. The BSP has windows like that: run 7 recorded a 162 ms
 * flush slice against a 10 ms budget and ended with the USB pool at usb_held=64/64.
 *
 * So measure the gap between consecutive drains directly. A max gap of tens of milliseconds is
 * the 4 kHz gate working; hundreds is the answer.
 */
static uint64_t g_kbd_gap_max_ticks;            /* longest interval between two drain calls */
static uint64_t g_kbd_gap_prev_tsc;
static unsigned long long g_kbd_isr_obf_clear;  /* ISR entries that found nothing waiting */
/*
 * #808, third refinement. Run 8's gap_max was 159 ms and was set ONCE, between the drain's first
 * call and its 18,582nd -- bring-up -- then never moved across the remaining 289,000 calls. A
 * lifetime maximum cannot tell that apart from a 159 ms stall every second, so it was the wrong
 * statistic: it answered "did this ever happen" when the question is "is it happening now".
 *
 * gap_over_5ms counts every window long enough to lose a keystroke (a PS/2 byte is ~1 ms on the
 * wire and the i8042 buffers one), and gap_recent_max resets each time the diagnostic reads it,
 * so a sample describes the interval it covers rather than the whole boot.
 *
 * isr_last_tsc is the one that speaks to the actual failure. Run 5's input death was
 * isr_entries frozen at 107 with `+0 since last` -- the ISR stopped. "The ISR has not fired for
 * N ms" is that signature stated directly, visible in the sample where it happens instead of
 * inferred afterwards from a counter that stopped moving.
 */
static unsigned long long g_kbd_gap_over_5ms;
static uint64_t g_kbd_gap_recent_max_ticks;
static uint64_t g_kbd_isr_last_tsc;

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
    g_kbd_isr_last_tsc = kbd_rdtsc(); /* #808: so a sample can say how long since the last IRQ1 */
    if (st == 0xFFu || (st & HYPE_PS2_HOST_STATUS_OBF) == 0u) {
        g_kbd_isr_obf_clear++; /* #808: fired, but nothing was waiting */
    }
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
        g_kbd_last_st = st; /* #808 */
        if (st == 0xFFu) {
            g_kbd_no_controller = 1u;
            g_kbd_exit_floating++; /* #808 */
            __asm__ volatile("pushq %0; popfq" ::"r"(flags) : "memory", "cc");
            return;
        }
        if ((st & HYPE_PS2_HOST_STATUS_OBF) == 0u) {
            g_kbd_exit_empty++; /* #808 */
            __asm__ volatile("pushq %0; popfq" ::"r"(flags) : "memory", "cc");
            return; /* nothing waiting -- the normal exit */
        }
        data = inb(HYPE_PS2_HOST_PORT_DATA);
        g_kbd_last_data = data; /* #808 */
        __asm__ volatile("pushq %0; popfq" ::"r"(flags) : "memory", "cc");
        if (data == 0xFFu && (st & HYPE_PS2_HOST_STATUS_AUX) == 0u) {
            g_kbd_exit_data_ff++; /* #808 */
            /* A keyboard never sends 0xFF as a make/break code; it is the floating bus again,
             * or a controller error byte. Consume it and stop rather than feed it upstream. */
            return;
        }
        if ((st & HYPE_PS2_HOST_STATUS_AUX) != 0u) {
            g_kbd_exit_aux++; /* #808 */
            continue; /* mouse byte: consumed so it cannot block the keyboard, then dropped */
        }
        g_kbd_polled_bytes++;
        g_kbd_last_push_tsc = kbd_rdtsc(); /* #808 */
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
    g_kbd_drain_calls++; /* #808 */
    {   /* #808: how long the input path went unserviced. See g_kbd_gap_max_ticks. */
        uint64_t nowg = kbd_rdtsc();
        if (g_kbd_gap_prev_tsc != 0ull) {
            uint64_t gap = nowg - g_kbd_gap_prev_tsc;
            if (gap > g_kbd_gap_max_ticks) g_kbd_gap_max_ticks = gap;
            if (gap > g_kbd_gap_recent_max_ticks) g_kbd_gap_recent_max_ticks = gap;
            /* 5 ms: a PS/2 byte is ~1 ms on the wire and the i8042 buffers one, so a window
             * this long has already lost anything typed into it after the first byte. */
            if (g_kbd_poll_interval_ticks != 0ull && gap > g_kbd_poll_interval_ticks * 20ull) {
                g_kbd_gap_over_5ms++;
            }
        }
        g_kbd_gap_prev_tsc = nowg;
    }
    host_kbd_drain_polled();
    g_kbd_drain_busy = 0u;
    return hype_host_kbd_buffer_pop(&g_host_kbd_buffer, out_scancode);
}

uint64_t hype_host_kbd_polled_bytes(void) { return g_kbd_polled_bytes; }

/*
 * #808: drain the i8042 into the software buffer WITHOUT consuming a scancode.
 *
 * The i8042 holds exactly one byte, so a long operation on the BSP loses every keystroke after
 * the first. Boot AMD-L0 run 11 named the operation: fw_1_vars_service() at 164 ms
 * (BSPSTARVE `vars=5(max 164ms)`), against the log flush honouring its own 10 ms slice budget
 * (`flush=7(max 10ms)`) -- so the answer is to give the varstore write the same treatment, and
 * to pump the keyboard between its chunks.
 *
 * Deliberately drain-only. The consumer is fw_1_host_input_poll() on the input phase and it
 * stays the only consumer: calling the full poll_scancode() from inside a write path would pop
 * a byte with nobody to route it, dropping the keystroke this exists to save. Bytes sit in
 * g_host_kbd_buffer until the input phase reads them, which is exactly what the buffer is for.
 *
 * Safe to call from anywhere on the owning core: it touches ports 0x60/0x64 only -- no USB, so
 * no re-entry into the transfer lock it is being called from underneath -- and
 * host_kbd_drain_polled() is already guarded against re-entry by g_kbd_drain_busy.
 */
void hype_host_kbd_pump(void) {
    if (g_kbd_no_controller || g_kbd_drain_busy) {
        return;
    }
    if (g_kbd_poll_interval_ticks != 0ull) {
        uint64_t now = kbd_rdtsc();
        if (now - g_kbd_poll_last_tsc < g_kbd_poll_interval_ticks) {
            return;
        }
        g_kbd_poll_last_tsc = now;
    }
    g_kbd_drain_busy = 1u;
    g_kbd_drain_calls++;
    {
        uint64_t nowg = kbd_rdtsc();
        if (g_kbd_gap_prev_tsc != 0ull) {
            uint64_t gap = nowg - g_kbd_gap_prev_tsc;
            if (gap > g_kbd_gap_max_ticks) g_kbd_gap_max_ticks = gap;
            if (gap > g_kbd_gap_recent_max_ticks) g_kbd_gap_recent_max_ticks = gap;
            if (g_kbd_poll_interval_ticks != 0ull && gap > g_kbd_poll_interval_ticks * 20ull) {
                g_kbd_gap_over_5ms++;
            }
        }
        g_kbd_gap_prev_tsc = nowg;
    }
    host_kbd_drain_polled();
    g_kbd_drain_busy = 0u;
}
void hype_host_kbd_drain_stats(hype_host_kbd_drain_stats_t *out) {
    if (out == 0) return;
    out->calls = g_kbd_drain_calls;
    out->exit_floating = g_kbd_exit_floating;
    out->exit_empty = g_kbd_exit_empty;
    out->exit_data_ff = g_kbd_exit_data_ff;
    out->exit_aux = g_kbd_exit_aux;
    out->last_status = g_kbd_last_st;
    out->last_data = g_kbd_last_data;
    out->last_push_tsc = g_kbd_last_push_tsc;
    out->no_controller = g_kbd_no_controller;
    out->gap_max_ticks = g_kbd_gap_max_ticks;
    out->isr_obf_clear = g_kbd_isr_obf_clear;
    out->pic_imr = hype_pic_read_master_imr();
    out->gap_over_thresh = g_kbd_gap_over_5ms;
    out->gap_recent_max_ticks = g_kbd_gap_recent_max_ticks;
    out->isr_last_tsc = g_kbd_isr_last_tsc;
    /* Reading clears the recent window, so the NEXT sample describes only its own interval.
     * Deliberately a side effect of the read: two readers would each see part of the interval,
     * and there is exactly one reader. */
    g_kbd_gap_recent_max_ticks = 0ull;
}

void hype_host_kbd_inject_scancode(uint8_t scancode) {
    /* Same buffer the ISR pushes into -- see the header for why USB HID joins the
     * queue rather than adding a parallel path. */
    hype_host_kbd_buffer_push(&g_host_kbd_buffer, scancode);
}
