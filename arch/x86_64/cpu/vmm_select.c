#include "vmm_select.h"
#include "../svm/svm.h"
#include "../vmx/vmx.h"

const hype_vmm_ops_t *hype_vmm_ops_for_kind(hype_vmm_kind_t kind) {
    switch (kind) {
    case HYPE_VMM_KIND_VMX: return &hype_vmx_ops;
    case HYPE_VMM_KIND_SVM: return &hype_svm_ops;
    case HYPE_VMM_KIND_NONE:
    default: return 0;
    }
}
