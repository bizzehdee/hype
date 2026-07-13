#include "vmx.h"

/* UNVALIDATED -- see vmx.h. vcpu_create/vcpu_enable_apic_accel/
 * vcpu_run are filled in by M2-3/M2-4/M2-5 as they land; NULL until
 * then is intentional. */
const hype_vmm_ops_t hype_vmx_ops = {
    "VMX",
    hype_vmx_enable,
    0,
    0,
    0
};
