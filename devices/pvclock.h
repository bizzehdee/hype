#ifndef HYPE_DEVICES_PVCLOCK_H
#define HYPE_DEVICES_PVCLOCK_H

#include <stdint.h>
#include "../core/guest_mem.h" /* hype_gpa_map_t, for hype_pvclock_arm_*'s VALID-3 checks */

/*
 * kvmclock (KVM paravirtual clock) -- the guest-facing side.
 *
 * Why: an AMD guest can't calibrate its TSC against hype's emulated PIT (the
 * PIT advances in VM-exit + host-tick-sized lumps, so Linux's
 * quick_pit_calibrate skips the MSB values it polls for -> "could not
 * calculate TSC khz" -> TSC marked unstable, observed on real HW). kvmclock
 * sidesteps calibration entirely: hype publishes the TSC->nanoseconds scaling
 * in a guest-shared page, and the guest computes time = system_time +
 * scale(rdtsc - tsc_timestamp). It is keyed on the KVM CPUID signature, NOT
 * the CPU vendor, so it works identically for AMD and Intel guests (Linux and
 * *BSD; Windows uses Hyper-V enlightenments instead).
 *
 * This header carries the KVM ABI (struct layout, MSR numbers, CPUID feature
 * bits) plus the pure logic -- the TSC->ns scale computation and the
 * version-protocol page fill -- which is fully unit-tested. The glue that
 * traps the KVM MSRs and locates the guest page lives in the exempt SVM path.
 */

/* KVM paravirt MSRs (arch/x86/include/uapi/asm/kvm_para.h). The "_NEW"
 * variants pair with KVM_FEATURE_CLOCKSOURCE2; the guest writes a
 * guest-physical address | 1 (enable) to SYSTEM_TIME to arm the pvclock. */
#define HYPE_MSR_KVM_WALL_CLOCK_NEW 0x4b564d00u
#define HYPE_MSR_KVM_SYSTEM_TIME_NEW 0x4b564d01u
#define HYPE_MSR_KVM_WALL_CLOCK_OLD 0x11u
#define HYPE_MSR_KVM_SYSTEM_TIME_OLD 0x12u
#define HYPE_KVM_SYSTEM_TIME_ENABLE 0x1u /* bit 0 of the SYSTEM_TIME write */
#define HYPE_KVM_MSR_ADDR_MASK 0xFFFFFFFFFFFFFFFCULL /* clear low 2 bits -> GPA */

/* KVM CPUID leaves. Signature leaf 0x40000000 EBX/ECX/EDX = "KVMKVMKVM\0\0\0".
 * Feature leaf 0x40000001 EAX carries the KVM_FEATURE_* bits. */
#define HYPE_KVM_CPUID_SIGNATURE 0x40000000u
#define HYPE_KVM_CPUID_FEATURES 0x40000001u
#define HYPE_KVM_CPUID_TSC_KHZ 0x40000010u /* EAX = tsc_khz, EBX = bus khz */
#define HYPE_KVM_SIGNATURE_EBX 0x4b4d564bu  /* "KVMK" */
#define HYPE_KVM_SIGNATURE_ECX 0x564b4d56u  /* "VMKV" */
#define HYPE_KVM_SIGNATURE_EDX 0x0000004du  /* "M\0\0\0" */
#define HYPE_KVM_FEATURE_CLOCKSOURCE (1u << 0)
#define HYPE_KVM_FEATURE_CLOCKSOURCE2 (1u << 3)
#define HYPE_KVM_FEATURE_CLOCKSOURCE_STABLE_BIT (1u << 24)

/* Per-vCPU pvclock page (KVM ABI, packed to the exact on-wire layout the
 * guest reads). An odd `version` means "update in progress" -- the guest
 * re-reads until it sees a stable even value. */
struct hype_pvclock_vcpu_time_info {
    uint32_t version;
    uint32_t pad0;
    uint64_t tsc_timestamp;
    uint64_t system_time;
    uint32_t tsc_to_system_mul;
    int8_t tsc_shift;
    uint8_t flags;
    uint8_t pad[2];
} __attribute__((packed));

