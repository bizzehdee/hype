#ifndef HYPE_CORE_MTRR_H
#define HYPE_CORE_MTRR_H

#include <stdint.h>

/*
 * #368: which memory type do the MTRRs give an address?
 *
 * hype marks the GOP framebuffer write-combining in its own page tables (PAT slot 1) and
 * programs IA32_PAT on every core that blits. Both say WC. But a real blit on the Intel box
 * measured 180 KB in 11.03 ms -- 16 MB/s, 244 ns per dword -- which is uncached PCIe speed,
 * against 725 MB/s measured on the same framebuffer when the machine is quiet.
 *
 * So the page tables and the stopwatch disagree, and one of them is wrong. core/gop.c already
 * carries the note that asserting WC is not measuring it; this module exists so the MTRR half
 * of the answer is a measurement too.
 *
 * Deliberately NOT included here: a table of "MTRR type X combined with PAT type Y gives Z".
 * That combining rule -- specifically whether PAT WC wins over an MTRR of UC -- is the exact
 * point in dispute, it is stated differently in the Intel and AMD manuals, and hardcoding my
 * belief about it would turn the thing being tested into an assumption. The caller reports the
 * MTRR type, the PAT entry and the measured throughput side by side, and the hardware says
 * which rule it implements.
 *
 * Pure: MSR values come in as parameters, so this is fully unit testable.
 */

/* Encodings from the MTRR/PAT type field; the numeric values are architectural. */
#define HYPE_MTRR_UC 0x00u
#define HYPE_MTRR_WC 0x01u
#define HYPE_MTRR_WT 0x04u
#define HYPE_MTRR_WP 0x05u
#define HYPE_MTRR_WB 0x06u
#define HYPE_MTRR_INVALID 0xFFu /* no MTRR covers it and MTRRs are disabled, or a reserved type */

#define HYPE_MTRR_MAX_VAR 16u /* IA32_MTRR_PHYSBASE0..15 */

typedef struct {
    uint64_t base; /* IA32_MTRR_PHYSBASEn: type in bits 7:0, base in 12:MAXPHYADDR-1 */
    uint64_t mask; /* IA32_MTRR_PHYSMASKn: V in bit 11, mask in 12:MAXPHYADDR-1 */
} hype_mtrr_var_t;

/* IA32_MTRR_DEF_TYPE bits. */
#define HYPE_MTRR_DEF_TYPE_MASK 0xFFu
#define HYPE_MTRR_DEF_E (1u << 11) /* MTRRs enabled at all */

/*
 * Resolves `addr` against the variable-range MTRRs, per the overlap rules: an unset valid
 * bit means the range is ignored; UC wins over every other type; WT wins over WB; no match
 * means the default type. Returns HYPE_MTRR_INVALID if MTRRs are disabled entirely (def_type
 * bit 11 clear), because then the architectural type is UC for a different reason and saying
 * "UC" would hide that.
 *
 * Fixed-range MTRRs are not modelled: they only cover the first 1 MB, and the framebuffer this
 * was written for sits at 0x4000000000. Passing an address below 1 MB therefore gives the
 * variable-range answer, which is not the whole truth for that region -- do not use it there.
 */
uint8_t hype_mtrr_type_for(uint64_t addr, const hype_mtrr_var_t *var, unsigned int count,
                           uint64_t def_type);

/* Short name for a type encoding ("UC", "WC", "WB", ...), or "??" for anything else.
 * Never returns 0, so it is safe to pass straight to a format string. */
const char *hype_mtrr_type_name(uint8_t type);

/*
 * Extracts PAT entry `index` (0..7) from an IA32_PAT MSR value. Returns HYPE_MTRR_INVALID for
 * an out-of-range index. hype's WC page tables select PA1 (PWT=1, PCD=0, PAT=0), so entry 1 is
 * the one that matters for the framebuffer.
 */
uint8_t hype_pat_entry(uint64_t pat, unsigned int index);

/*
 * This core's actual MSRs (arch/x86_64/cpu/mtrr_hw.c). Declared here so callers need only this
 * header. hype_mtrr_read_var() returns how many entries it filled, taken from IA32_MTRRCAP's
 * VCNT -- reading an unimplemented PHYSBASE is a #GP, which after ExitBootServices is a triple
 * fault rather than a diagnostic.
 */
uint64_t hype_mtrr_read_pat(void);
uint64_t hype_mtrr_read_def_type(void);
unsigned int hype_mtrr_read_var(hype_mtrr_var_t *out, unsigned int max);

/*
 * MSR_SMI_COUNT: how many System Management Interrupts this core has taken. An SMI storm is one
 * of the few things that can steal 100 ms from a straight store loop while every architectural
 * state stays identical, so it is worth ruling in or out directly rather than by elimination.
 *
 * INTEL ONLY. Reading this MSR on another vendor is a #GP, and after ExitBootServices that is a
 * triple fault, not a diagnostic. The caller must check the vendor -- hence "unchecked".
 */
uint64_t hype_smi_count_unchecked(void);

/* #368: delivered-vs-nominal frequency (see mtrr_hw.c). Both vendors. */
void hype_perf_read_amperf(uint64_t *aperf, uint64_t *mperf);

/* IA32_THERM_STATUS. INTEL ONLY -- same #GP hazard as the SMI counter. */
uint64_t hype_therm_status_unchecked(void);

/* CR0, for the cache-disable (bit 30) and not-write-through (bit 29) bits. */
uint64_t hype_read_cr0(void);

#endif /* HYPE_CORE_MTRR_H */
