#ifndef HYPE_CORE_VM_ISOLATION_H
#define HYPE_CORE_VM_ISOLATION_H

#include <stdint.h>

/*
 * #274: a self-check that two concurrently-running guests are actually isolated,
 * rather than merely both alive.
 *
 * Two login prompts prove liveness and nothing else -- a pair of guests sharing
 * one translation root would produce exactly the same two prompts while
 * providing no isolation at all. What actually separates them is (a) disjoint
 * host RAM and (b) distinct second-level translation roots (EPT PML4 on Intel,
 * NPT PML4 on AMD). Both guests here run their filesystem from a RAM overlay
 * inside their own guest RAM, so filesystem isolation follows from memory
 * isolation; checking the memory is checking the thing underneath.
 *
 * Pure arithmetic on addresses the caller reads out of its VM structs -- no
 * hardware, so it is unit-tested.
 */

/* Bit flags: 0 means isolated. Every failing condition is reported, not just the
 * first, so one run names every problem. */
#define HYPE_VM_ISOLATION_OK 0u
/* The two guests' RAM ranges overlap -- each can reach the other's memory. */
#define HYPE_VM_ISOLATION_RAM_OVERLAP (1u << 0)
/* Both guests were handed the same translation root, so they share an address
 * space no matter where their RAM sits. */
#define HYPE_VM_ISOLATION_SAME_ROOT (1u << 1)
/* A zero-sized or zero-based range, or a null root: not an isolation failure as
 * such, but it means the caller passed something unconfigured and the other
 * answers cannot be trusted. Reported rather than silently passing. */
#define HYPE_VM_ISOLATION_UNCONFIGURED (1u << 2)

/*
 * Returns a bitwise OR of the flags above; HYPE_VM_ISOLATION_OK (0) iff the two
 * guests are separated. Sizes are in bytes. Ranges touching end-to-end (a's last
 * byte immediately before b's first) do NOT overlap.
 */
unsigned hype_vm_isolation_check(uint64_t a_ram_base, uint64_t a_ram_size, uint64_t a_root,
                                 uint64_t b_ram_base, uint64_t b_ram_size, uint64_t b_root);

/* Human-readable one-line reason for a flag set, for the boot log. Returns
 * "isolated" when flags == 0. */
const char *hype_vm_isolation_describe(unsigned flags);

#endif /* HYPE_CORE_VM_ISOLATION_H */
