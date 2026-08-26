#ifndef HYPE_CORE_GUEST_MTRR_H
#define HYPE_CORE_GUEST_MTRR_H

#include <stdint.h>

/*
 * #436 / #729: the guest's MTRR set, modelled as pure round-trip storage.
 *
 * These MTRRs are COSMETIC to hype's own memory typing -- guest RAM is typed WB through
 * NPT/EPT and PAT, and nothing here changes that. They exist so a guest reads back what it
 * wrote, because the alternative is not "no MTRRs" but an inconsistent pair: MTRRcap
 * advertises variable MTRRs, and a stub that ignores writes and returns 0 makes OVMF's
 * MtrrLib -- invoked by Windows winload's SetMemoryAttributes; Linux and BSD never call it --
 * loop forever in MtrrLibSetMemoryAttributesWorker waiting for a verify that cannot converge.
 *
 * #436 fixed that on SVM. #729 found VMX still carrying the pre-#436 stub, which is why this
 * is one shared model rather than a second copy: the failure it prevents took a real Windows
 * boot to find the first time, and a divergence between the two backends would only ever be
 * found the same expensive way.
 */

/* 8 variable base/mask pairs (0x200-0x20F) + 11 fixed MTRRs + IA32_MTRR_DEF_TYPE. */
typedef struct {
    uint64_t deftype;  /* IA32_MTRR_DEF_TYPE (0x2FF) */
    uint64_t var[16];  /* 0x200..0x20F, base/mask interleaved as the MSR numbers are */
    uint64_t fix[11];  /* 0x250, 0x258, 0x259, 0x268..0x26F */
} hype_guest_mtrr_t;

/*
 * IA32_MTRRCAP (0xFE), read-only: VCNT=8 [7:0], FIX=1 [8], WC=1 [10]. It must agree with the
 * array sizes above -- an advertised capability with no storage behind it is the exact bug
 * this model exists to prevent.
 */
#define HYPE_GUEST_MTRRCAP 0x0508u

/* True for MTRRcap, MTRRdefType, the 8 variable pairs and the fixed MTRRs. IA32_PAT (0x277)
 * is NOT one of these -- both backends hold PAT in their own vendor field (VMCB save.g_pat,
 * VMCS GUEST_IA32_PAT), because the CPU loads it on entry. */
int hype_guest_mtrr_is_msr(uint32_t msr);

/*
 * The power-on state hype presents, which is NOT all-zero and must not become so (#481):
 *
 *   deftype = 0x806 -- E (bit 11) set, default type WB (6), FE (bit 10) CLEAR.
 *   fix[]   = all 0x06 bytes (WB).
 *   var[]   = 0 (disabled; with the default already WB, none are needed).
 *
 * Starting from 0 means "MTRRs disabled, default type UC", i.e. ALL memory is uncached.
 * FreeBSD reads the MTRRs, believed it, and panicked/reset. FE was set here once and a DEBUG
 * OVMF rejected it outright -- "ASSERT MemDetect.c: (MtrrSettings.MtrrDefType & 0x400) == 0"
 * -- halting guest firmware in PEI; RELEASE builds compile that assert out, so it stayed
 * invisible until a DEBUG firmware was booted. Clearing FE does not change the effective
 * type: with the fixed ranges not consulted, the low 1 MiB falls through to the WB default.
 */
void hype_guest_mtrr_reset(hype_guest_mtrr_t *m);

/* Store a guest WRMSR. A write to MTRRcap is silently dropped: it is read-only, and real
 * hardware raises #GP, which hype does not model for MSRs. Any other MSR is ignored. */
void hype_guest_mtrr_write(hype_guest_mtrr_t *m, uint32_t msr, uint64_t value);

/* The value a guest RDMSR must see. MTRRcap returns the fixed capability above; anything not
 * modelled returns 0. */
uint64_t hype_guest_mtrr_read(const hype_guest_mtrr_t *m, uint32_t msr);

#endif /* HYPE_CORE_GUEST_MTRR_H */
