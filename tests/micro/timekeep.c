/*
 * SMP-15 (#471): a guest keeps correct time while its vCPU is switched out of a shared core.
 *
 * The guest takes the PIT's IRQ0 at 100 Hz and HALTS between ticks, so on a shared core it is
 * idle most of the time and gives the core away (hype's voluntary yield), then must be woken and
 * handed every tick that arrived while it was switched out. It counts ticks across exactly
 * `secs` seconds of the emulated CMOS RTC -- which hype advances from HOST elapsed time -- and
 * compares against 100 per second. A tick lost to a descheduled vCPU, or delivered to the wrong
 * one, shows as a shortfall; hype coalesces a periodic timer only when a vCPU misses a whole
 * period, which a 2 ms slice with two co-tenants cannot cause.
 *
 *   cmdline: [secs=<decimal>]   (default 30; PASS within 1%)
 */
#include "micro.h"
#include "micro_idt.h"

#define NAME "timekeep"
#define PIC_BASE 0x20u
#define IRQ0_VECTOR (PIC_BASE + 0u)
#define HZ 100u
#define PIT_DIVISOR (MICRO_PIT_HZ / HZ)

static volatile unsigned long long g_ticks;
static volatile unsigned long long g_wrong;

MICRO_ISR(irq0_isr,
          "pushq %rax\n\t"
          "incq g_ticks(%rip)\n\t"
          "movb $0x20, %al\n\t"
          "outb %al, $0x20\n\t"
          "popq %rax\n\t")

MICRO_ISR(spurious_isr,
          "pushq %rax\n\t"
          "incq g_wrong(%rip)\n\t"
          "movb $0x20, %al\n\t"
          "outb %al, $0x20\n\t"
          "popq %rax\n\t")

/* The RTC seconds register, read only when no update is in progress (register A bit 7). */
static uint8_t rtc_seconds(void) {
    unsigned int spins = 100000u;
    for (;;) {
        micro_outb(0x70, 0x0A);
        if ((micro_inb(0x71) & 0x80u) == 0u || spins-- == 0u) break;
    }
    micro_outb(0x70, 0x00);
    return micro_inb(0x71);
}

/* HLT until the RTC seconds register changes; returns 0 if it never does. */
static int wait_second_edge(void) {
    uint8_t s0 = rtc_seconds();
    unsigned long long start = g_ticks;
    while (rtc_seconds() == s0) {
        __asm__ volatile("hlt" ::: "memory");
        if (g_ticks - start > 10u * HZ) return 0; /* 10 s of ticks and no second passed */
    }
    return 1;
}

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    const char *sv = micro_cmdline_value(micro_cmdline(zero_page_gpa), "secs");
    unsigned long long secs = 30, t0, got, want, tol;
    unsigned v, i;

    micro_puts("\n");
    if (sv != 0) {
        secs = 0;
        while (*sv >= '0' && *sv <= '9') secs = secs * 10u + (unsigned long long)(*sv++ - '0');
        if (secs == 0u || (*sv != '\0' && *sv != ' ')) {
            micro_fail(NAME, "secs= is not a positive decimal");
            micro_halt();
        }
    }
    micro_cli();
    micro_gdt_load();
    micro_idt_load();
    micro_idt_set_gate(IRQ0_VECTOR, irq0_isr);
    for (v = PIC_BASE; v < PIC_BASE + 16u; v++) {
        if (v != IRQ0_VECTOR) micro_idt_set_gate(v, spurious_isr);
    }
    micro_pic_remap((uint8_t)PIC_BASE, (uint8_t)(PIC_BASE + 8u));
    micro_pit_periodic((uint16_t)PIT_DIVISOR);
    micro_pic_unmask(0u);
    micro_sti();

    if (!wait_second_edge()) {
        micro_fail(NAME, "the RTC seconds register never changed");
        micro_halt();
    }
    t0 = g_ticks;
    micro_puts("micro/" NAME ": counting PIT ticks across ");
    micro_put_uint(secs);
    micro_puts(" RTC seconds\n");
    for (i = 0; i < secs; i++) {
        if (!wait_second_edge()) {
            micro_fail(NAME, "the RTC stopped mid-run");
            micro_halt();
        }
    }
    got = g_ticks - t0;
    want = secs * HZ;
    tol = want / 100u;
    micro_puts("micro/" NAME ": ");
    micro_put_uint(got);
    micro_puts(" ticks in ");
    micro_put_uint(secs);
    micro_puts(" s (want ");
    micro_put_uint(want);
    micro_puts(" +/- ");
    micro_put_uint(tol);
    micro_puts("), wrong vectors ");
    micro_put_uint(g_wrong);
    micro_puts("\n");
    if (g_wrong != 0u) {
        micro_fail(NAME, "an interrupt arrived on a vector this guest did not program");
        micro_halt();
    }
    if (got + tol < want || got > want + tol) {
        micro_fail(NAME, "the tick count does not match host time");
        micro_halt();
    }
    micro_pass(NAME);
    micro_halt();
}
