#ifndef HYPE_ARCH_HYPERV_H
#define HYPE_ARCH_HYPERV_H

#include <stdint.h>

#include "../../../core/guest_mem.h"

/* M7-1b (#300): guest-visible Hyper-V hypercall interface constants. */
#define HYPE_HV_HYPERCALL_PAGE_SIZE 4096u
#define HYPE_HV_HYPERCALL_ENABLE 0x1ull
#define HYPE_HV_HYPERCALL_LOCKED 0x2ull
#define HYPE_HV_HYPERCALL_GPA_MASK 0xFFFFFFFFFFFFF000ull
#define HYPE_HV_STATUS_INVALID_HYPERCALL_CODE 0x0002ull

typedef enum {
    HYPE_HV_HYPERCALL_VENDOR_VMX = 0,
    HYPE_HV_HYPERCALL_VENDOR_SVM = 1
} hype_hv_hypercall_vendor_t;

/*
 * Apply a guest write to HV_X64_MSR_HYPERCALL.
 *
 * The full 4 KiB page is translated through this VM's GPA map before any byte
 * is touched. A nonzero Guest OS ID is required to set Enable. A locked MSR is
 * immutable. On success, out_value is the value the guest will read back.
 * Returns -1 for an enabled page outside the VM's map; no guest memory or MSR
 * state is changed in that case.
 */
int hype_hv_hypercall_page_write(uint64_t current_value, uint64_t requested_value,
                                 uint64_t guest_os_id, const hype_gpa_map_t *map,
                                 hype_hv_hypercall_vendor_t vendor, uint64_t *out_value);

/* Clearing Guest OS ID disables an established page, as required by the TLFS. */
uint64_t hype_hv_hypercall_disable(uint64_t msr_value);

/* #436: HV_X64_MSR_REFERENCE_TSC (0x40000021). Windows' bootlib/kernel use the
 * reference TSC page as their clock under Hyper-V: ReferenceTime(100ns) =
 * ((tsc * TscScale) >> 64) + TscOffset. The guest writes the page GPA + enable
 * bit; the hypervisor fills {TscSequence, TscScale, TscOffset}. */
#define HYPE_HV_REF_TSC_ENABLE 0x1ull
#define HYPE_HV_REF_TSC_GPA_MASK 0xFFFFFFFFFFFFF000ull

/*
 * Apply a guest write to HV_X64_MSR_REFERENCE_TSC. On enable, fills the guest
 * page (translated through this VM's GPA map) with a valid sequence/scale/offset
 * for a guest TSC running at tsc_hz. Returns -1 for an enabled page outside the
 * map (caller injects #GP) -- no state changed.
 */
int hype_hv_reference_tsc_write(uint64_t requested_value, const hype_gpa_map_t *map,
                                uint64_t tsc_hz, uint64_t *out_value);

/*
 * Dispatch one x64 Hyper-V hypercall input value. No call codes are implemented
 * by #300, so every code receives the architectural well-formed failure status.
 */
uint64_t hype_hv_hypercall_dispatch(uint64_t input_value);

#endif /* HYPE_ARCH_HYPERV_H */
