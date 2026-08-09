#include "mtrr.h"

#define PHYSMASK_VALID (1ull << 11)
#define ADDR_BITS (~0xFFFull) /* bits 12 and up; reserved high bits read as 0 */

uint8_t hype_mtrr_type_for(uint64_t addr, const hype_mtrr_var_t *var, unsigned int count,
                           uint64_t def_type) {
    unsigned int i;
    int matched = 0;
    int saw_uc = 0, saw_wt = 0, saw_wb = 0;
    uint8_t first = HYPE_MTRR_INVALID;

    if ((def_type & HYPE_MTRR_DEF_E) == 0ull) return HYPE_MTRR_INVALID;
    if (var == 0) count = 0;
    if (count > HYPE_MTRR_MAX_VAR) count = HYPE_MTRR_MAX_VAR;

    for (i = 0; i < count; i++) {
        uint64_t m;
        uint8_t type;
        if ((var[i].mask & PHYSMASK_VALID) == 0ull) continue;
        m = var[i].mask & ADDR_BITS;
        if (m == 0ull) continue; /* a zero mask matches everything; treat as unprogrammed */
        if ((addr & m) != (var[i].base & m)) continue;
        type = (uint8_t)(var[i].base & 0xFFu);
        if (!matched) first = type;
        matched = 1;
        if (type == HYPE_MTRR_UC) saw_uc = 1;
        else if (type == HYPE_MTRR_WT) saw_wt = 1;
        else if (type == HYPE_MTRR_WB) saw_wb = 1;
    }

    if (!matched) return (uint8_t)(def_type & HYPE_MTRR_DEF_TYPE_MASK);
    /* Overlap rules: UC beats everything; WT beats WB. Anything else that overlaps is
     * architecturally undefined, so report the first match rather than invent a winner. */
    if (saw_uc) return HYPE_MTRR_UC;
    if (saw_wt && saw_wb) return HYPE_MTRR_WT;
    return first;
}

const char *hype_mtrr_type_name(uint8_t type) {
    switch (type) {
        case HYPE_MTRR_UC: return "UC";
        case HYPE_MTRR_WC: return "WC";
        case HYPE_MTRR_WT: return "WT";
        case HYPE_MTRR_WP: return "WP";
        case HYPE_MTRR_WB: return "WB";
        default: return "??";
    }
}

uint8_t hype_pat_entry(uint64_t pat, unsigned int index) {
    if (index > 7u) return HYPE_MTRR_INVALID;
    return (uint8_t)((pat >> (index * 8u)) & 0xFFu);
}
