#include "gdt.h"

/*
 * Loads the GDT and reloads every segment register. Exempt from unit
 * testing per AGENTS.md: `lgdt` and a live CS reload only make sense
 * with a real CPU executing them -- there is no way to observe this
 * function's effect other than "did the machine keep running." All the
 * actual table-construction logic lives in the tested hype_gdt_build()/
 * hype_gdt_encode_entry() above; this is deliberately just the load.
 */
void hype_gdt_load(const hype_gdt_entry_t *table, uint16_t entry_count) {
    hype_gdt_ptr_t gdtr;

    gdtr.limit = (uint16_t)(entry_count * sizeof(hype_gdt_entry_t) - 1);
    gdtr.base = (uint64_t)table;

    __asm__ volatile(
        "lgdt %0\n\t"
        "mov %1, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %2\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        :
        : "m"(gdtr), "i"(HYPE_GDT_DATA_SEL), "i"(HYPE_GDT_CODE64_SEL)
        : "rax", "memory");
}
