/*
 * #541: INT-1/INT-2, ported out of boot/main.c.
 *
 * The in-binary test proved that hype could inject a vector into a guest -- with the HOST installing
 * the guest's GDT and IDT, forcing its CS/SS selectors, and requesting the interrupt directly. None
 * of that is what a guest does. What it actually validated was hype's injection primitive against
 * descriptor tables hype had built itself.
 *
 * This drives the whole path from inside the guest instead:
 *
 *   lgdt its own GDT, reload CS via a far return and SS/DS/ES from its own data descriptor
 *   lidt its own IDT with one real 64-bit interrupt gate
 *   remap the 8259 pair to vector base 0x20, exactly as a real OS does
 *   program the 8254 channel 0 as a rate generator
 *   unmask IRQ0, sti, and count handler entries with an EOI in each
 *
 * Everything there is a device hype models and a path a real guest uses. A failure now means one of
 * those models is wrong, rather than that hype's injection primitive is -- and the injection
 * primitive is still covered, because it is what delivers each tick.
 *
 * cmdline (#546): `ticks=N` sets how many are required. The default is deliberately small; a larger
 * value is a longer soak from the same artifact with no rebuild.
 */
#include "micro_idt.h"

#define NAME "intdeliver"

#define PIC_BASE 0x20u
#define IRQ0_VECTOR (PIC_BASE + 0u)

/* 100 Hz -- slow enough that a missing tick is obvious, fast enough that a handful arrive in the
 * time a microtest is given. */
#define PIT_DIVISOR (MICRO_PIT_HZ / 100u)

static volatile unsigned long long g_ticks;
static volatile unsigned long long g_wrong_vector;
/*
 * How many times control got past the HLT. Volatile so it counts what its name says -- it began as a
 * plain local, which at -O2 the compiler was free not to keep coherent.
 *
 * Making it volatile did NOT change the number, which is the interesting part: the guest reports
 * ~1154 deliveries against ONE resume past the HLT. Those should be close. #553 exists to explain
 * it and deliberately is not guessed at here; the most likely mechanism is that the HLT is not
 * retired before injection, so each iretq returns to the hlt itself, but that has not been
 * confirmed and this comment is not the place to assert it.
 *
 * It does not affect this test's verdict: what is asserted is that ticks arrive, on the vector IRQ0
 * was remapped to, and keep arriving. Both counts are reported so a change in their relationship is
 * visible rather than silent.
 */
static volatile unsigned long long g_resumes;

/*
 * The handler. Increments the counter and EOIs the master. Written in asm because a naked function
 * must not have a compiler prologue -- an interrupt frame is not a call frame, and a pushed
 * register would be popped as part of it.
 */
MICRO_ISR(irq0_isr,
          "incq g_ticks(%rip)\n\t"
          "movb $0x20, %al\n\t"
          "outb %al, $0x20\n\t")

/*
 * A gate for one OTHER vector, so a delivery that lands on the wrong vector is counted rather than
 * faulting. A not-present gate would #GP, which reports as a fault of unknown origin; a counted
 * wrong vector reports as "delivered, to the wrong place", which is a different and more useful
 * statement.
 */
MICRO_ISR(spurious_isr,
          "incq g_wrong_vector(%rip)\n\t"
          "movb $0x20, %al\n\t"
          "outb %al, $0x20\n\t")

static inline unsigned long long rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | (unsigned long long)lo;
}

