#include "vmx.h"

/*
 * UNVALIDATED -- see vmx.h. vcpu_create/vcpu_enable_apic_accel/
 * vcpu_run stay NULL, deliberately, past M2-7 (unlike SVM's real
 * hype_svm_ops, which is fully wired up in this same commit): VMX's
 * VMLAUNCH/VMRESUME don't return control to "the next instruction"
 * the way SVM's VMRUN does -- on a VM-exit, the CPU jumps directly to
 * whatever HOST_RIP/HOST_RSP the VMCS holds (vmcs_hw.c currently
 * points these at a placeholder halt stub), which means a working
 * vcpu_run() needs its own hand-written VM-entry/VM-exit assembly
 * trampoline (saving/restoring host GPRs around the transition,
 * analogous to arch/x86_64/cpu/isr_stubs.S but for a completely
 * different control-transfer model) to actually return to a C caller
 * afterward. That's a fundamentally different kind of risk than the
 * struct-filling/VMWRITE-sequence code already written for this
 * backend -- it cannot be reasoned through against the SDM alone the
 * way vmcs_hw.c's field values could, it needs real VMX hardware to
 * develop and debug interactively. Writing it blind here, with zero
 * ability to even single-step it, would produce exactly the kind of
 * confidently-wrong code this project's rigor is meant to avoid.
 * Deferred to M2-8's real Intel hardware pass, where it can actually
 * be built and iterated against.
 */
const hype_vmm_ops_t hype_vmx_ops = {
    "VMX",
    hype_vmx_enable,
    0,
    0,
    0
};
