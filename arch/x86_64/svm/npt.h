#ifndef HYPE_ARCH_SVM_NPT_H
#define HYPE_ARCH_SVM_NPT_H

#include "../cpu/paging.h"

/*
 * NPT (Nested Page Tables, M3-1): AMD's NPT reuses the exact same
 * page-table-entry format as ordinary long-mode paging (AMD SDM) --
 * hype_pte_t/hype_paging_encode_entry (arch/x86_64/cpu/paging.h)
 * apply unchanged, same as the host's own M1-3 identity map. The one
 * difference: every level here also sets the User/Supervisor bit.
 * NPT translates guest-*physical* addresses regardless of what
 * privilege level the guest itself is executing at -- the guest's own
 * page tables (if it has any) are what enforce supervisor/user
 * semantics for the guest; NPT sits underneath that translation and
 * must not add its own extra restriction, or any guest-mode (CPL>0)
 * access would fault against NPT before even reaching the guest's own
 * paging.
 */

/* Was a separate, smaller fixed constant (4) until real-hardware
 * testing (M4-5's real-AMD-hardware pass) found it insufficient: this
 * project's own static guest/test buffers live in the same statically
 * linked image as everything else, and real UEFI firmware is free to
 * load that image well above 4GB of physical address space (confirmed
 * on real hardware -- a static buffer landed just past 5GB) even
 * though QEMU's own small test VMs never happen to place it that high.
 * NPT must cover at least as much as the host's own identity map
 * already does (HYPE_PAGING_MAX_GB, paging.h) for exactly the same
 * reason, so it's tied to that constant directly instead of drifting
 * out of sync with it again. Revisit once real per-VM memory sizing
 * (hype.cfg's mem_mb, already summed by ADM-1) drives this instead. */
#define HYPE_NPT_MAX_GB HYPE_PAGING_MAX_GB

/*
 * Fills pml4[0] -> pdpt -> pd_tables[0..gb_to_map-1] as a flat
 * identity map (guest-physical == host-physical) via 2MB pages --
 * same shape and ownership/alignment requirements as
 * hype_paging_build_identity(), but with the User/Supervisor bit also
 * set on every level (see above). Pure struct-filling, no CPU state
 * touched.
 */
void hype_npt_build_identity(hype_pte_t *pml4, hype_pte_t *pdpt,
                              hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE],
                              unsigned int gb_to_map);

/*
 * Marks the 2MB PD entry covering `phys_addr` as not-present (M4-3):
 * any guest access anywhere in that 2MB range then takes an NPF
 * (#VMEXIT_NPF) instead of a direct memory access, which is how this
 * project traps MMIO-style devices (the emulated CFI flash,
 * devices/pflash.h, being the first) without needing 4KB-granularity
 * NPT at all -- coarse (whole 2MB), but simple, and correct as long as
 * the device's actual backing region is placed within its own
 * dedicated 2MB range not shared with real guest RAM. `phys_addr` must
 * fall within the range hype_npt_build_identity() already mapped
 * (gb_to_map GB) -- this only clears an existing entry, it doesn't
 * grow the table. Pure struct mutation, no CPU state touched.
 */
void hype_npt_mark_not_present(hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE], uint64_t phys_addr);

/*
 * Marks every 2MB PD entry in [base, base+size) not-present (FW-1a):
 * the range version of hype_npt_mark_not_present(), used to leave a
 * guest a clean map where only its real RAM region and its firmware
 * flash window are present and EVERYTHING else faults. Any guest access
 * to an unmapped 2MB range then takes a located #VMEXIT_NPF (with the
 * guest-physical address in EXITINFO2) instead of silently reaching
 * whatever the flat identity map would otherwise have pointed at --
 * critically, the host's own sub-4GB MMIO hole, which reads all-1s and
 * is exactly what let real OVMF read a garbage pointer and jump to it.
 * `base` and `size` must both be 2MB-aligned; the whole range must fall
 * within what hype_npt_build_identity() already sized (gb_to_map GB) --
 * this only clears existing entries, it doesn't grow the table. Pure
 * struct mutation, no CPU state touched.
 */
void hype_npt_mark_range_not_present(hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE], uint64_t base,
                                      uint64_t size);

/*
 * Remaps `size` bytes (a whole multiple of HYPE_PAGING_2MB) of guest-
 * physical address space, starting at `guest_phys_base`, to
 * `host_phys_base` instead of the identity mapping
 * hype_npt_build_identity() already put there for that range (FW-1).
 * Needed whenever a guest-physical address is architecturally fixed
 * (e.g. the classic x86 reset-vector convention's top-of-4GB flash
 * window that real, unmodified guest firmware assumes) but does NOT
 * correspond to real, available host RAM at that same literal
 * physical address -- confirmed the hard way: that exact range is
 * where the underlying machine's own real firmware flash lives
 * (whether that's a real motherboard's BIOS/UEFI flash, or, when
 * nested under another hypervisor the way this project's own dev
 * environment runs, the L0 host's own OVMF), not general-purpose RAM
 * this project can safely repurpose -- attempting to identity-map and
 * write into it corrupts the underlying host's own firmware state.
 * Must be called AFTER hype_npt_build_identity() for the same
 * pd_tables, since it overwrites whatever entries the identity sweep
 * already wrote for this range. `guest_phys_base` and `host_phys_base`
 * must both be 2MB-aligned. Pure struct mutation, no CPU state
 * touched.
 */
void hype_npt_map_range(hype_pte_t pd_tables[][HYPE_PAGING_ENTRIES_PER_TABLE], uint64_t guest_phys_base,
                         uint64_t host_phys_base, uint64_t size);

#endif /* HYPE_ARCH_SVM_NPT_H */
