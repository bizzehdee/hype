#ifndef HYPE_DUMP_FMT_H
#define HYPE_DUMP_FMT_H

#include <stdint.h>
#include "dashboard.h"

/*
 * #611: the `dump <vm>` terminal command's formatter.
 *
 * plan.md decision 43 forbids an observer from touching a live VMCS/VMCB: the owning core is the
 * only one allowed to make a vCPU's state current, and everyone else reads the snapshot that
 * owner publishes at each exit. `dump` runs on the BSP (the terminal's own core), never the AP
 * that owns the VM being inspected, so every field in hype_dump_snapshot_t below is something the
 * caller already fetched through that published-snapshot path (hype_{vmx,svm}_vcpu_get_gpr,
 * _get_debug_state, _get_intr_state -- see boot/main.c's term_dump_cmd) -- this header and .c file
 * hold no VM/ctx access of their own, which is what makes the formatter host-testable.
 *
 * GPR encoding order matches the vCPU ctx contract documented in arch/x86_64/vmx/vmcs_hw.c and
 * arch/x86_64/svm/svm_vcpu.c: 0=RAX,1=RCX,2=RDX,3=RBX,4=RSP,5=RBP,6=RSI,7=RDI,8-15=R8..R15.
 */
#define HYPE_DUMP_MAX_VCPUS 8u

typedef struct {
    int present; /* 0 => this vCPU was never dispatched; every other field here is 0 */
    uint64_t gprs[16];
    uint64_t rip;
    uint64_t cr3;
    /* Pending/injected event state -- hype_vmm_intr_state_t's own fields (arch/x86_64/cpu/vmm_ops.h),
     * reused verbatim rather than re-modeled. */
    int can_accept;
    uint64_t eventinj;
    int vintr_armed;
    int pending_valid;
    unsigned pending_count;
    unsigned pending_vector;
} hype_dump_vcpu_t;

typedef struct {
    const char *vm_name;
    const char *lifecycle; /* hype_vm_lifecycle_name()'s own string */
    unsigned n_vcpus;      /* <= HYPE_DUMP_MAX_VCPUS */
    hype_dump_vcpu_t vcpu[HYPE_DUMP_MAX_VCPUS];
    /* Exit-reason history -- the same bucket set as the FW-1 EXHIST diagnostic line, so the two
     * never drift into different classifications of the same exits. Attributed to vcpu[0]'s own
     * run loop, the only one hype currently accumulates these against (see boot/main.c's
     * fw_1_publish_and_render()). */
    unsigned long long ex_total;
    unsigned long long ex_hlt;
    unsigned long long ex_npf;
    unsigned long long ex_ioio;
    unsigned long long ex_msr;
    unsigned long long ex_cpuid;
    unsigned long long ex_vintr;
    unsigned long long ex_pause;
    unsigned long long ex_intr;
    unsigned long long ex_other;
} hype_dump_snapshot_t;

/* Formats `snap` into `out` (reset first, then appended to -- see hype_dash_text_add for the
 * overflow convention). Pure function: no VM/ctx/hardware access, so it is exercised directly by
 * core/tests/test_dump_fmt.c on the host. */
void hype_dump_format(const hype_dump_snapshot_t *snap, hype_dash_text_t *out);

#endif /* HYPE_DUMP_FMT_H */
