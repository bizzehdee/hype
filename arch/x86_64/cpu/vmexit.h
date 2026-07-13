#ifndef HYPE_ARCH_VMEXIT_H
#define HYPE_ARCH_VMEXIT_H

#include <stdint.h>

#include "cpu_features.h"
#include "vmm_ops.h"

/*
 * Vendor-agnostic VM-exit dispatch (M2-5). Same split as the rest of
 * this project: classifying/deciding what to do about an exit is pure
 * decision logic, tested here; actually running the vCPU is each
 * backend's own real VMRUN/VMLAUNCH, done in vcpu_run() (still
 * unimplemented pending M2-7, which is what wires a real
 * hype_vcpu_ctx_t and vcpu_run() into this loop for the hand-written
 * hlt-loop test guest).
 */

typedef enum {
    HYPE_VMEXIT_REASON_HLT,
    HYPE_VMEXIT_REASON_SHUTDOWN,
    HYPE_VMEXIT_REASON_OTHER,
    HYPE_VMEXIT_REASON_ENTRY_FAILED
} hype_vmexit_reason_t;

typedef enum {
    /* The M2-7 test guest's only expected exit -- its whole body is a
     * single HLT, so seeing one means the VM-exit round trip works. */
    HYPE_VMEXIT_ACTION_GUEST_HALTED,
    /* Anything else: no device model (MMIO/PIO emulation, exception
     * injection, ...) exists until M3+, so there is nothing else this
     * project can do with a VM-exit yet. */
    HYPE_VMEXIT_ACTION_FATAL
} hype_vmexit_action_t;

/*
 * Normalizes a backend's native exit reason (SVM's #VMEXIT code or
 * VMX's basic exit reason, both carried verbatim in
 * hype_vmexit_info_t.reason by vcpu_run()) into a vendor-agnostic
 * hype_vmexit_reason_t, given which backend produced it. VMX signals
 * VM-entry failure through vcpu_run()'s own return value (there's no
 * VMX exit-reason sentinel for it the way SVM has
 * HYPE_SVM_EXITCODE_INVALID), so HYPE_VMEXIT_REASON_ENTRY_FAILED only
 * ever comes from the SVM side of this classification. Pure lookup,
 * no CPU state touched.
 */
hype_vmexit_reason_t hype_vmexit_classify(hype_vmm_kind_t kind, uint64_t raw_reason);

/* Given a classified reason, decides what the dispatch loop does
 * next. Pure decision logic. */
hype_vmexit_action_t hype_vmexit_decide(hype_vmexit_reason_t reason);

/*
 * Runs `ctx` via ops->vcpu_run() and classifies the result: returns 0
 * if the guest halted cleanly (HYPE_VMEXIT_ACTION_GUEST_HALTED),
 * non-zero if VM-entry itself failed or the exit was anything else
 * (HYPE_VMEXIT_ACTION_FATAL -- no device model yet, so there's
 * nothing to resume the guest with). *out_info holds the exit info
 * either way. Doesn't yet loop resuming the guest on non-terminal
 * exits -- there are none to resume on until M3+ adds a device model,
 * and the M2-7 test guest's only exit (HLT) is deliberately terminal
 * (nothing productive to do by resuming a guest whose entire body is
 * one instruction). The name anticipates that extension rather than
 * describing a real loop yet.
 */
int hype_vmexit_dispatch_loop(const hype_vmm_ops_t *ops, hype_vcpu_ctx_t *ctx, hype_vmm_kind_t kind,
                               hype_vmexit_info_t *out_info);

#endif /* HYPE_ARCH_VMEXIT_H */
