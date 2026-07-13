#include "gdt.h"

/* Access bytes: P=1, DPL=00, S=1 (code/data, not system), Type per
 * segment kind. Code: execute/read (0xA). Data: read/write (0x2). */
#define ACCESS_CODE64 0x9A
#define ACCESS_DATA 0x92

/* Flags nibble (G, D/B, L, AVL): code64 sets L=1, D=0; data sets D/B=1
 * (L is only meaningful for code segments). Both set G=1 (4KB
 * granularity) even though base/limit are otherwise ignored in 64-bit
 * mode -- this matches the conventional flat descriptor every OS-dev
 * reference uses, so it reads as familiar rather than surprising. */
#define FLAGS_CODE64 0xA
#define FLAGS_DATA 0xC

void hype_gdt_encode_entry(hype_gdt_entry_t *entry, uint32_t base, uint32_t limit,
                            uint8_t access, uint8_t flags) {
    entry->limit_low = (uint16_t)(limit & 0xFFFFu);
    entry->base_low = (uint16_t)(base & 0xFFFFu);
    entry->base_mid = (uint8_t)((base >> 16) & 0xFFu);
    entry->access = access;
    entry->limit_high_flags = (uint8_t)(((limit >> 16) & 0x0Fu) | ((flags & 0x0Fu) << 4));
    entry->base_high = (uint8_t)((base >> 24) & 0xFFu);
}

void hype_gdt_build(hype_gdt_entry_t *table) {
    hype_gdt_encode_entry(&table[0], 0, 0, 0, 0); /* null descriptor */
    hype_gdt_encode_entry(&table[1], 0, 0xFFFFF, ACCESS_CODE64, FLAGS_CODE64);
    hype_gdt_encode_entry(&table[2], 0, 0xFFFFF, ACCESS_DATA, FLAGS_DATA);
}
