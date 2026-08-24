#include "avic.h"

/* #193: pure AVIC table logic. See avic.h. */

int hype_avic_supported(uint32_t svm_feature_edx) {
    return (svm_feature_edx & HYPE_AVIC_CPUID_EDX_BIT) != 0u;
}

uint64_t hype_avic_physical_entry(uint32_t host_apic_id, uint64_t backing_page_phys, int is_running,
                                  int valid) {
    uint64_t e;
    if (!valid) {
        return 0ull;
    }
    e = (uint64_t)host_apic_id & HYPE_AVIC_PHYS_HOST_APIC_ID_MASK;
    e |= backing_page_phys & HYPE_AVIC_PHYS_BACKING_MASK;
    if (is_running) {
        e |= HYPE_AVIC_PHYS_IS_RUNNING;
    }
    e |= HYPE_AVIC_PHYS_VALID;
    return e;
}

uint32_t hype_avic_logical_entry(uint32_t guest_physical_id, int valid) {
    if (!valid) {
        return 0u;
    }
    return (guest_physical_id & HYPE_AVIC_LOG_GUEST_PHYS_ID_MASK) | HYPE_AVIC_LOG_VALID;
}

unsigned int hype_avic_build_physical_table(uint64_t *table, unsigned int table_entries,
                                            const uint32_t *host_apic_ids,
                                            const uint64_t *backing_page_phys, unsigned int count,
                                            uint8_t *max_index_out) {
    unsigned int i;
    if (table == 0 || host_apic_ids == 0 || backing_page_phys == 0 || table_entries == 0u) {
        if (max_index_out) {
            *max_index_out = 0u;
        }
        return 0u;
    }
    if (count > table_entries) {
        count = table_entries;
    }
    for (i = 0; i < table_entries; i++) {
        if (i < count) {
            table[i] = hype_avic_physical_entry(host_apic_ids[i], backing_page_phys[i],
                                                1 /* running */, 1 /* valid */);
        } else {
            table[i] = 0ull;
        }
    }
    if (max_index_out) {
        *max_index_out = (count > 0u) ? (uint8_t)(count - 1u) : 0u;
    }
    return count;
}

int hype_avic_bitmap_highest(const uint32_t words[8]) {
    unsigned int word = 8u;

    while (word-- > 0u) {
        uint32_t bits = words[word];
        unsigned int bit = 32u;
        if (bits == 0u) {
            continue;
        }
        while (bit-- > 0u) {
            if ((bits & (1u << bit)) != 0u) {
                return (int)(word * 32u + bit);
            }
        }
    }
    return -1;
}

int hype_avic_ldr_flat_index(uint32_t ldr) {
    uint32_t logical_id = ldr >> 24;
    unsigned int i;

    if (logical_id == 0u) {
        return -1;
    }
    for (i = 0; i < 8u; i++) {
        if (logical_id == (1u << i)) {
            return (int)i;
        }
    }
    return -1; /* multiple bits set, or a bit above 7 -- not a flat-mode single ID */
}