#define HYPE_PVCLOCK_TSC_STABLE_BIT 0x01u

struct hype_pvclock_wall_clock {
    uint32_t version;
    uint32_t sec;
    uint32_t nsec;
} __attribute__((packed));

/*
 * Computes the (multiplier, shift) that convert a guest TSC delta to
 * nanoseconds under the pvclock rule:
 *   if (shift < 0) delta >>= -shift; else delta <<= shift;
 *   ns = (delta * mul) >> 32;
 * i.e. ns ~= tsc_delta * 1e9 / tsc_hz. This is the standard KVM/Xen
 * kvm_get_time_scale(NSEC_PER_SEC, tsc_hz) algorithm. tsc_hz must be nonzero.
 */
void hype_pvclock_calc_scale(uint64_t tsc_hz, uint32_t *out_mul, int8_t *out_shift);

/* Applies the pvclock scale to a TSC delta exactly as the guest kernel does
 * -- provided so tests can round-trip calc_scale against known frequencies. */
uint64_t hype_pvclock_scale_delta(uint64_t tsc_delta, uint32_t mul, int8_t shift);

/* Fills the per-vCPU time-info page using the version handshake (bump to odd,
 * write fields, bump to even) so a guest reading concurrently never sees a
 * torn update. */
void hype_pvclock_write_time_info(volatile struct hype_pvclock_vcpu_time_info *ti,
                                   uint64_t tsc_timestamp, uint64_t system_time_ns, uint32_t mul,
                                   int8_t shift, uint8_t flags);

/* Fills the wall-clock page (boot wall time) with the same version handshake. */
void hype_pvclock_write_wall_clock(volatile struct hype_pvclock_wall_clock *wc, uint32_t sec,
                                    uint32_t nsec);

/*
 * Count of pvclock time-info page fills; nonzero proves a guest actually armed
 * kvmclock. Declared here rather than in svm.h (its historical home) because BOTH
 * backends arm pvclock now -- leaving it SVM-owned meant the VMX path could not
 * increment it, so the "PVCLOCK arm_count" diagnostic would read 0 on Intel even
 * once kvmclock worked. Still DEFINED in svm_vcpu.c; moving the definition is a
 * tidy-up for whenever pvclock gets its own translation unit.
 */
extern volatile uint32_t g_hype_pvclock_arm_count;

/*
 * #667: the whole MSR_KVM_SYSTEM_TIME arm sequence, extracted from the SVM/VMX MSR-write handlers
 * (arch/x86_64/svm/svm_vcpu.c, arch/x86_64/vmx/vmcs_hw.c -- both call this instead of duplicating
 * it) so the VALID-3 GPA-rejection path is independently unit-testable without a full vcpu
 * context, mirroring hype_hv_reference_tsc_write's shape (arch/x86_64/cpu/hyperv.c). `tsc_now`,
 * `mul` and `shift` are the caller's already-known TSC reading and scale (each backend keeps its
 * own file-scope mul/shift state) rather than globals this module would have to own.
 *
 * Returns 1 if the page was armed, 0 if the write is a no-op by the KVM ABI's own rules (ENABLE
 * bit clear, or no pvclock map registered yet -- neither is an error), or -1 if the requested GPA
 * could not be translated (the page is left untouched). The call sites do not need to inspect
 * this -- a WRMSR always "succeeds" from the guest's perspective either way -- it exists so a
 * test can distinguish the three outcomes.
 */
int hype_pvclock_arm_system_time(uint64_t msr_value, const hype_gpa_map_t *map, uint64_t tsc_now,
                                 uint32_t mul, int8_t shift);

/* Same contract as hype_pvclock_arm_system_time, for MSR_KVM_WALL_CLOCK -- no ENABLE bit to gate
 * on, so the only no-op case is "no pvclock map registered yet". */
int hype_pvclock_arm_wall_clock(uint64_t msr_value, const hype_gpa_map_t *map);

#endif /* HYPE_DEVICES_PVCLOCK_H */
