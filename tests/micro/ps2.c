/*
 * #542: INPUT-1 and INPUT-2, ported out of boot/main.c as one guest-driven test.
 *
 * Both in-binary tests injected the event from the HOST -- hype_ps2_kbd_enqueue_scancode() and
 * hype_ps2_mouse_enqueue_movement() called directly -- and then asserted the guest saw it. That is
 * the coupling #534 removes, and removing it exposed something: hype's guest PS/2 MOUSE had no
 * stimulus path at all that did not involve a human at a physical mouse. The keyboard did have one
 * (INPUT-11/#284's `sendkey`); the mouse did not, so #542 added the symmetric `sendmouse` directive.
 *
 * So this test is driven entirely from outside the guest by a checked-in section-6k input script,
 * over the same path a real operator's input takes. The guest is a small i8042 driver: it takes IRQ1
 * and IRQ12, reads port 0x60, distinguishes keyboard bytes from mouse bytes by the AUX_DATA status
 * bit, and reports what it got.
 *
 * ONE TRAP WORTH NAMING. This test cannot pass without its input script, and an absent script does
 * not look like a failure -- it looks like a guest waiting forever for a keystroke, which is
 * indistinguishable from a wedge. So the wait is bounded and the failure says "no input arrived",
 * naming the script as the first thing to check.
 */
#include "micro_idt.h"

#define NAME "ps2"

#define PIC_BASE 0x20u
#define IRQ1_VECTOR (PIC_BASE + 1u)   /* keyboard */
#define IRQ12_VECTOR (PIC_BASE + 8u + 4u) /* mouse: slave IRQ4 == IRQ12 overall */

#define PS2_DATA 0x60u
#define PS2_STATUS 0x64u
#define PS2_STATUS_OBF (1u << 0)
#define PS2_STATUS_AUX (1u << 5)

#define MAX_CAPTURE 64u

static volatile uint8_t g_kbd[MAX_CAPTURE];
static volatile unsigned long long g_kbd_n;
static volatile uint8_t g_mouse[MAX_CAPTURE];
static volatile unsigned long long g_mouse_n;
/* The STATUS byte that accompanied each data byte, kept so a misclassification reports the evidence
 * rather than just the wrong total. Reading 0x64 is what decides keyboard vs mouse, so when the
 * split comes out wrong this is the only thing that says why. */
static volatile uint8_t g_status[MAX_CAPTURE];
static volatile uint8_t g_data[MAX_CAPTURE];
static volatile unsigned long long g_all_n;
static volatile unsigned long long g_irq1;
static volatile unsigned long long g_irq12;

/*
 * Both handlers read the STATUS register before the data byte, because that is the only thing that
 * says which device the byte came from -- real hardware's AUX_DATA bit, which hype models
 * (HYPE_PS2_STATUS_AUX_DATA). A handler that assumed "IRQ1 means keyboard" would pass even if hype
 * routed mouse bytes to port 0x60 with the wrong status, which is exactly the kind of mistake this
 * test should catch.
 *
 * Written in C and called from a naked stub, so the capture logic stays readable while the frame
 * stays intact.
 */
static void ps2_drain(void) {
    unsigned guard = 0u;
    while ((micro_inb(PS2_STATUS) & PS2_STATUS_OBF) != 0u && guard++ < 16u) {
        uint8_t st = micro_inb(PS2_STATUS);
        uint8_t b = micro_inb(PS2_DATA);

        if (g_all_n < MAX_CAPTURE) {
            g_status[g_all_n] = st;
            g_data[g_all_n] = b;
        }
        g_all_n++;

        if ((st & PS2_STATUS_AUX) != 0u) {
            if (g_mouse_n < MAX_CAPTURE) {
                g_mouse[g_mouse_n] = b;
            }
            g_mouse_n++;
        } else {
            if (g_kbd_n < MAX_CAPTURE) {
                g_kbd[g_kbd_n] = b;
            }
            g_kbd_n++;
        }
    }
}

void ps2_irq1_c(void);
void ps2_irq12_c(void);

void ps2_irq1_c(void) {
    g_irq1++;
    ps2_drain();
    micro_outb(0x20u, 0x20u); /* EOI to the master */
}

void ps2_irq12_c(void) {
    g_irq12++;
    ps2_drain();
    /* A slave-chip IRQ needs BOTH EOIs, slave first. Missing the master EOI leaves IRQ2 in service
     * and every later slave interrupt is blocked -- a failure that looks like "the mouse stopped
     * working after one packet". */
    micro_outb(0xA0u, 0x20u);
    micro_outb(0x20u, 0x20u);
}

