#include "guest_mtrr.h"

/* Which fix[] slot an MSR number names, or -1. The fixed MTRRs are not contiguous:
 * 0x250, then 0x258/0x259, then 0x268-0x26F. */
static int fixed_index(uint32_t msr) {
    if (msr == 0x250u) return 0;
    if (msr == 0x258u) return 1;
    if (msr == 0x259u) return 2;
    if (msr >= 0x268u && msr <= 0x26Fu) return 3 + (int)(msr - 0x268u);
    return -1;
}

int hype_guest_mtrr_is_msr(uint32_t msr) {
    return msr == 0xFEu || msr == 0x2FFu || (msr >= 0x200u && msr <= 0x20Fu) ||
           fixed_index(msr) >= 0;
}

void hype_guest_mtrr_reset(hype_guest_mtrr_t *m) {
    unsigned int i;

    if (m == (hype_guest_mtrr_t *)0) return;
    m->deftype = 0x0806u;                                  /* E=1, type=WB, FE=0 -- see header */
    for (i = 0; i < 16u; i++) m->var[i] = 0;               /* disabled; the default is WB */
    for (i = 0; i < 11u; i++) m->fix[i] = 0x0606060606060606ull; /* all WB */
}

void hype_guest_mtrr_write(hype_guest_mtrr_t *m, uint32_t msr, uint64_t value) {
    int fi;

    if (m == (hype_guest_mtrr_t *)0) return;
    if (msr == 0x2FFu) {
        m->deftype = value;
        return;
    }
    if (msr >= 0x200u && msr <= 0x20Fu) {
        m->var[msr - 0x200u] = value;
        return;
    }
    fi = fixed_index(msr);
    if (fi >= 0) {
        m->fix[fi] = value;
    }
    /* 0xFE (MTRRcap) and anything else: dropped. */
}

uint64_t hype_guest_mtrr_read(const hype_guest_mtrr_t *m, uint32_t msr) {
    int fi;

    if (msr == 0xFEu) return (uint64_t)HYPE_GUEST_MTRRCAP;
    if (m == (const hype_guest_mtrr_t *)0) return 0;
    if (msr == 0x2FFu) return m->deftype;
    if (msr >= 0x200u && msr <= 0x20Fu) return m->var[msr - 0x200u];
    fi = fixed_index(msr);
    if (fi >= 0) return m->fix[fi];
    return 0;
}
