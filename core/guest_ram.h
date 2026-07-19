#ifndef HYPE_CORE_GUEST_RAM_H
#define HYPE_CORE_GUEST_RAM_H

#include <stdint.h>

/*
 * Guest RAM zeroing (M2-6). Hard invariant from plan.md §10 decision
 * #15 / AGENTS.md: "Guest RAM is zeroed before first execution, on
 * every (re)start, including after Force power off -- never reused
 * as-is." Every page of a VM's reserved guest RAM must be zeroed
 * immediately before that VM's first instruction runs, every single
 * time, so no guest ever observes stale contents left behind by a
 * prior occupant of that memory (a previous guest, or hypervisor
 * scratch data). Applies to the *entire* range handed to a guest, not
 * just the pages the hypervisor happens to write itself (a loaded
 * kernel/initrd image, say) -- anything left unzeroed is exactly the
 * leak this exists to prevent. Does not apply to Stop/Resume, which
 * intentionally preserves guest RAM across the pause (plan.md §6f).
 */

/*
 * Zeroes `size` bytes starting at `base`. Callers must pass the full
 * host-physical (identity-mapped) range that will be handed to a
 * guest as its RAM, before that guest's first VM-entry, on every
 * (re)start of that VM. Pure -- plain memory writes, no CPU/MSR/VMCB/
 * VMCS state touched, no UEFI dependency.
 */
void hype_guest_ram_zero(void *base, uint64_t size);

/*
 * Copies `size` bytes from `src` to `dst` (both host-physical,
 * identity-mapped). M8-0b STEP 2 uses this to give a second VM its own
 * pristine copy of the OVMF firmware image already loaded for the first VM
 * (each VM needs a private, writable UEFI variable store, so they cannot
 * share one buffer). Pure -- plain memory copy, no overlap handling needed
 * (the two guest buffers are always distinct allocations).
 */
void hype_guest_ram_copy(void *dst, const void *src, uint64_t size);

#endif /* HYPE_CORE_GUEST_RAM_H */