/* Naked stubs: save the registers a C function may clobber, call it, restore, iretq. */
MICRO_ISR(irq1_isr,
          "pushq %rax\n\tpushq %rcx\n\tpushq %rdx\n\tpushq %rsi\n\tpushq %rdi\n\t"
          "pushq %r8\n\tpushq %r9\n\tpushq %r10\n\tpushq %r11\n\t"
          "call ps2_irq1_c\n\t"
          "popq %r11\n\tpopq %r10\n\tpopq %r9\n\tpopq %r8\n\t"
          "popq %rdi\n\tpopq %rsi\n\tpopq %rdx\n\tpopq %rcx\n\tpopq %rax\n\t")

MICRO_ISR(irq12_isr,
          "pushq %rax\n\tpushq %rcx\n\tpushq %rdx\n\tpushq %rsi\n\tpushq %rdi\n\t"
          "pushq %r8\n\tpushq %r9\n\tpushq %r10\n\tpushq %r11\n\t"
          "call ps2_irq12_c\n\t"
          "popq %r11\n\tpopq %r10\n\tpopq %r9\n\tpopq %r8\n\t"
          "popq %rdi\n\tpopq %rsi\n\tpopq %rdx\n\tpopq %rcx\n\tpopq %rax\n\t")

static inline unsigned long long rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | (unsigned long long)lo;
}

/*
 * Timeouts here are expressed in TSC CYCLES, not loop iterations -- and that is not a style choice.
 *
 * The first version bounded the wait at 100000 HLT resumes, and the run ended before the bound was
 * reached, so a failing test produced no verdict at all. #553 is why: on this hypervisor a single HLT
 * can absorb a very large number of interrupt deliveries without the surrounding loop advancing, so
 * an iteration count is not proportional to elapsed time. Any microtest that waits for something must
 * bound itself against a clock.
 *
 * 40e9 cycles is ~12 s at 3.4 GHz and ~4 s at 10 GHz -- generous on any host, and far inside the
 * time the harness gives a run, so the verdict always gets printed.
 */
#define WAIT_CYCLES 40000000000ull

