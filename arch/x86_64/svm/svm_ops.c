#include "svm.h"

const hype_vmm_ops_t hype_svm_ops = {
    "SVM",
    hype_svm_enable,
    hype_svm_vcpu_create,
    hype_svm_vcpu_enable_apic_accel_ops,
    hype_svm_vcpu_run
};
