#include "idt.h"

/*
 * Loads the IDT via `lidt`. Exempt from unit testing per AGENTS.md --
 * same reasoning as gdt_load.c: only makes sense with a real CPU, and
 * the only way to observe its effect is "did a subsequent interrupt get
 * routed correctly," which needs a live vector to fire.
 */
void hype_idt_load(const hype_idt_entry_t *table, uint16_t entry_count) {
    hype_idt_ptr_t idtr;

    idtr.limit = (uint16_t)(entry_count * sizeof(hype_idt_entry_t) - 1);
    idtr.base = (uint64_t)table;

    __asm__ volatile("lidt %0" : : "m"(idtr));
}
