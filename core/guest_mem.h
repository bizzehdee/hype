#ifndef HYPE_CORE_GUEST_MEM_H
#define HYPE_CORE_GUEST_MEM_H

#include <stdint.h>

/*
 * VALID-1: guest-physical address translation + bounds checking.
 *
 * A guest-isolation invariant (AGENTS.md / plan.md §10): device
 * emulation must NEVER dereference a guest-supplied physical address
 * raw. Every guest-physical pointer a device model is handed (a virtio
 * descriptor address, an AHCI command-list/PRDT/FIS address, a
 * framebuffer base, ...) is attacker-controlled from the hypervisor's
 * point of view and must be validated against the VM's own mapped
 * guest-physical layout -- with its length -- before it is turned into
 * a host pointer and touched. Otherwise a malicious or buggy guest can
 * steer a device DMA at hypervisor memory or another VM's RAM.
 *
 * A hype_gpa_map_t describes one VM's guest-physical -> host address
 * layout as a small set of contiguous regions (e.g. FW-1: guest RAM
 * [0, GUEST_RAM) -> its host buffer, plus the flash window near 4GB).
 * hype_gpa_to_host() translates a [gpa, gpa+len) range to a host
 * address ONLY if that whole range lies within a single mapped region;
 * anything out of range, straddling two regions, zero-length, or
 * arithmetic-overflowing returns 0 (invalid) rather than a bogus
 * pointer. Pure logic -- no CPU/VMCB/UEFI dependency, no real memory
 * touched -- so it is exhaustively unit-testable.
 *
 * This module is the VALID-1 primitive; VALID-2/3/4 route each device's
 * guest-supplied buffers through it.
 */

/* Enough for a VM's RAM + firmware-flash window + a few device-backed
 * windows; bump if a future VM legitimately needs more distinct
 * guest-physical regions. */
#define HYPE_GPA_MAP_MAX_REGIONS 8u

typedef struct {
    uint64_t guest_base; /* guest-physical start of the region */
    uint64_t host_base;  /* host address the region maps to */
    uint64_t length;     /* region size in bytes (nonzero) */
} hype_gpa_region_t;

typedef struct {
    hype_gpa_region_t regions[HYPE_GPA_MAP_MAX_REGIONS];
    unsigned count;
} hype_gpa_map_t;

/* Clears the map to empty. Call before adding regions. */
void hype_gpa_map_reset(hype_gpa_map_t *map);

/*
 * Adds a guest-physical -> host region. Returns 0 on success, -1 if the
 * map is full, the length is 0, or guest_base+length / host_base+length
 * would overflow 64 bits (a malformed region is rejected, never stored
 * half-formed). Regions are not required to be sorted or non-adjacent,
 * but callers should not add overlapping guest ranges (the lookup
 * returns the first match).
 */
int hype_gpa_map_add(hype_gpa_map_t *map, uint64_t guest_base, uint64_t host_base, uint64_t length);

/*
 * Validates [gpa, gpa+len) and translates it to a host address.
 * Returns the host address (host_base + (gpa - guest_base)) iff len > 0
 * and the entire range fits within a single region; otherwise 0. Never
 * returns a pointer for a range that straddles a region boundary, runs
 * off the end of a region, or overflows. A 0 return means "reject" --
 * callers must treat it as a fatal/error, never fall back to a raw
 * dereference.
 */
uint64_t hype_gpa_to_host(const hype_gpa_map_t *map, uint64_t gpa, uint64_t len);

/*
 * Bounds-check only: 1 if [gpa, gpa+len) is fully within one region
 * (same rules as hype_gpa_to_host), 0 otherwise. For callers that only
 * need to validate, not translate.
 */
int hype_gpa_range_valid(const hype_gpa_map_t *map, uint64_t gpa, uint64_t len);

#endif /* HYPE_CORE_GUEST_MEM_H */
