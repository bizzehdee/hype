#include "vmcs.h"
#include "vmx.h"

/*
 * M2-8 (VMX-1): vcpu_create/vcpu_run are now wired up. The hand-written
 * VM-entry/exit trampoline they needed -- VMX's VMLAUNCH/VMRESUME jump to
 * HOST_RIP on a VM-exit rather than returning to the next instruction the way
 * SVM's VMRUN does -- lives in vmx_run.S (hype_vmx_launch), built and iterated
 * against real Intel VMX hardware as this backend always required.
 * vcpu_enable_apic_accel stays NULL: the test guests don't use APICv (the
 * APICv secondary controls were dropped from the launchable VMCS build).
 */
const hype_vmm_ops_t hype_vmx_ops = {
    .name = "VMX",
    .enable = hype_vmx_enable,
    .enable_on = hype_vmx_enable_on, /* #242: per-core, for the AP landing */
    .vcpu_create = hype_vmx_vcpu_create,
    /* .vcpu_enable_apic_accel deliberately unset -- see above. */
    .vcpu_run = hype_vmx_vcpu_run
};
