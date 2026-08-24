#include "svm.h"

/* Designated initializers, not positional: a new hype_vmm_ops_t field silently
 * shifts every later entry by one, which is a whole class of bug that costs a
 * hardware boot to find (adding enable_on for #242 did exactly that to the mock
 * table in core/tests/test_vmexit.c). Naming the fields also documents the
 * intentional gaps rather than leaving a bare 0 to be counted by hand. */
const hype_vmm_ops_t hype_svm_ops = {
    .name = "SVM",
    .enable = hype_svm_enable,
    .enable_on = hype_svm_enable_on_page, /* #242: per-core, for the AP landing */
    .vcpu_create = hype_svm_vcpu_create,
    /* .vcpu_enable_apic_accel deliberately unset, same reasoning as vmx_ops.c's own copy of
     * this comment: nothing dispatches AVIC/APICv enablement through hype_vmm_ops_t -- boot/
     * main.c's fw_1_maybe_enable_avic() calls hype_svm_vcpu_enable_apic_accel_ops() directly,
     * because it needs vm_idx/guest_apic_id/max_physical_id (#640 cause 2) that this table's
     * fixed (hype_vcpu_ctx_t *) signature has no room for. */
    .vcpu_run = hype_svm_vcpu_run,
    .vcpu_tlb_tag = hype_svm_vcpu_tlb_tag /* #244: ASID */
};
