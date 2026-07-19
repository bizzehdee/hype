#include "pvclock.h"

/* (numerator << 32) / denominator, the pvclock multiplier's fractional form. */
static uint32_t div_frac(uint64_t numerator, uint32_t denominator) {
    return (uint32_t)((numerator << 32) / denominator);
}

void hype_pvclock_calc_scale(uint64_t tsc_hz, uint32_t *out_mul, int8_t *out_shift) {
    /* kvm_get_time_scale(scaled_hz = NSEC_PER_SEC, base_hz = tsc_hz): find
     * shift + 32-bit multiplier so that a value counted at base_hz, run
     * through pvclock_scale_delta, comes out counted at scaled_hz. */
    uint64_t scaled_hz = 1000000000ULL; /* nanoseconds per second (target) */
    uint64_t base_hz = tsc_hz;          /* TSC cycles per second (source) */
    uint64_t tps64;
    uint32_t tps32;
    int shift = 0;

    if (base_hz == 0) {
        *out_mul = 0;
        *out_shift = 0;
        return;
    }

    tps64 = base_hz;
    /* Shrink base into a range where it fits 32 bits and is <= 2x target. */
    while (tps64 > (scaled_hz * 2ULL) || (tps64 >> 32) != 0) {
        tps64 >>= 1;
        shift--;
    }

    tps32 = (uint32_t)tps64;
    /* Grow base until it exceeds the target, so the division below yields a
     * multiplier with full 32-bit fractional range. (The canonical KVM
     * routine also handles a target that overflows 32 bits by halving it, but
     * our target is fixed at NSEC_PER_SEC = 1e9, which always fits, so that
     * branch is omitted as dead code.) */
    while (tps32 <= scaled_hz) {
        tps32 <<= 1;
        shift++;
    }

    *out_shift = (int8_t)shift;
    *out_mul = div_frac(scaled_hz, tps32);
}

uint64_t hype_pvclock_scale_delta(uint64_t tsc_delta, uint32_t mul, int8_t shift) {
    uint64_t d = tsc_delta;
    if (shift < 0) {
        d >>= (unsigned int)(-shift);
    } else {
        d <<= (unsigned int)shift;
    }
    /* 64x32 -> take the high 32 of the 96-bit product's relevant window; the
     * guest does the same mulhi, matching pvclock_scale_delta(). */
    return (uint64_t)(((unsigned __int128)d * mul) >> 32);
}

/* Compiler barrier -- keeps the version bumps ordered around the field stores
 * so a concurrently-reading guest vCPU never observes a torn update. */
#define HYPE_PVCLOCK_BARRIER() __asm__ __volatile__("" ::: "memory")

void hype_pvclock_write_time_info(volatile struct hype_pvclock_vcpu_time_info *ti,
                                   uint64_t tsc_timestamp, uint64_t system_time_ns, uint32_t mul,
                                   int8_t shift, uint8_t flags) {
    uint32_t v = ti->version;

    ti->version = v + 1; /* odd: update in progress */
    HYPE_PVCLOCK_BARRIER();
    ti->pad0 = 0;
    ti->tsc_timestamp = tsc_timestamp;
    ti->system_time = system_time_ns;
    ti->tsc_to_system_mul = mul;
    ti->tsc_shift = shift;
    ti->flags = flags;
    ti->pad[0] = 0;
    ti->pad[1] = 0;
    HYPE_PVCLOCK_BARRIER();
    ti->version = v + 2; /* even: update complete */
}

void hype_pvclock_write_wall_clock(volatile struct hype_pvclock_wall_clock *wc, uint32_t sec,
                                    uint32_t nsec) {
    uint32_t v = wc->version;

    wc->version = v + 1;
    HYPE_PVCLOCK_BARRIER();
    wc->sec = sec;
    wc->nsec = nsec;
    HYPE_PVCLOCK_BARRIER();
    wc->version = v + 2;
}
