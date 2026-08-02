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

uint64_t hype_vmx_cr_with_fixed_bits(uint64_t cr, uint64_t fixed0, uint64_t fixed1) {
    return (cr | fixed0) & fixed1;
}

uint16_t hype_vmx_vpid_for_slot(unsigned slot, unsigned max_slots) {
    if (max_slots == 0u) {
        return 1u;
    }
    if (slot >= max_slots) {
        slot = max_slots - 1u;
    }
    return (uint16_t)(slot + 1u);
}

int hype_vmx_vpid_usable(uint64_t ept_vpid_cap) {
    int have_invvpid = (ept_vpid_cap & HYPE_VMX_EPT_VPID_CAP_INVVPID) != 0;
    int have_single_context = (ept_vpid_cap & HYPE_VMX_EPT_VPID_CAP_INVVPID_SINGLE) != 0;

    return (have_invvpid && have_single_context) ? 1 : 0;
}

uint32_t hype_vmx_adjust_controls(uint32_t desired, uint64_t capability_msr) {
    uint32_t allowed0 = (uint32_t)(capability_msr & 0xFFFFFFFFu);
    uint32_t allowed1 = (uint32_t)(capability_msr >> 32);

    return (desired | allowed0) & allowed1;
}
