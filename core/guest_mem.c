#include "guest_mem.h"

void hype_gpa_map_reset(hype_gpa_map_t *map) {
    map->count = 0;
}

int hype_gpa_map_add(hype_gpa_map_t *map, uint64_t guest_base, uint64_t host_base, uint64_t length) {
    hype_gpa_region_t *r;

    if (map->count >= HYPE_GPA_MAP_MAX_REGIONS) {
        return -1;
    }
    if (length == 0) {
        return -1;
    }
    /* Reject a region whose end would wrap past 2^64 in either address
     * space -- a malformed region must never be stored, since the
     * lookup's containment math assumes base+length does not overflow. */
    if (guest_base > UINT64_MAX - length || host_base > UINT64_MAX - length) {
        return -1;
    }

    r = &map->regions[map->count];
    r->guest_base = guest_base;
    r->host_base = host_base;
    r->length = length;
    map->count++;
    return 0;
}

/*
 * Shared containment check for both public entry points. Returns 1 and
 * writes the translated host address to *out_host iff len > 0 and the
 * whole [gpa, gpa+len) range lies within a single region; returns 0
 * otherwise (out_host untouched). Keeping the validity result separate
 * from the address avoids conflating "invalid" with a legitimate host
 * address of 0.
 */
static int lookup(const hype_gpa_map_t *map, uint64_t gpa, uint64_t len, uint64_t *out_host) {
    unsigned i;

    if (len == 0) {
        return 0;
    }

    for (i = 0; i < map->count; i++) {
        const hype_gpa_region_t *r = &map->regions[i];
        uint64_t offset;

        if (gpa < r->guest_base) {
            continue;
        }
        offset = gpa - r->guest_base;
        if (offset >= r->length) {
            continue;
        }
        /* Whole [gpa, gpa+len) within the region? r->length - offset is
         * safe (offset < r->length); comparing len against it avoids
         * computing gpa+len (which could overflow). A range running past
         * this region's end is rejected outright -- NOT retried against a
         * later region, because a device buffer must be contiguous
         * within a single mapped region. */
        if (len > r->length - offset) {
            return 0;
        }
        *out_host = r->host_base + offset;
        return 1;
    }

    return 0;
}

uint64_t hype_gpa_to_host(const hype_gpa_map_t *map, uint64_t gpa, uint64_t len) {
    uint64_t host = 0;
    if (lookup(map, gpa, len, &host)) {
        return host;
    }
    return 0;
}

int hype_gpa_range_valid(const hype_gpa_map_t *map, uint64_t gpa, uint64_t len) {
    uint64_t host = 0;
    return lookup(map, gpa, len, &host);
}