static void dump(const char *what, const volatile uint8_t *buf, unsigned long long n) {
    unsigned long long i;
    micro_puts("micro/" NAME ": ");
    micro_puts(what);
    micro_puts(" bytes(");
    micro_put_uint(n);
    micro_puts("):");
    for (i = 0; i < n && i < MAX_CAPTURE; i++) {
        micro_puts(" ");
        micro_put_hex(buf[i]);
    }
    micro_puts("\n");
}

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    unsigned long long deadline;

    (void)zero_page_gpa;
    micro_puts("\n");

    micro_cli();
    micro_gdt_load();
    micro_idt_load();
    micro_idt_set_gate(IRQ1_VECTOR, irq1_isr);
    micro_idt_set_gate(IRQ12_VECTOR, irq12_isr);
    micro_pic_remap((uint8_t)PIC_BASE, (uint8_t)(PIC_BASE + 8u));
    micro_pic_unmask(1u);  /* keyboard */
    micro_pic_unmask(12u); /* mouse -- opens the cascade line too, see micro_pic_unmask */

    /*
     * Enable the auxiliary device and its interrupt through the controller, exactly as a real OS
     * does: read the config byte, set both interrupt-enable bits, clear the mouse-disable bit, write
     * it back, then tell the mouse to report. Without this the model is entitled to deliver nothing,
     * and the test would be asserting against a device it never turned on.
     */
    micro_outb(PS2_STATUS, 0x20u); /* read config byte */
    {
        unsigned guard = 0u;
        uint8_t cfg = 0u;
        while ((micro_inb(PS2_STATUS) & PS2_STATUS_OBF) == 0u && guard++ < 100000u) {
        }
        cfg = micro_inb(PS2_DATA);
        micro_puts("micro/" NAME ": controller config byte ");
        micro_put_hex(cfg);
        micro_puts("\n");
        cfg |= 0x03u;  /* keyboard + aux interrupt enable */
        cfg &= (uint8_t)~0x20u; /* clear aux-disable */
        micro_outb(PS2_STATUS, 0x60u); /* write config byte */
        micro_outb(PS2_DATA, cfg);
    }
    micro_outb(PS2_STATUS, 0xD4u); /* next data byte goes to the aux device */
    micro_outb(PS2_DATA, 0xF4u);   /* enable reporting */

    micro_sti();
    micro_puts("micro/" NAME ": i8042 armed; waiting for scripted input (needs \\input\\vmN.txt -- "
               "an absent script looks exactly like a wedge)\n");

    /* Wait for at least one keyboard byte AND one full 3-byte mouse packet. */
    /*
     * PAUSE, NOT HLT -- and that is a finding, not a preference.
     *
     * The first two versions waited with `hlt` inside the loop. The bytes arrived and the handlers
     * ran (hype's own ps2 trace shows them being read at the ISRs' addresses), and the loop STILL
     * never advanced past the hlt, so no bound expressed inside it could ever fire and a failing test
     * produced no verdict at all. That is #553: on this hypervisor a guest's hlt-based wait loop does
     * not reliably make forward progress, which is a real defect for any real driver that polls that
     * way -- escalated there with this evidence.
     *
     * A PAUSE spin makes progress reliably (tests/micro/pausespin.c proves preemption during exactly
     * such a spin), so this test uses one. Waking on HLT is already covered by
     * tests/micro/intdeliver.c, so nothing is lost by not exercising it twice.
     */
    deadline = rdtsc() + WAIT_CYCLES;
    while (g_kbd_n == 0ull || g_mouse_n < 3ull) {
        __asm__ volatile("pause" ::: "memory");
        if (rdtsc() > deadline) {
            micro_cli();
            dump("keyboard", g_kbd, g_kbd_n);
            dump("mouse", g_mouse, g_mouse_n);
            micro_puts("micro/" NAME ": irq1=");
            micro_put_uint(g_irq1);
            micro_puts(" irq12=");
            micro_put_uint(g_irq12);
            micro_puts("\n");
            {
                unsigned long long i;
                micro_puts("micro/" NAME ": status/data pairs seen:");
                for (i = 0; i < g_all_n && i < MAX_CAPTURE; i++) {
                    micro_puts(" ");
                    micro_put_hex(g_status[i]);
                    micro_puts("/");
                    micro_put_hex(g_data[i]);
                }
                micro_puts("\n");
            }
            /*
             * TWO different failures reach here and the first version of this could only ever
             * report one of them. This test provokes a 0xFA ACK itself (the 0xD4/0xF4 aux-enable
             * above), so g_all_n is NEVER zero -- which made the "no scripted input" branch
             * below unreachable, and made a run with no input script report an AUX_DATA defect
             * that had not happened. hype set the bit correctly on the one byte it had.
             *
             * The distinguishing question is whether AUX was set on ANY byte, not how many
             * bytes arrived. A wrong verdict is worse than no verdict: it sends the reader to
             * the wrong subsystem, and this suite exists to be believed.
             */
            if (g_all_n != 0ull && g_mouse_n == 0ull) {
                micro_fail(NAME, "bytes arrived but AUX_DATA (status bit 5) never marked any of them "
                                 "as mouse bytes -- see the status/data pairs above; a guest cannot "
                                 "tell the two devices apart without that bit");
                micro_halt();
            }
            micro_fail(NAME, "scripted input did not arrive in full -- one keyboard byte and a "
                             "3-byte mouse packet are needed, and the counts above say what came "
                             "(a lone 0xFA is this test's own command ACK, not input); check the "
                             "VM has an input script at \\input\\vmN.txt with sendkey and "
                             "sendmouse directives");
            micro_halt();
        }
    }

    micro_cli();
    dump("keyboard", g_kbd, g_kbd_n);
    dump("mouse", g_mouse, g_mouse_n);
    micro_puts("micro/" NAME ": irq1=");
    micro_put_uint(g_irq1);
    micro_puts(" irq12=");
    micro_put_uint(g_irq12);
    micro_puts("\n");

    /*
     * Content checks. A byte count alone would pass if hype delivered the wrong bytes on the right
     * lines, which is the failure a test driven by real input is uniquely able to see.
     */
    if (g_irq1 == 0ull) {
        micro_fail(NAME, "keyboard bytes arrived without IRQ1 -- the guest polled them rather than "
                         "being interrupted");
        micro_halt();
    }
    if (g_irq12 == 0ull) {
        micro_fail(NAME, "mouse bytes arrived without IRQ12");
        micro_halt();
    }
    /* The mouse packet's first byte is the status byte, whose bit 3 is always 1 on a real device. */
    if ((g_mouse[0] & 0x08u) == 0u) {
        micro_fail(NAME, "the first mouse byte does not have the always-1 bit set, so it is not a "
                         "packet status byte -- the 3-byte framing is wrong");
        micro_halt();
    }
    /* A Set-1 make code has bit 7 clear; a break code sets it. The script types real characters, so
     * at least one make code must be present. */
    {
        unsigned long long i;
        int saw_make = 0;
        for (i = 0; i < g_kbd_n && i < MAX_CAPTURE; i++) {
            if ((g_kbd[i] & 0x80u) == 0u) {
                saw_make = 1;
                break;
            }
        }
        if (!saw_make) {
            micro_fail(NAME, "no Set-1 make code among the keyboard bytes -- only break codes "
                             "arrived, so the press half of each keystroke was lost");
            micro_halt();
        }
    }

    micro_pass(NAME);
    micro_halt();
}
