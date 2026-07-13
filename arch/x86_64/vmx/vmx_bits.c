#include "vmx.h"

int hype_vmx_feature_control_allows_vmxon(uint64_t feature_control) {
    int locked = (feature_control & HYPE_FEATURE_CONTROL_LOCK) != 0;
    int enabled_outside_smx = (feature_control & HYPE_FEATURE_CONTROL_VMX_OUTSIDE_SMX) != 0;

    if (!locked) {
        return 1;
    }
    return enabled_outside_smx;
}

uint64_t hype_vmx_feature_control_with_vmxon_enabled(uint64_t feature_control) {
    return feature_control | HYPE_FEATURE_CONTROL_LOCK | HYPE_FEATURE_CONTROL_VMX_OUTSIDE_SMX;
}

uint64_t hype_vmx_cr4_with_vmxe(uint64_t old_cr4) {
    return old_cr4 | HYPE_CR4_VMXE;
}
