/*
 * #540: the PAUSE-filter test, ported out of boot/main.c -- and reframed.
 *
 * The in-binary test counted SVM PAUSE intercepts and was SVM-only, because SVM pause-filtering was
 * the mechanism it inspected. A guest cannot see its own VM-exits, so I originally filed this saying
 * the verdict could not move guest-side.
 *
 * That was too pessimistic. Counting intercepts is not what the test is FOR -- the original's own
 * words are "host reclaimed control from a spinning guest -- preemption mechanism works", and that
 * has a consequence a guest can observe directly:
 *
 *   A GUEST SPINNING ON PAUSE WITH INTERRUPTS ENABLED STILL RECEIVES ITS TIMER TICKS.
 *
 * If the host never regained control, nothing could be injected and the tick count would not move
 * for the whole spin. So the test arms a periodic tick, spins on PAUSE without ever executing a
 * voluntary exit, and checks the count advanced. That is a stronger statement than "hype counted N
 * PAUSE exits": it proves the whole preemption path including delivery, rather than that an
 * intercept fired.
 *
 * The distinction from tests/micro/intdeliver.c is deliberate and is the whole point: intdeliver
 * HLTs, which is a VOLUNTARY exit -- hype gets control because the guest gave it up. This one never
 * exits voluntarily. Every instruction it executes is PAUSE or a loop, so the only way a tick can
 * arrive is if hype TOOK control back.
 *
 * It also runs on BOTH vendors, which the SVM-only original could not. What is being tested is the
 * outcome, not the mechanism -- hype preempts via its own periodic host timer (RT-2b's
 * INTERCEPT_INTR) on Intel and AMD alike, with SVM pause-filtering as an additional trigger where
 * the CPU has it. So there is no "this CPU has no pause filter, skip" arm any more: a host that
 * cannot preempt a spinning guest is broken on either vendor, and this reports it as such.
 */
#include "micro_idt.h"

#define NAME "pausespin"

#define PIC_BASE 0x20u
#define IRQ0_VECTOR (PIC_BASE + 0u)
#define PIT_DIVISOR (MICRO_PIT_HZ / 100u) /* 100 Hz, as in intdeliver */

static volatile unsigned long long g_ticks;

MICRO_ISR(irq0_isr,
          "incq g_ticks(%rip)\n\t"
          "movb $0x20, %al\n\t"
          "outb %al, $0x20\n\t")

static inline unsigned long long cpuid_ext7_edx(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000007u), "c"(0u));
    (void)eax; (void)ebx; (void)ecx;
    return (unsigned long long)edx;
}

