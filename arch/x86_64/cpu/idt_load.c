#include "idt.h"

/*
 * Masks interrupts, then loads the IDT via `lidt`. Exempt from unit
 * testing per AGENTS.md -- same reasoning as gdt_load.c: only makes
 * sense with a real CPU, and the only way to observe its effect is "did
 * a subsequent interrupt get routed correctly," which needs a live
 * vector to fire.
 *
 * The `cli` isn't optional bring-up hygiene: every vector in this IDT
 * is fatal (M1's policy, isr_entry.c). It only durably holds, though,
 * if the caller installs the IDT *after* it's done needing Boot
 * Services -- UEFI's Boot Services calls (ConOut, GetMemoryMap, ...)
 * can re-enable interrupts as a documented side effect of raising and
 * restoring TPL internally, which is outside our control. Installing
 * this IDT any earlier races a live timer IRQ against every subsequent
 * Boot Services call (confirmed empirically while building M1-5: it
 * panicked at a different point on every run depending on timing).
 * See boot/main.c for where this actually gets called and why.
 */
void hype_idt_load(const hype_idt_entry_t *table, uint16_t entry_count) {
    hype_idt_ptr_t idtr;

    idtr.limit = (uint16_t)(entry_count * sizeof(hype_idt_entry_t) - 1);
    idtr.base = (uint64_t)table;

    __asm__ volatile("cli\n\t"
                      "lidt %0"
                      :
                      : "m"(idtr));
}

/* Masks/unmasks interrupts directly. Exempt from unit testing -- same
 * reasoning as hype_idt_load() above. */
void hype_cli(void) {
    __asm__ volatile("cli");
}

void hype_sti(void) {
    __asm__ volatile("sti");
}
