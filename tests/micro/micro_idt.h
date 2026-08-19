#ifndef HYPE_MICRO_IDT_H
#define HYPE_MICRO_IDT_H

#include "micro.h"

/*
 * #541: guest-side descriptor tables and legacy interrupt controllers, for micro-kernels that need
 * to take a real interrupt.
 *
 * WHY THE GUEST MUST DO THIS ITSELF. hype's long-mode entry state gives every segment
 * selector = 0 (vmcb.c's set_longmode_seg) with usable attributes -- enough to execute 64-bit code,
 * and NOT enough to take an interrupt. Delivery loads CS from the IDT gate's selector, which must
 * name a real 64-bit code descriptor in a real GDT; and IRET restores CS from the frame the CPU
 * pushed, which is the selector that was current when the interrupt arrived. A null CS in that frame
 * makes IRET #GP.
 *
 * So a guest that wants interrupts must load its own GDT, reload CS and SS from it, and load its own
 * IDT. The in-binary INT test sidestepped all of that: the host installed the guest's GDT and IDT
 * for it (vmm_set_gdt/vmm_set_idt) and forced its CS/SS selectors (vmm_set_cs_ss_selectors), which
 * is not something any real guest experiences. That is the substance of this port, not an incidental
 * detail -- the lgdt/lidt/CS-reload path had never been exercised from inside a guest here.
 *
 * Everything lives in guest RAM at fixed addresses below the payload, chosen clear of core/kboot.h's
 * layout (page tables 0x1000-0x6FFF, zero page 0x7000, cmdline 0x8000, stack top 0x80000).
 */

#define MICRO_GDT_GPA 0x90000ull  /* above the stack top, below 1 MB */
#define MICRO_IDT_GPA 0x91000ull  /* 256 gates x 16 bytes = 4096 */

#define MICRO_CS_SELECTOR 0x08u
#define MICRO_SS_SELECTOR 0x10u

/*
 * The descriptor bytes are copied from vmcb.c's own LONGMODE_CODE_ACCESS/FLAGS and
 * LONGMODE_DATA_ACCESS/FLAGS, so a hardware CS reload during delivery lands on the same effective
 * attributes hype gave CS directly. A mismatch here would make the guest fault on delivery in a way
 * that looks like hype injecting badly.
 */
static inline void micro_gdt_load(void) {
    volatile uint8_t *gdt = (volatile uint8_t *)(uintptr_t)MICRO_GDT_GPA;
    static const uint8_t entries[24] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* null */
        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x9B, 0xAF, 0x00, /* 0x08: flat 64-bit code */
        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x93, 0xCF, 0x00, /* 0x10: flat data */
    };
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } gdtr = {(uint16_t)(sizeof(entries) - 1u), MICRO_GDT_GPA};
    unsigned i;

    for (i = 0; i < sizeof(entries); i++) {
        gdt[i] = entries[i];
    }
    __asm__ volatile("lgdt %0" : : "m"(gdtr) : "memory");

    /*
     * Reload CS via a far return, and SS/DS/ES from the data descriptor. lretq pops RIP then CS, so
     * CS is pushed first. Without this the interrupt frame carries CS = 0 and IRET faults -- which
     * presents as an exception INSIDE delivery, i.e. exactly like a hypervisor injecting wrongly.
     */
    __asm__ volatile("pushq %[cs]\n\t"
                     "leaq 1f(%%rip), %%rax\n\t"
                     "pushq %%rax\n\t"
                     "lretq\n\t"
                     "1:\n\t"
                     "movw %[ss], %%ax\n\t"
                     "movw %%ax, %%ss\n\t"
                     "movw %%ax, %%ds\n\t"
                     "movw %%ax, %%es\n\t"
                     :
                     : [cs] "i"(MICRO_CS_SELECTOR), [ss] "i"((uint16_t)MICRO_SS_SELECTOR)
                     : "rax", "memory");
}

/* A 64-bit interrupt gate: present, DPL 0, IST 0, type 0xE. */
static inline void micro_idt_set_gate(unsigned vector, void (*handler)(void)) {
    volatile uint8_t *gate = (volatile uint8_t *)(uintptr_t)(MICRO_IDT_GPA + (uint64_t)vector * 16ull);
    uint64_t off = (uint64_t)(uintptr_t)handler;

    gate[0] = (uint8_t)off;
    gate[1] = (uint8_t)(off >> 8);
    gate[2] = (uint8_t)MICRO_CS_SELECTOR;
    gate[3] = (uint8_t)(MICRO_CS_SELECTOR >> 8);
    gate[4] = 0x00u; /* IST 0 -- no separate stack; the guest's own stack is used */
    gate[5] = 0x8Eu; /* present | DPL0 | 64-bit interrupt gate */
    gate[6] = (uint8_t)(off >> 16);
    gate[7] = (uint8_t)(off >> 24);
    gate[8] = (uint8_t)(off >> 32);
    gate[9] = (uint8_t)(off >> 40);
    gate[10] = (uint8_t)(off >> 48);
    gate[11] = (uint8_t)(off >> 56);
    gate[12] = 0u;
    gate[13] = 0u;
    gate[14] = 0u;
    gate[15] = 0u;
}

