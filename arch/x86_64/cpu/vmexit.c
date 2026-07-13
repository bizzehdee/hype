#include "vmexit.h"

#include "../svm/vmcb.h"
#include "../vmx/vmcs_fields.h"

hype_vmexit_reason_t hype_vmexit_classify(hype_vmm_kind_t kind, uint64_t raw_reason) {
    if (kind == HYPE_VMM_KIND_SVM) {
        if (raw_reason == HYPE_SVM_EXITCODE_HLT) {
            return HYPE_VMEXIT_REASON_HLT;
        }
        if (raw_reason == HYPE_SVM_EXITCODE_SHUTDOWN) {
            return HYPE_VMEXIT_REASON_SHUTDOWN;
        }
        if (raw_reason == HYPE_SVM_EXITCODE_INVALID) {
            return HYPE_VMEXIT_REASON_ENTRY_FAILED;
        }
        return HYPE_VMEXIT_REASON_OTHER;
    }

    if (kind == HYPE_VMM_KIND_VMX) {
        if (raw_reason == HYPE_VMX_EXIT_REASON_HLT) {
            return HYPE_VMEXIT_REASON_HLT;
        }
        if (raw_reason == HYPE_VMX_EXIT_REASON_TRIPLE_FAULT) {
            return HYPE_VMEXIT_REASON_SHUTDOWN;
        }
        return HYPE_VMEXIT_REASON_OTHER;
    }

    return HYPE_VMEXIT_REASON_OTHER;
}

hype_vmexit_action_t hype_vmexit_decide(hype_vmexit_reason_t reason) {
    if (reason == HYPE_VMEXIT_REASON_HLT) {
        return HYPE_VMEXIT_ACTION_GUEST_HALTED;
    }
    return HYPE_VMEXIT_ACTION_FATAL;
}

int hype_vmexit_dispatch_loop(const hype_vmm_ops_t *ops, hype_vcpu_ctx_t *ctx, hype_vmm_kind_t kind,
                               hype_vmexit_info_t *out_info) {
    if (ops->vcpu_run(ctx, out_info) != 0) {
        return -1;
    }

    hype_vmexit_action_t action = hype_vmexit_decide(hype_vmexit_classify(kind, out_info->reason));
    return (action == HYPE_VMEXIT_ACTION_GUEST_HALTED) ? 0 : -1;
}