#define EXT7_EDX_INVARIANT_TSC (1u << 8)

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
    unsigned long long spin_iters = 200000000ull; /* enough to cover several ticks at 100 Hz */
    unsigned long long before, after, t0, t1, i;
    unsigned v;

    micro_puts("\n");

    if (cl != 0) {
        const char *n = micro_cmdline_value(cl, "spin");
        if (n != 0) {
            unsigned long long got = parse_uint(n);
            if (got == 0ull) {
                micro_fail(NAME, "cmdline spin= was not a positive number");
                micro_halt();
            }
            spin_iters = got;
        }
    }

    /*
     * INVARIANT TSC FIRST, because the rate check below is only meaningful if it holds.
     *
     * An invariant TSC counts at a fixed reference rate regardless of core frequency, P-states,
     * C-states or boost -- so a host that boosts from 3.4 to 4.4 GHz mid-spin changes nothing this
     * guest can see through RDTSC, and the implied figure stays put. Without the bit, RDTSC could
     * track the core clock and the number below would drift with boost for reasons that have nothing
     * to do with hype's tick pacing.
     *
     * hype advertises this deliberately (cpuid_emulate.c, leaf 0x80000007 EDX bit 8): it passes the
     * host TSC straight through, and the bit's absence makes a real Linux guest's clocksource
     * watchdog see the emulated PIT drift against the raw TSC, mark the TSC unstable and fall back
     * to a slow skewed clock -- "clock skew detected", observed on real hardware. So checking it
     * here guards a real guest's timekeeping, not just this test's arithmetic.
     */
    if ((cpuid_ext7_edx() & EXT7_EDX_INVARIANT_TSC) == 0ull) {
        micro_fail(NAME, "CPUID 0x80000007 EDX bit 8 (invariant TSC) is NOT advertised -- a real "
                         "guest's clocksource watchdog would mark the TSC unstable and fall back to "
                         "a skewed clock, and this test's rate figure would be meaningless");
        micro_halt();
    }
    micro_puts("micro/" NAME ": invariant TSC advertised, so RDTSC is boost-independent\n");

    micro_cli();
    micro_gdt_load();
    micro_idt_load();
    micro_idt_set_gate(IRQ0_VECTOR, irq0_isr);
    for (v = PIC_BASE; v < PIC_BASE + 16u; v++) {
        if (v != IRQ0_VECTOR) {
            micro_idt_set_gate(v, irq0_isr); /* any PIC vector counts; mis-routing is intdeliver's job */
        }
    }
    micro_pic_remap((uint8_t)PIC_BASE, (uint8_t)(PIC_BASE + 8u));
    micro_pit_periodic((uint16_t)PIT_DIVISOR);
    micro_pic_unmask(0u);
    micro_sti();

    /*
     * Let at least one tick land first, so a zero delta during the spin cannot be explained by the
     * timer simply never having started. Bounded, and it HLTs -- this part is not the test.
     */
    {
        unsigned long long guard = 0ull;
        while (g_ticks == 0ull) {
            __asm__ volatile("hlt" ::: "memory");
            if (++guard > 1000ull) {
                micro_fail(NAME, "no tick arrived even before the spin -- the timer never started, "
                                 "so this test cannot say anything about preemption");
                micro_halt();
            }
        }
    }

    micro_puts("micro/" NAME ": timer live (");
    micro_put_uint(g_ticks);
    micro_puts(" tick(s)); now spinning on PAUSE with IF=1 and NO voluntary exits\n");

    before = g_ticks;
    t0 = rdtsc();
    /*
     * The spin. PAUSE and a decrement, nothing else -- no HLT, no port I/O, no CPUID, no MMIO. If
     * hype cannot take control back from this, the tick count cannot move.
     *
     * Written as asm so the loop cannot be optimised away or have anything else hoisted into it: a
     * compiler-generated loop that happened to contain a memory access hype traps would invalidate
     * the whole premise.
     */
    for (i = 0; i < spin_iters; i++) {
        __asm__ volatile("pause" ::: "memory");
    }
    t1 = rdtsc();
    after = g_ticks;

    micro_cli();

    micro_puts("micro/" NAME ": spin of ");
    micro_put_uint(spin_iters);
    micro_puts(" PAUSE iterations took ");
    micro_put_uint(t1 - t0);
    micro_puts(" TSC cycles and ");
    micro_put_uint(after - before);
    micro_puts(" tick(s) were delivered DURING it\n");

    if (after == before) {
        micro_fail(NAME, "no tick was delivered during a PAUSE spin -- hype did not reclaim control "
                         "from a spinning guest, so a guest that busy-waits would freeze its own "
                         "clock");
        micro_halt();
    }

    /*
     * The RATE, which is decidable here even without an independent clock -- unlike in
     * tests/micro/intdeliver.c, where it is only reported.
     *
     * Each tick is 1/100 s by construction, so the TSC cycles the spin took, divided by the seconds
     * those ticks account for, is an IMPLIED TSC FREQUENCY. That number cannot be verified exactly,
     * but it can be bounded: no real x86 host runs below 200 MHz or above 20 GHz. So a tick rate
     * wrong by an order of magnitude in either direction -- ticks bursting, or the guest's clock
     * running slow because preemption is too coarse -- lands outside those bounds and fails, while a
     * correct rate on any plausible host passes.
     *
     * Measured on AMD: 11.84e9 cycles for 348 ticks implies a 3.40 GHz TSC rate. The two numbers the
     * guest can see agree on how much time passed, which is what makes this an assertion rather
     * than a note.
     *
     * The bounds are wide on purpose. This is a 10x-error detector, not a precision measurement --
     * so it is insensitive to anything that shifts the figure by a factor of two or less, which
     * includes every plausible boost ratio even on a part whose TSC did track core frequency.
     */
    {
        unsigned long long ticks = after - before;
        unsigned long long implied_hz = ((t1 - t0) * 100ull) / ticks;

        /* "TSC rate", not "CPU frequency": on an invariant-TSC part this is the fixed reference
         * rate, which is typically the nominal/base frequency and is NOT whatever the core happens
         * to be boosting to. Comparing it against a datasheet max-turbo figure would be a mistake,
         * so the label says which number it is. */
        micro_puts("micro/" NAME ": implied TSC rate ");
        micro_put_uint(implied_hz / 1000000ull);
        micro_puts(" MHz (invariant, so boost-independent; NOT the core clock)\n");

        if (implied_hz < 200000000ull || implied_hz > 20000000000ull) {
            micro_fail(NAME, "the tick rate during the spin implies an impossible TSC frequency -- "
                             "ticks are arriving far too fast or far too slowly for 100 Hz");
            micro_halt();
        }
    }

    micro_pass(NAME);
    micro_halt();
}