static inline void micro_idt_load(void) {
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } idtr = {(uint16_t)(256u * 16u - 1u), MICRO_IDT_GPA};
    volatile uint8_t *idt = (volatile uint8_t *)(uintptr_t)MICRO_IDT_GPA;
    unsigned i;

    /* Every gate not-present by default: an unexpected vector then faults in a located way rather
     * than jumping through whatever happened to be in RAM. */
    for (i = 0; i < 256u * 16u; i++) {
        idt[i] = 0u;
    }
    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
}

static inline void micro_sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void micro_cli(void) { __asm__ volatile("cli" ::: "memory"); }

/*
 * An interrupt handler. Declared naked so the compiler emits no prologue that would corrupt the
 * interrupt frame, and it must end in IRETQ rather than RET. The body increments a counter and
 * sends the EOI; anything more elaborate belongs in the polling loop, not in here.
 */
#define MICRO_ISR(name, body_asm)                                                                  \
    __attribute__((naked)) static void name(void) {                                                \
        __asm__ volatile(body_asm "\n\tiretq\n\t");                                                \
    }

/* ---- 8259 PIC ---- */

#define MICRO_PIC_MASTER_CMD 0x20u
#define MICRO_PIC_MASTER_DATA 0x21u
#define MICRO_PIC_SLAVE_CMD 0xA0u
#define MICRO_PIC_SLAVE_DATA 0xA1u
#define MICRO_PIC_EOI 0x20u

/*
 * Remap both chips, exactly as a real OS does: ICW1 (init, expect ICW4), ICW2 (vector base),
 * ICW3 (cascade wiring), ICW4 (8086 mode), then the interrupt masks. Vectors 0-31 are reserved for
 * exceptions, so a base of 0x20 is the conventional choice and the one every guest hype boots uses.
 */
static inline void micro_pic_remap(uint8_t master_base, uint8_t slave_base) {
    micro_outb(MICRO_PIC_MASTER_CMD, 0x11u);
    micro_outb(MICRO_PIC_SLAVE_CMD, 0x11u);
    micro_outb(MICRO_PIC_MASTER_DATA, master_base);
    micro_outb(MICRO_PIC_SLAVE_DATA, slave_base);
    micro_outb(MICRO_PIC_MASTER_DATA, 0x04u); /* slave on IRQ2 */
    micro_outb(MICRO_PIC_SLAVE_DATA, 0x02u);  /* slave identity: cascade line 2 */
    micro_outb(MICRO_PIC_MASTER_DATA, 0x01u); /* 8086 mode */
    micro_outb(MICRO_PIC_SLAVE_DATA, 0x01u);
    micro_outb(MICRO_PIC_MASTER_DATA, 0xFFu); /* everything masked; callers unmask what they want */
    micro_outb(MICRO_PIC_SLAVE_DATA, 0xFFu);
}

static inline void micro_pic_unmask(unsigned irq) {
    if (irq < 8u) {
        micro_outb(MICRO_PIC_MASTER_DATA, (uint8_t)(micro_inb(MICRO_PIC_MASTER_DATA) & ~(1u << irq)));
    } else {
        micro_outb(MICRO_PIC_SLAVE_DATA,
                   (uint8_t)(micro_inb(MICRO_PIC_SLAVE_DATA) & ~(1u << (irq - 8u))));
        /* The cascade line must be open too, or a slave IRQ never reaches the CPU. */
        micro_outb(MICRO_PIC_MASTER_DATA, (uint8_t)(micro_inb(MICRO_PIC_MASTER_DATA) & ~(1u << 2)));
    }
}

/* ---- 8254 PIT ---- */

#define MICRO_PIT_CH0_DATA 0x40u
#define MICRO_PIT_CMD 0x43u
#define MICRO_PIT_HZ 1193182u

/* Channel 0, rate generator (mode 2), 16-bit binary, low byte then high byte. */
static inline void micro_pit_periodic(uint16_t divisor) {
    micro_outb(MICRO_PIT_CMD, 0x34u);
    micro_outb(MICRO_PIT_CH0_DATA, (uint8_t)divisor);
    micro_outb(MICRO_PIT_CH0_DATA, (uint8_t)(divisor >> 8));
}

#endif /* HYPE_MICRO_IDT_H */
