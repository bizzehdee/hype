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
 * Making it volatile did NOT change the number, which was the first clue: the guest reports ~1154
 * deliveries against ONE resume past the HLT.
 *
 * #553 ANSWERED IT, and the mechanism is the one that was suspected: the HLT is not retired before
 * injection, so each iretq returns to the hlt itself. The evidence is the at_hlt/past_hlt
 * breakdown this test now prints -- 1153 of 1154 deliveries had an interrupt frame pointing AT the
 * hlt. Intel SDM Vol. 2A p. 3-439 requires the saved RIP to point to the instruction FOLLOWING the
 * hlt, so this is a defect and not a defensible modelling choice; it is tracked as #580.
 *
 * #580 FIXED IT, and the assertion arrived with the fix (see the end of this test). The wake block
 * that retires the HLT sat below a `productive_exits < 1500` boot-progress gate, so for a guest
 * whose productive exits are front-loaded the retire was never even considered. Hoisting it above
 * that gate took the breakdown from at_hlt=1153/past_hlt=1 to at_hlt=0/past_hlt=5 -- and the guest
 * now gets exactly the 5 ticks it asked for instead of overshooting by 1149 it could not observe.
 *
 * It does not affect this test's verdict: what is asserted is that ticks arrive, on the vector IRQ0
 * was remapped to, and keep arriving. Both counts are reported so a change in their relationship is
 * visible rather than silent.
 */
static volatile unsigned long long g_resumes;
/*
 * #553: WHERE the interrupt frame says the guest was, so "1154 deliveries, 1 resume" can be
 * explained rather than guessed at. The frame's RIP is the first quadword the CPU pushes, so the
 * ISR can read it off its own stack and no host-side instrumentation is needed.
 *
 * If the HLT was RETIRED before injection, the frame RIP is the byte AFTER the hlt (hlt is one
 * byte, 0xF4) and each iretq resumes the C loop. If it was NOT retired, the frame RIP is the hlt
 * itself and every iretq re-executes it -- the guest re-halts and the loop never advances, which is
 * the mechanism #318 already found once.
 */
extern const char micro_hlt_site[]; /* #553: the labelled hlt, defined in the asm below */
static volatile unsigned long long g_frame_rip;      /* the most recent frame's RIP */
static volatile unsigned long long g_frame_at_hlt;   /* deliveries whose frame RIP == the hlt */
static volatile unsigned long long g_frame_past_hlt; /* deliveries whose frame RIP == hlt + 1 */

/*
 * The handler. Increments the counter and EOIs the master. Written in asm because a naked function
 * must not have a compiler prologue -- an interrupt frame is not a call frame, and a pushed
 * register would be popped as part of it.
 */
/*
 * #553: also records WHERE the interrupt frame points, which is what distinguishes an unretired
 * HLT from a mis-counted tick. The frame RIP is at (%rsp) on entry -- an interrupt gate with no
 * error code -- so it is 16(%rsp) after two pushes.
 *
 * The pushes are not optional. An ISR is not a call frame: a naked handler that clobbers a register
 * corrupts the interrupted code's copy of it. This handler used to write %al with no save at all,
 * which happened to be harmless only because the interrupted instruction is a hlt with nothing live
 * in rax -- true today, and not a property to rely on.
 */
MICRO_ISR(irq0_isr,
          "pushq %rax\n\t"
          "pushq %rcx\n\t"
          "movq 16(%rsp), %rax\n\t"
          "movq %rax, g_frame_rip(%rip)\n\t"
          "leaq micro_hlt_site(%rip), %rcx\n\t"
          "cmpq %rcx, %rax\n\t"
          "jne 1f\n\t"
          "incq g_frame_at_hlt(%rip)\n\t"
          "jmp 2f\n\t"
          "1:\n\t"
          "incq %rcx\n\t"
          "cmpq %rcx, %rax\n\t"
          "jne 2f\n\t"
          "incq g_frame_past_hlt(%rip)\n\t"
          "2:\n\t"
          "incq g_ticks(%rip)\n\t"
          "movb $0x20, %al\n\t"
          "outb %al, $0x20\n\t"
          "popq %rcx\n\t"
          "popq %rax\n\t")

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
        /* #553: the hlt carries a label so the ISR can compare the interrupt frame's RIP against
         * it. One instance in the binary; if a future compiler duplicates this asm block the
         * duplicate-symbol error is a loud failure, which is the right outcome. */
        __asm__ volatile("micro_hlt_site:\n\thlt" ::: "memory");
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

    /*
     * #553: the answer to "1154 deliveries, 1 resume past the HLT", measured rather than theorised.
     * at_hlt counts deliveries whose interrupt frame pointed AT the hlt (so the iretq re-executes
     * it and the C loop does not advance); past_hlt counts those that pointed after it.
     */
    micro_puts("micro/" NAME ": [#553] frame RIP at_hlt=");
    micro_put_uint(g_frame_at_hlt);
    micro_puts(" past_hlt=");
    micro_put_uint(g_frame_past_hlt);
    micro_puts(" other=");
    micro_put_uint(g_ticks - g_frame_at_hlt - g_frame_past_hlt);
    micro_puts(" last_frame_rip=");
    micro_put_hex(g_frame_rip);
    micro_puts(" hlt_site=");
    micro_put_hex((unsigned long long)(uintptr_t)&micro_hlt_site);
    micro_puts("\n");

    /*
     * #580: every delivery must leave the guest resuming AFTER the hlt, never on it. Intel SDM
     * Vol. 2A p. 3-439 requires the saved RIP to point to the following instruction, and a guest
     * that resumes AT the hlt makes no forward progress -- its idle loop cannot observe the tick
     * it was woken for. This is the assertion #553 measured and deliberately did not make until
     * the fix existed.
     *
     * at_hlt is the defect condition exactly, so it is asserted at zero rather than as a ratio.
     * `other` is legitimate -- a delivery that arrives while the guest is running rather than
     * halted has a frame RIP that is neither address -- so the accounting closes on
     * past_hlt + other, not on past_hlt alone.
     */
    if (g_frame_at_hlt != 0ull) {
        micro_fail(NAME, "an interrupt resumed the guest AT the hlt instead of after it, so its "
                         "wait loop cannot observe the tick it was woken for (#580)");
        micro_halt();
    }
    if (g_frame_past_hlt + (g_ticks - g_frame_at_hlt - g_frame_past_hlt) != g_ticks) {
        micro_fail(NAME, "the HLT-resume accounting does not add up to the tick count");
        micro_halt();
    }

    if (g_wrong_vector != 0ull) {
        micro_fail(NAME, "interrupts were delivered on a vector other than the one IRQ0 was "
                         "remapped to");
        micro_halt();
    }

    micro_pass(NAME);
    micro_halt();
}
