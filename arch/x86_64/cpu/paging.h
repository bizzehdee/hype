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
#define HYPE_PAGING_PWT (1ULL << 3)
#define HYPE_PAGING_PCD (1ULL << 4) /* page cache disable -- uncacheable (MMIO) */
#define HYPE_PAGING_PS (1ULL << 7)
#define HYPE_PAGING_NX (1ULL << 63) /* requires EFER.NXE=1 -- see hype_paging_apply_nx()'s caller */

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

/*
 * #535: the same identity hierarchy, but for tables that live at a DIFFERENT
 * address than the one the entries must name.
 *
 * hype_paging_build_identity() encodes each table's own C pointer as the next
 * level's physical address, which is only correct when the pointer already IS
 * that address -- true for hype's own host tables and for the identity-mapped
 * microtest guests, and false for a configured VM. Such a guest's page tables
 * have to sit in ITS guest RAM, so hype writes them through a host pointer
 * (`gpa0_host` + offset) while the entries must name guest-physical addresses.
 * Encoding the host pointer there hands the guest a CR3 pointing into host RAM.
 *
 * `gpa0_host` is where guest-physical 0 is mapped in the caller's address
 * space. `pml4_gpa`, `pdpt_gpa` and `pd0_gpa` are guest-physical, 4KB-aligned,
 * and must all fall inside the caller's mapping; the PD tables are `gb_to_map`
 * consecutive pages from `pd0_gpa`. The resulting map is identity over
 * [0, gb_to_map GB) in 2MB pages, so guest-virtual == guest-physical.
 */
void hype_paging_build_identity_at(void *gpa0_host, uint64_t pml4_gpa, uint64_t pdpt_gpa,
                                   uint64_t pd0_gpa, unsigned int gb_to_map);

/*
 * Adds a 2MB-page identity mapping for the 1GB-aligned region(s) that
 * cover [phys_base, phys_base+size) into an already-built PML4[0]->pdpt
 * hierarchy (i.e. call after hype_paging_build_identity, sharing its
 * pdpt). For a physical region ABOVE the low identity map -- notably a
 * GOP framebuffer BAR that firmware placed in high MMIO space (e.g.
 * 256GB on an Intel i5-13420H), which would otherwise be unmapped the
 * instant CR3 is loaded, taking the debug console down with it. Wires
 * pdpt[gb] -> pd_tables[n] for each 1GB slot touched (n = 0,1,...) and
 * fills each pd with that GB's 512 2MB identity pages. pd_tables must
 * provide one table per GB the region spans; a realistic framebuffer
 * touches 1 (or 2 if it straddles a 1GB boundary). Requires
 * phys_base+size <= 512GB (a single PML4[0] entry). Returns the number
 * of GB slots mapped, 0 if the region is out of PML4[0] range or empty.
 * Pure table-filling, no CPU state touched.
 */
/*
 * Maps the 1 GiB region containing `phys` into `pml4` at its natural PML4/PDPT
 * index, using caller-owned `pdpt` and `pd` tables (each a 4KB-aligned
 * 512-entry array), via 512 uncacheable (PCD) 2MB PS pages. Unlike
 * hype_paging_map_region_2mb this reaches ANY address, including above
 * PML4[0]'s low 512 GiB -- e.g. a 64-bit MMIO BAR firmware placed high (an
 * NVMe controller's register window). PCD is set because MTRR coverage of a
 * high 64-bit MMIO hole can't be assumed. Returns the PML4 index populated.
 * Pure: touches only the three supplied tables.
 */
unsigned int hype_paging_map_mmio_1gb(hype_pte_t *pml4, hype_pte_t *pdpt, hype_pte_t *pd,
                                       uint64_t phys);

/*
 * Same job as hype_paging_map_mmio_1gb -- one uncacheable (PCD) 1 GiB identity
 * window for a device register BAR -- but wired into an EXISTING `pdpt` rather
 * than a dedicated one installed at a fresh PML4 slot. Sets only pdpt[gb % 512]
 * and fills `pd`; never touches the PML4 or any other PDPT entry, so the low
 * identity map around it survives.
 *
 * This covers the gap the two existing helpers left open (#240): a 64-bit BAR
 * above the low identity map (HYPE_PAGING_MAX_GB) but still inside PML4[0]'s
 * 512 GiB. hype_paging_map_mmio_1gb cannot be used there -- it writes
 * pml4[0], replacing the whole low map with a near-empty PDPT -- and
 * hype_paging_map_region_2mb reaches it but maps cacheable pages, which is
 * wrong for device registers. Real firmware lands squarely in that gap: an
 * Intel i5-13420H parks its xHCI BAR at 0x6001120000 (384 GiB), i.e. ~6x above
 * the 64 GiB map yet well inside PML4[0], and the first register read #PF'd.
 *
 * `phys` must be below 512 GiB (assert-by-construction: the caller picks this
 * helper precisely because it is). Returns the PDPT index populated. Pure
 * table-filling, no CPU state touched -- the caller reloads CR3.
 */
