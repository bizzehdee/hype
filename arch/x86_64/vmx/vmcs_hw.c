#include "vmcs.h"
#include "vmx.h"

/* UNVALIDATED -- see vmx.h and vmcs.h. */

static uint8_t g_vmcs_region[4096] __attribute__((aligned(4096)));
static uint8_t g_virtual_apic_page[4096] __attribute__((aligned(4096)));

static void hype_vmx_host_exit_stub(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t read_cr0(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline uint64_t read_cr4(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline uint16_t read_cs(void) {
    uint16_t v;
    __asm__ volatile("mov %%cs, %0" : "=r"(v));
    return v;
}

static inline uint16_t read_ss(void) {
    uint16_t v;
    __asm__ volatile("mov %%ss, %0" : "=r"(v));
    return v;
}

static inline uint16_t read_ds(void) {
    uint16_t v;
    __asm__ volatile("mov %%ds, %0" : "=r"(v));
    return v;
}

static inline uint16_t read_es(void) {
    uint16_t v;
    __asm__ volatile("mov %%es, %0" : "=r"(v));
    return v;
}

static inline uint16_t read_fs(void) {
    uint16_t v;
    __asm__ volatile("mov %%fs, %0" : "=r"(v));
    return v;
}

static inline uint16_t read_gs(void) {
    uint16_t v;
    __asm__ volatile("mov %%gs, %0" : "=r"(v));
    return v;
}

static inline uint16_t read_tr(void) {
    uint16_t v;
    __asm__ volatile("str %0" : "=r"(v));
    return v;
}

struct descriptor_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static inline uint64_t read_gdtr_base(void) {
    struct descriptor_ptr dp;
    __asm__ volatile("sgdt %0" : "=m"(dp));
    return dp.base;
}

static inline uint64_t read_idtr_base(void) {
    struct descriptor_ptr dp;
    __asm__ volatile("sidt %0" : "=m"(dp));
    return dp.base;
}

/*
 * VMWRITE's AT&T operand order, reasoned (not execution-verified -- see
 * vmcs.h's header comment): Intel's documented mnemonic is
 * "VMWRITE r/m64, r64" where the SDM's prose says the field encoding
 * (first, dest-position operand) identifies which VMCS field to write,
 * and the value (second, src-position operand) is what gets written
 * into it. AT&T syntax reverses Intel's operand order, so the AT&T form
 * is "vmwrite value, field" -- value first, field second. That's what's
 * implemented below: "r"(value) bound to %0, "r"(field) bound to %1,
 * template "vmwrite %0, %1".
 */
static inline int vmwrite(uint64_t field, uint64_t value) {
    uint8_t fail_zf, fail_cf;
    __asm__ volatile("vmwrite %2, %3\n\t"
                      "setz %0\n\t"
                      "setc %1"
                      : "=q"(fail_zf), "=q"(fail_cf)
                      : "r"(value), "r"(field)
                      : "cc");
    return (fail_zf || fail_cf) ? -1 : 0;
}

static inline int vmclear(const void *vmcs_phys_addr) {
    uint8_t fail_zf, fail_cf;
    __asm__ volatile("vmclear %1\n\t"
                      "setz %0\n\t"
                      "setc %2"
                      : "=q"(fail_zf), "=m"(vmcs_phys_addr), "=q"(fail_cf)
                      :
                      : "cc");
    (void)fail_cf;
    return fail_zf ? -1 : 0;
}

static inline int vmptrld(const void *vmcs_phys_addr_ptr) {
    uint8_t fail_zf, fail_cf;
    __asm__ volatile("vmptrld %1\n\t"
                      "setz %0\n\t"
                      "setc %2"
                      : "=q"(fail_zf), "=m"(vmcs_phys_addr_ptr), "=q"(fail_cf)
                      :
                      : "cc");
    (void)fail_cf;
    return fail_zf ? -1 : 0;
}

static int write_realmode_guest_segment(uint32_t selector_field, uint32_t base_field,
                                         uint32_t limit_field, uint32_t ar_field,
                                         uint16_t selector, uint8_t code) {
    int rc = 0;
    /* Real-mode-style segment: base = selector*16, 64KB limit, byte
     * granularity. AR byte layout matches the SVM side's convention
     * (accessed/writable/executable bits + present + non-system +
     * usable), packed the way Intel's VMCS guest-segment AR field
     * expects (bits 0-7 = access rights byte, bit 16 = unusable). */
    uint32_t ar = code ? 0x9Bu : 0x93u;
    rc |= vmwrite(selector_field, selector);
    rc |= vmwrite(base_field, (uint64_t)selector * 16u);
    rc |= vmwrite(limit_field, 0xFFFFu);
    rc |= vmwrite(ar_field, ar);
    return rc;
}

int hype_vmx_vmcs_build_realmode_guest(uint16_t entry_seg, uint64_t stack_phys) {
    int rc = 0;

    for (unsigned i = 0; i < sizeof(g_vmcs_region); i++) {
        g_vmcs_region[i] = 0;
    }

    uint64_t vmx_basic = rdmsr(HYPE_MSR_IA32_VMX_BASIC);
    uint32_t revision_id = (uint32_t)(vmx_basic & 0x7FFFFFFFu);
    *(uint32_t *)g_vmcs_region = revision_id;

    if (vmclear(&g_vmcs_region) != 0) {
        return -1;
    }
    if (vmptrld(&g_vmcs_region) != 0) {
        return -1;
    }

    int have_true_ctls = (vmx_basic & HYPE_VMX_BASIC_HAS_TRUE_CTLS) != 0;

    uint32_t pin_cap = rdmsr(have_true_ctls ? HYPE_MSR_IA32_VMX_TRUE_PINBASED_CTLS
                                             : HYPE_MSR_IA32_VMX_PINBASED_CTLS);
    uint32_t proc_cap = rdmsr(have_true_ctls ? HYPE_MSR_IA32_VMX_TRUE_PROCBASED_CTLS
                                              : HYPE_MSR_IA32_VMX_PROCBASED_CTLS);
    uint32_t proc2_cap = rdmsr(HYPE_MSR_IA32_VMX_PROCBASED_CTLS2);
    uint32_t exit_cap = rdmsr(have_true_ctls ? HYPE_MSR_IA32_VMX_TRUE_EXIT_CTLS
                                              : HYPE_MSR_IA32_VMX_EXIT_CTLS);
    uint32_t entry_cap = rdmsr(have_true_ctls ? HYPE_MSR_IA32_VMX_TRUE_ENTRY_CTLS
                                               : HYPE_MSR_IA32_VMX_ENTRY_CTLS);

    uint32_t pin_ctls = hype_vmx_adjust_controls(0, pin_cap);
    /* TPR shadow (M2-4) needs neither EPT nor "virtualize APIC
     * accesses" -- see vmcs_fields.h's comment on why it and the
     * secondary APICv bits below are safe to enable ahead of M3's
     * EPT. */
    uint32_t proc_ctls = hype_vmx_adjust_controls(
        HYPE_VMX_PROCBASED_ACTIVATE_SECONDARY_CONTROLS | HYPE_VMX_PROCBASED_USE_TPR_SHADOW,
        proc_cap);
    uint32_t proc2_ctls = hype_vmx_adjust_controls(
        HYPE_VMX_PROCBASED2_UNRESTRICTED_GUEST | HYPE_VMX_PROCBASED2_APIC_REGISTER_VIRT |
            HYPE_VMX_PROCBASED2_VIRTUAL_INTERRUPT_DELIVERY,
        proc2_cap);
    uint32_t exit_ctls = hype_vmx_adjust_controls(0, exit_cap);
    uint32_t entry_ctls = hype_vmx_adjust_controls(0, entry_cap);

    rc |= vmwrite(HYPE_VMCS_PIN_BASED_VM_EXEC_CONTROL, pin_ctls);
    rc |= vmwrite(HYPE_VMCS_CPU_BASED_VM_EXEC_CONTROL, proc_ctls);
    rc |= vmwrite(HYPE_VMCS_SECONDARY_VM_EXEC_CONTROL, proc2_ctls);
    rc |= vmwrite(HYPE_VMCS_VM_EXIT_CONTROLS, exit_ctls);
    rc |= vmwrite(HYPE_VMCS_VM_ENTRY_CONTROLS, entry_ctls);
    rc |= vmwrite(HYPE_VMCS_EXCEPTION_BITMAP, 0);

    /* TPR shadow/APICv (M2-4): only takes effect if the capability
     * negotiation above actually granted USE_TPR_SHADOW (older CPUs
     * without it will simply ignore VIRTUAL_APIC_PAGE_ADDR/
     * TPR_THRESHOLD). 0 threshold = no TPR-masking VM-exits. */
    rc |= vmwrite(HYPE_VMCS_VIRTUAL_APIC_PAGE_ADDR, (uint64_t)(uintptr_t)g_virtual_apic_page);
    rc |= vmwrite(HYPE_VMCS_TPR_THRESHOLD, 0);

    /* Guest state: flat real-mode-like guest at entry_seg:0, matching
     * hype_vmcb_build_realmode_guest()'s convention on the SVM side. */
    rc |= write_realmode_guest_segment(HYPE_VMCS_GUEST_CS_SELECTOR, HYPE_VMCS_GUEST_CS_BASE,
                                        HYPE_VMCS_GUEST_CS_LIMIT, HYPE_VMCS_GUEST_CS_AR_BYTES,
                                        entry_seg, 1);
    rc |= write_realmode_guest_segment(HYPE_VMCS_GUEST_DS_SELECTOR, HYPE_VMCS_GUEST_DS_BASE,
                                        HYPE_VMCS_GUEST_DS_LIMIT, HYPE_VMCS_GUEST_DS_AR_BYTES, 0, 0);
    rc |= write_realmode_guest_segment(HYPE_VMCS_GUEST_ES_SELECTOR, HYPE_VMCS_GUEST_ES_BASE,
                                        HYPE_VMCS_GUEST_ES_LIMIT, HYPE_VMCS_GUEST_ES_AR_BYTES, 0, 0);
    rc |= write_realmode_guest_segment(HYPE_VMCS_GUEST_SS_SELECTOR, HYPE_VMCS_GUEST_SS_BASE,
                                        HYPE_VMCS_GUEST_SS_LIMIT, HYPE_VMCS_GUEST_SS_AR_BYTES, 0, 0);
    rc |= write_realmode_guest_segment(HYPE_VMCS_GUEST_FS_SELECTOR, HYPE_VMCS_GUEST_FS_BASE,
                                        HYPE_VMCS_GUEST_FS_LIMIT, HYPE_VMCS_GUEST_FS_AR_BYTES, 0, 0);
    rc |= write_realmode_guest_segment(HYPE_VMCS_GUEST_GS_SELECTOR, HYPE_VMCS_GUEST_GS_BASE,
                                        HYPE_VMCS_GUEST_GS_LIMIT, HYPE_VMCS_GUEST_GS_AR_BYTES, 0, 0);

    rc |= vmwrite(HYPE_VMCS_GUEST_LDTR_SELECTOR, 0);
    rc |= vmwrite(HYPE_VMCS_GUEST_LDTR_LIMIT, 0);
    rc |= vmwrite(HYPE_VMCS_GUEST_LDTR_BASE, 0);
    rc |= vmwrite(HYPE_VMCS_GUEST_LDTR_AR_BYTES, 0x10000u); /* unusable */

    rc |= vmwrite(HYPE_VMCS_GUEST_TR_SELECTOR, 0);
    rc |= vmwrite(HYPE_VMCS_GUEST_TR_LIMIT, 0xFFFFu);
    rc |= vmwrite(HYPE_VMCS_GUEST_TR_BASE, 0);
    rc |= vmwrite(HYPE_VMCS_GUEST_TR_AR_BYTES, 0x8Bu); /* busy 32-bit TSS, present */

    rc |= vmwrite(HYPE_VMCS_GUEST_GDTR_BASE, 0);
    rc |= vmwrite(HYPE_VMCS_GUEST_GDTR_LIMIT, 0xFFFFu);
    rc |= vmwrite(HYPE_VMCS_GUEST_IDTR_BASE, 0);
    rc |= vmwrite(HYPE_VMCS_GUEST_IDTR_LIMIT, 0x3FFu);

    rc |= vmwrite(HYPE_VMCS_GUEST_CR0, 0x00000010u); /* ET-only, matching the SVM side */
    rc |= vmwrite(HYPE_VMCS_GUEST_CR3, 0);
    rc |= vmwrite(HYPE_VMCS_GUEST_CR4, 0);
    rc |= vmwrite(HYPE_VMCS_CR0_GUEST_HOST_MASK, 0);
    rc |= vmwrite(HYPE_VMCS_CR4_GUEST_HOST_MASK, 0);
    rc |= vmwrite(HYPE_VMCS_CR0_READ_SHADOW, 0x00000010u);
    rc |= vmwrite(HYPE_VMCS_CR4_READ_SHADOW, 0);
    rc |= vmwrite(HYPE_VMCS_GUEST_DR7, 0x400u);
    rc |= vmwrite(HYPE_VMCS_GUEST_RSP, stack_phys);
    rc |= vmwrite(HYPE_VMCS_GUEST_RIP, 0);
    rc |= vmwrite(HYPE_VMCS_GUEST_RFLAGS, 0x2u);
    rc |= vmwrite(HYPE_VMCS_GUEST_INTERRUPTIBILITY_STATE, 0);
    rc |= vmwrite(HYPE_VMCS_GUEST_ACTIVITY_STATE, 0);
    rc |= vmwrite(HYPE_VMCS_VMCS_LINK_POINTER, 0xFFFFFFFFFFFFFFFFULL);

    /*
     * Host state: whatever this project's own runtime is currently
     * using (M1-2/M1-3's GDT/IDT, current CR0/CR3/CR4), so a VM-exit
     * returns to our own environment. HOST_RIP/HOST_RSP point at a
     * placeholder halt stub, not a real dispatch loop -- M2-5 replaces
     * this once the VM-exit handler exists; nothing here is wired into
     * an actual VMLAUNCH yet (that's M2-7).
     */
    rc |= vmwrite(HYPE_VMCS_HOST_CR0, read_cr0());
    rc |= vmwrite(HYPE_VMCS_HOST_CR3, read_cr3());
    rc |= vmwrite(HYPE_VMCS_HOST_CR4, read_cr4());
    rc |= vmwrite(HYPE_VMCS_HOST_CS_SELECTOR, read_cs() & 0xF8u);
    rc |= vmwrite(HYPE_VMCS_HOST_SS_SELECTOR, read_ss() & 0xF8u);
    rc |= vmwrite(HYPE_VMCS_HOST_DS_SELECTOR, read_ds() & 0xF8u);
    rc |= vmwrite(HYPE_VMCS_HOST_ES_SELECTOR, read_es() & 0xF8u);
    rc |= vmwrite(HYPE_VMCS_HOST_FS_SELECTOR, read_fs() & 0xF8u);
    rc |= vmwrite(HYPE_VMCS_HOST_GS_SELECTOR, read_gs() & 0xF8u);
    rc |= vmwrite(HYPE_VMCS_HOST_TR_SELECTOR, read_tr() & 0xF8u);
    rc |= vmwrite(HYPE_VMCS_HOST_FS_BASE, 0);
    rc |= vmwrite(HYPE_VMCS_HOST_GS_BASE, 0);
    rc |= vmwrite(HYPE_VMCS_HOST_TR_BASE, 0);
    rc |= vmwrite(HYPE_VMCS_HOST_GDTR_BASE, read_gdtr_base());
    rc |= vmwrite(HYPE_VMCS_HOST_IDTR_BASE, read_idtr_base());
    rc |= vmwrite(HYPE_VMCS_HOST_IA32_SYSENTER_CS, 0);
    rc |= vmwrite(HYPE_VMCS_HOST_RSP, (uint64_t)&g_vmcs_region[sizeof(g_vmcs_region)]);
    rc |= vmwrite(HYPE_VMCS_HOST_RIP, (uint64_t)&hype_vmx_host_exit_stub);

    return rc;
}
