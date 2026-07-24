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

/*
 * Programs IA32_PAT (MSR 0x277) so slot 1 is Write-Combining. Value
 * 0x0007040600070106 = the reset default (PA0=WB,PA2=UC-,PA3=UC,PA4=WB,PA5=WT,
 * PA6=UC-,PA7=UC) with only PA1 changed from WT(0x04) to WC(0x01). A PDE/PTE
 * selecting PA1 (PWT=1,PCD=0,PAT=0) then reads/writes WC even where an MTRR marks
 * the page UC (WC is the one PAT type that wins over MTRR-UC on both AMD and
 * Intel). Exempt from unit testing (wrmsr / per-CPU MSR), like hype_paging_load.
 */
void hype_paging_set_pat_wc(void) {
    uint32_t lo = 0x00070106u; /* PA3..PA0 = 00 07 01 06 */
    uint32_t hi = 0x00070406u; /* PA7..PA4 = 00 07 04 06 */
    __asm__ volatile("wrmsr" : : "c"(0x277u), "a"(lo), "d"(hi));
}
