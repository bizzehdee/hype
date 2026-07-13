#include "svm.h"

/*
 * vcpu_create/vcpu_enable_apic_accel/vcpu_run are filled in by M2-3/
 * M2-4/M2-5 as they land; NULL until then is intentional, not an
 * oversight -- nothing calls through them yet.
 */
const hype_vmm_ops_t hype_svm_ops = {
    "SVM",
    hype_svm_enable,
    0,
    0,
    0
};
