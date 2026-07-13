#ifndef HYPE_ARCH_PAGING_H
#define HYPE_ARCH_PAGING_H

#include <stdint.h>

/*
 * Own paging (M1-3): a flat identity map (every physical address maps
 * to the same virtual address) covering the first HYPE_PAGING_MAX_GB
 * gigabytes, built from 2MB pages (PD entries with PS=1) so a 3-level
 * hierarchy (PML4 -> PDPT -> PD) suffices -- no need for 4KB PT-level
 * tables at this stage. UEFI firmware already runs with paging enabled
 * (long mode requires it) and already identity-maps its own memory, so
 * swapping in our own tables via a plain CR3 reload is safe as long as
 * our map covers everything firmware/our own code currently touches --
 * same reasoning already validated for the GDT/IDT swap in M1-2.
 *
 * Same split as gdt.h/idt.h: entry encoding and the identity-map
 * builder are pure logic, unit tested directly; `mov cr3` is a thin,
 * hardware-only shim in paging_load.c.
 */

#define HYPE_PAGING_ENTRIES_PER_TABLE 512
#define HYPE_PAGING_2MB (2ULL * 1024 * 1024)
#define HYPE_PAGING_1GB (1024ULL * 1024 * 1024)

/* Generous default covering any realistic dev/test machine's RAM;
 * revisit if a real system needs more (the array is sized for the max,
 * hype_paging_build_identity only populates gb_to_map of it, so bumping
 * this is just a bigger static allocation, not a design change). */
#define HYPE_PAGING_MAX_GB 64

#define HYPE_PAGING_PRESENT (1ULL << 0)
#define HYPE_PAGING_WRITE (1ULL << 1)
#define HYPE_PAGING_USER (1ULL << 2)
#define HYPE_PAGING_PS (1ULL << 7)

typedef uint64_t hype_pte_t;

/*
 * Encodes one page-table entry (PML4E/PDPTE/PDE, all the same 8-byte
 * shape): `addr` is the next table's (or, with PS set, the final page's)
 * physical address, masked to bits 12-51; `flags` is everything else
 * (Present/RW/PS/... in bits 0-11, NX in bit 63) masked to just those
 * bits so a caller can't accidentally set a reserved bit. Pure
 * bit-packing, no CPU state touched.
 */
uint64_t hype_paging_encode_entry(uint64_t addr, uint64_t flags);

/*
 * Fills pml4[0] -> pdpt -> pd_tables[0..gb_to_map-1], each pd_tables[i]
 * mapping [i*1GB, i*1GB+1GB) via 512 2MB pages. All PML4/PDPT entries
 * beyond what's used are left not-present. Every table (pml4, pdpt,
 * each pd_tables[i]) must be a caller-owned, 4KB-aligned
 * HYPE_PAGING_ENTRIES_PER_TABLE-entry array -- alignment is enforced at
 * the point of definition (e.g. `__attribute__((aligned(4096)))`), not
 * by this function.
 */
void hype_paging_build_identity(hype_pte_t *pml4, hype_pte_t *pdpt,
                                 hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE],
                                 unsigned int gb_to_map);

/* Loads `pml4`'s physical address into CR3. Never unit tested -- see
 * paging_load.c. */
void hype_paging_load(const hype_pte_t *pml4);

#endif /* HYPE_ARCH_PAGING_H */
