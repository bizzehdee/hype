#ifndef HYPE_ARCH_IDT_H
#define HYPE_ARCH_IDT_H

#include <stdint.h>

/*
 * Own IDT (M1-2). 256 long-mode interrupt-gate descriptors, all pointing
 * at per-vector assembly stubs (arch/x86_64/cpu/isr_stubs.S) that push
 * the vector number (and a dummy error code for vectors the CPU doesn't
 * push one for) and jump to a common C-callable trampoline. For M1,
 * every vector is fatal -- decode and panic; graceful fault recovery
 * (the per-vCPU watchdog) is M8-8's job, built on top of this once
 * there's an actual VM to isolate a fault to.
 *
 * Same split as gdt.h/gdt_load.c: descriptor encoding is pure and unit
 * tested here; `lidt` is a thin, hardware-only shim in idt_load.c; the
 * per-vector push+jmp stubs and register save/restore are hand-written
 * asm with nothing to unit test (isr_stubs.S, isr_common.S); the actual
 * "what does this fault mean" decision is a plain, testable C function
 * in isr_common.c fed a struct of already-saved register/vector state,
 * per AGENTS.md's guidance for exactly this kind of hardware boundary.
 */

#define HYPE_IDT_ENTRY_COUNT 256

typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed)) hype_idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) hype_idt_ptr_t;

/* type_attr for a present, ring-0, 64-bit interrupt gate (IF cleared on
 * entry) -- what every vector uses for M1; a trap gate (IF left alone)
 * or DPL=3 gate can be added via a different type_attr if a later
 * milestone needs one. */
#define HYPE_IDT_TYPE_INTERRUPT_GATE 0x8E

/*
 * Encodes one 16-byte gate descriptor. `ist` selects an Interrupt Stack
 * Table entry (0 = "don't switch stacks", per the AMD64/Intel SDM) --
 * M1 has no separate exception stack yet, so every vector uses 0 until
 * a later milestone adds one for #DF/NMI. Pure bit-packing, no CPU
 * state touched.
 */
void hype_idt_encode_entry(hype_idt_entry_t *entry, uint64_t handler,
                            uint16_t selector, uint8_t ist, uint8_t type_attr);

/*
 * Fills `table` (must have HYPE_IDT_ENTRY_COUNT entries), pointing
 * every vector at its corresponding isr_stub_N from `stub_table`
 * (HYPE_IDT_ENTRY_COUNT addresses, one per vector -- see isr_stubs.S).
 */
void hype_idt_build(hype_idt_entry_t *table, const uint64_t *stub_table, uint16_t code_selector);

/* Loads `table` via `lidt`. Never unit tested -- see idt_load.c. */
void hype_idt_load(const hype_idt_entry_t *table, uint16_t entry_count);

/* HYPE_IDT_ENTRY_COUNT stub entry points, one per vector, defined in
 * isr_stubs.S -- pass directly as hype_idt_build()'s stub_table. */
extern const uint64_t hype_isr_stub_table[HYPE_IDT_ENTRY_COUNT];

#endif /* HYPE_ARCH_IDT_H */
