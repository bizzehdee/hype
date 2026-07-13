#ifndef HYPE_ARCH_VMM_OPS_H
#define HYPE_ARCH_VMM_OPS_H

#include <stdint.h>

#include "cpu_features.h"

/*
 * Vendor-agnostic VM-exit info, filled in by whichever backend actually
 * ran the vCPU. `reason` and `qualification` carry each backend's own
 * native exit-reason encoding for now (VMX's 32-bit exit reason vs
 * SVM's #VMEXIT code are different numbering spaces) -- M2-5's dispatch
 * loop is what gives them a shared meaning; M2 alone doesn't need more
 * than "did we get an exit, and does the hlt-loop guest's known exit
 * reason show up."
 */
typedef struct {
    uint64_t reason;
    uint64_t qualification;
    uint64_t guest_rip;
} hype_vmexit_info_t;

/* Opaque per-vCPU context. Each backend (arch/x86_64/vmx,
 * arch/x86_64/svm) defines its own concrete struct; the VM-exit
 * dispatch loop (M2-5) and device model only ever see this pointer,
 * which is what keeps them vendor-agnostic (plan.md §4). */
typedef struct hype_vcpu_ctx hype_vcpu_ctx_t;

typedef struct {
    const char *name;

    /* Enables VMX/SVM operation on the calling physical CPU (VMXON /
     * setting EFER.SVME). Returns 0 on success, non-zero on failure. */
    int (*enable)(void);

    /*
     * Allocates and minimally initializes a vCPU context: the guest
     * starts executing at guest_rip with guest_rsp, using
     * ept_or_npt_root as its EPT/NPT table root (physical address; 0 is
     * a valid "not set up yet" value pre-M3). Returns an opaque
     * context, or 0 (NULL) on failure.
     */
    hype_vcpu_ctx_t *(*vcpu_create)(uint64_t guest_rip, uint64_t guest_rsp,
                                     uint64_t ept_or_npt_root);

    /* Enables APICv (Intel)/AVIC (AMD) for this vCPU context (M2-4). */
    void (*vcpu_enable_apic_accel)(hype_vcpu_ctx_t *ctx);

    /*
     * Runs the vCPU until the next VM-exit, filling *info. Returns 0 on
     * a normal VM-exit; non-zero if VM-entry itself failed (a VMCS/VMCB
     * misconfiguration bug, not a guest action -- VMLAUNCH/VMRUN report
     * this distinctly from a real exit).
     */
    int (*vcpu_run)(hype_vcpu_ctx_t *ctx, hype_vmexit_info_t *info);
} hype_vmm_ops_t;

#endif /* HYPE_ARCH_VMM_OPS_H */
