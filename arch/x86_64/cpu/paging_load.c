#include "paging.h"

/*
 * Loads CR3. Exempt from unit testing per AGENTS.md -- same reasoning
 * as gdt_load.c/idt_load.c: only makes sense with a real CPU walking
 * page tables, and the only way to observe its effect is "did the
 * machine keep running with memory still readable/writable."
 */
void hype_paging_load(const hype_pte_t *pml4) {
    __asm__ volatile("mov %0, %%cr3" : : "r"((uint64_t)pml4) : "memory");
}