unsigned int hype_paging_map_mmio_1gb_into_pdpt(hype_pte_t *pdpt, hype_pte_t *pd, uint64_t phys);

unsigned int hype_paging_map_region_2mb(hype_pte_t *pdpt,
                                         hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE],
                                         uint64_t phys_base, uint64_t size);

/*
 * PERF-2 companion for regions mapped by hype_paging_map_region_2mb: same
 * write-combining marking as hype_paging_mark_region_wc, but for `pd_tables`
 * whose slot 0 is the GB containing `base` rather than GB 0.
 *
 * The absolute-indexed version cannot be used on those tables at all: for a
 * framebuffer at 256 GiB it would index pd_tables[256] and run off the end of
 * the 2-entry array the caller owns (and its `gb >= gb_mapped` bound would
 * reject the region outright). That mismatch is why a high-mapped framebuffer
 * silently stayed uncached: the low-map branch marks WC, the high-map branch
 * had no way to. Every console blit then paid full uncached-MMIO cost -- the
 * ~30x the PERF-2 note above describes -- which is what made pre-EBS visibly
 * slower on an Intel i5-13420H (framebuffer at 256 GiB) than on the AMD box
 * (framebuffer at 0xe0000000, inside the low map and therefore already WC).
 *
 * `gb_mapped` is hype_paging_map_region_2mb's return value. Pure.
 */
void hype_paging_mark_region_wc_relative(hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE],
                                          uint64_t base, uint64_t size, unsigned int gb_mapped);

/* Loads `pml4`'s physical address into CR3. Never unit tested -- see
 * paging_load.c. */
void hype_paging_load(const hype_pte_t *pml4);

/* PERF-2 (#234): mark the 2MB identity pages covering [base, base+size) as
 * write-combining, by OR-ing PWT (bit 3) into their PDEs. Combined with a PAT
 * whose slot 1 is WC (hype_paging_set_pat_wc), the effective type becomes WC even
 * where MTRRs mark the region UC -- e.g. the GOP framebuffer, so hype's per-frame
 * blit isn't ~30x slowed by uncached MMIO writes. `pd_tables` is the same
 * [gb][512] array passed to hype_paging_build_identity; `gb_mapped` bounds it.
 * Only touches PDEs already present (built as PS 2MB pages). Pure. */
void hype_paging_mark_region_wc(hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE],
                                uint64_t base, uint64_t size, unsigned int gb_mapped);

/*
 * #604: marks NX (bit 63) on every PRESENT 2MB leaf in pd_tables[0..gb_mapped-1] EXCEPT the
 * 2MB-aligned pages overlapping [exec_base, exec_base+exec_size) or [exec2_base, exec2_size) --
 * guest RAM, DMA buffers, stacks and every other host mapping become non-executable; the one
 * or two ranges the CPU must still be able to fetch instructions from are left alone. The
 * second range exists for the AP identity map: hype's own image is the BSP's only exec range,
 * but an AP's trampoline blob (ap_trampoline.S, copied to a page below 1MB) keeps executing
 * FROM THAT PAGE for several instructions after it loads CR3 and sets CR0.PG -- every
 * instruction fetch from that point is already walking these very tables, so that page must
 * stay executable too, in the AP's table only (pass exec2_size = 0 for the BSP's own g_pd,
 * which the trampoline never runs under). 2MB granularity only -- W^X (making even the image's
 * OWN .data/.rodata non-executable at section granularity) is a separate, finer-grained
 * follow-up. exec_size == 0 exempts nothing from the first range; the caller (efi_main) treats
 * a failure to determine its own image extent as fatal rather than reaching this with
 * exec_size == 0 by accident.
 *
 * The caller MUST have EFER.NXE set (via wrmsr) BEFORE the CR3 carrying these tables is ever
 * loaded -- with NXE=0, bit 63 is a RESERVED bit in a paging-structure entry, not an ignored
 * one, and any entry with it set faults with a reserved-bit violation the instant it is used to
 * translate anything at all. This function does not touch EFER; it only encodes the bit.
 *
 * Pure table-editing, no CPU state touched -- same shape as hype_paging_mark_region_wc.
 */
void hype_paging_apply_nx(hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE],
                          unsigned int gb_mapped, uint64_t exec_base, uint64_t exec_size,
                          uint64_t exec2_base, uint64_t exec2_size);

/* Programs IA32_PAT (MSR 0x277) so slot 1 = WC (the default, but with PA1 changed
 * from WT to WC): 0x0007040600070106. Selected by a PDE/PTE with PWT=1,PCD=0,
 * PAT=0. Must run on every core that blits the framebuffer (each has its own PAT).
 * Never unit tested (wrmsr) -- see paging_load.c. */
void hype_paging_set_pat_wc(void);

#endif /* HYPE_ARCH_PAGING_H */
