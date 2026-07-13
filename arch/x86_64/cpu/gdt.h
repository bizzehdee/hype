#ifndef HYPE_ARCH_GDT_H
#define HYPE_ARCH_GDT_H

#include <stdint.h>

/*
 * Own GDT (M1-2, plan.md §7 /arch/x86_64/cpu). Minimal 64-bit long-mode
 * table: null, kernel code, kernel data -- no TSS/IST yet (added
 * alongside whichever milestone first needs a separate exception
 * stack). Table encoding is pure logic and unit tested directly;
 * loading it onto the real CPU (lgdt + segment reload) is a thin,
 * hardware-only shim in gdt_load.c, same split as halt.c/panic.c.
 */

#define HYPE_GDT_NULL_SEL 0x00
#define HYPE_GDT_CODE64_SEL 0x08
#define HYPE_GDT_DATA_SEL 0x10
#define HYPE_GDT_ENTRY_COUNT 3

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t limit_high_flags;
    uint8_t base_high;
} __attribute__((packed)) hype_gdt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) hype_gdt_ptr_t;

/*
 * Encodes one 8-byte GDT descriptor. `flags` occupies the *upper* nibble
 * of the limit_high_flags byte (G, D/B, L, AVL from bit 7 down to bit 4);
 * `access` is the full access byte (P, DPL, S, Type). Pure bit-packing,
 * no CPU state touched.
 */
void hype_gdt_encode_entry(hype_gdt_entry_t *entry, uint32_t base, uint32_t limit,
                            uint8_t access, uint8_t flags);

/* Fills `table` (must have HYPE_GDT_ENTRY_COUNT entries) with the fixed
 * null/code64/data layout described above. */
void hype_gdt_build(hype_gdt_entry_t *table);

/*
 * Loads `table` via `lgdt`, then reloads every segment register
 * (CS via a far-return trick, since there's no far jmp in 64-bit mode)
 * to point at the new descriptors. Never unit tested -- see gdt_load.c.
 */
void hype_gdt_load(const hype_gdt_entry_t *table, uint16_t entry_count);

#endif /* HYPE_ARCH_GDT_H */