static unsigned long long parse_uint(const char *s) {
    unsigned long long v = 0ull;
    if (s == 0) {
        return 0ull;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10ull + (unsigned long long)(*s - '0');
        s++;
    }
    return v;
}

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    const char *cl = micro_cmdline(zero_page_gpa);
    unsigned long long want = 5ull;
    unsigned long long t0, t1;
    unsigned v;

    micro_puts("\n");

    if (cl != 0) {
        const char *t = micro_cmdline_value(cl, "ticks");
        if (t != 0) {
            unsigned long long n = parse_uint(t);
            if (n == 0ull) {
                micro_fail(NAME, "cmdline ticks= was not a positive number");
                micro_halt();
            }
            want = n;
        }
    }

    micro_puts("micro/" NAME ": loading a guest GDT and IDT, then arming IRQ0 at vector ");
    micro_put_hex(IRQ0_VECTOR);
    micro_puts("\n");

    micro_cli();
    micro_gdt_load();
    micro_idt_load();

    micro_idt_set_gate(IRQ0_VECTOR, irq0_isr);
    /* Every other PIC vector gets the spurious handler, so a mis-routed IRQ is counted. */
    for (v = PIC_BASE; v < PIC_BASE + 16u; v++) {
        if (v != IRQ0_VECTOR) {
            micro_idt_set_gate(v, spurious_isr);
        }
    }

    micro_pic_remap((uint8_t)PIC_BASE, (uint8_t)(PIC_BASE + 8u));
    micro_pit_periodic((uint16_t)PIT_DIVISOR);
    micro_pic_unmask(0u);

    micro_puts("micro/" NAME ": PIC remapped, PIT at 100 Hz, IRQ0 unmasked -- sti\n");
    t0 = rdtsc();
    micro_sti();

    /*
     * Wait for the ticks. HLT rather than a busy spin: a halted guest is what hype's own idle path
     * expects, and it makes the test depend on delivery WAKING the guest rather than on the guest
     * happening to poll at the right moment. A bounded loop, so a tick that never arrives ends with
     * a verdict instead of a wedge.
     */
    while (g_ticks < want) {
        __asm__ volatile("hlt" ::: "memory");
        g_resumes++;
        if (g_resumes > 200000ull) {
            micro_puts("micro/" NAME ": only ");
            micro_put_uint(g_ticks);
            micro_puts(" of ");
            micro_put_uint(want);
            micro_puts(" tick(s) arrived in ");
            micro_put_uint(rdtsc() - t0);
            micro_puts(" TSC cycles; resumes past HLT=");
            micro_put_uint(g_resumes);
            micro_puts("; wrong-vector deliveries=");
            micro_put_uint(g_wrong_vector);
            micro_puts("\n");
            micro_fail(NAME, "IRQ0 did not keep being delivered -- the guest halted and was not "
                             "woken enough times");
            micro_halt();
        }
    }

    micro_cli();
    t1 = rdtsc();

    /*
     * REPORT THE RATE, do not assert it.
     *
     * The first run of this test delivered 1155 ticks across ONE HLT wakeup, which is a number with
     * no interpretation: it could be hype pacing correctly against a fast guest clock, or delivering
     * a burst of catch-up ticks, and "delivery works" cannot tell those apart. A passing test that
     * prints an uninterpretable number is where a rate regression would hide.
     *
     * What the guest CAN report is TSC cycles per tick. What it cannot do is convert that to Hz: it
     * has no second, independent clock -- the PIT is the thing under test, so timing the PIT with the
     * PIT is circular, and nothing here knows the TSC frequency. So the number goes in the log where
     * a change in it is visible across runs, and the assertion stays on what IS decidable: ticks
     * arrived, on the right vector, and kept arriving.
     *
     * Measured on AMD: 34.0M cycles/tick, which on a ~3.4 GHz host is 10 ms -- exactly the 100 Hz
     * programmed above. That is the number to compare against, and it is why the figure is worth
     * printing even though the test cannot verify it independently.
     */
    micro_puts("micro/" NAME ": ");
    micro_put_uint(g_ticks);
    micro_puts(" tick(s) delivered on vector ");
    micro_put_hex(IRQ0_VECTOR);
    micro_puts(", wrong-vector deliveries=");
    micro_put_uint(g_wrong_vector);
    micro_puts(", resumes past HLT=");
    micro_put_uint(g_resumes);
    micro_puts(", TSC cycles/tick=");
    micro_put_uint(g_ticks != 0ull ? (t1 - t0) / g_ticks : 0ull);
    micro_puts(" (reported, not asserted -- no independent clock in here)\n");

    if (g_wrong_vector != 0ull) {
        micro_fail(NAME, "interrupts were delivered on a vector other than the one IRQ0 was "
                         "remapped to");
        micro_halt();
    }

    micro_pass(NAME);
    micro_halt();
}
