#include "vmcs.h"
#include "vmx.h"

#include "../../../core/fatal.h"
#include "../../../devices/pflash.h"
#include "../../../devices/pic.h"
#include "../../../devices/pit.h"
#include "../cpu/cpuid_emulate.h"
#include "../cpu/mmio_decode.h"
#include "../cpu/msr_emulate.h"
#include "../cpu/paging.h"
#include "../cpu/vmm_ops.h"
#include "ept.h"

#define HYPE_VMX_MMIO_MAX_INSTR_BYTES 15u

/* UNVALIDATED -- see vmx.h and vmcs.h. */

static uint8_t g_vmcs_region[4096] __attribute__((aligned(4096)));
static uint8_t g_virtual_apic_page[4096] __attribute__((aligned(4096)));

/* EPT paging structures for the (currently single) VMX test vCPU. Identity
 * map, built once in hype_vmx_vcpu_create() when no external root is passed.
 * Same shape/ownership as the SVM NPT tables; page-aligned as EPT requires. */
static hype_ept_pte_t g_ept_pml4[HYPE_EPT_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_ept_pte_t g_ept_pdpt[HYPE_EPT_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_ept_pte_t g_ept_pd[HYPE_EPT_MAX_GB][HYPE_EPT_ENTRIES_PER_TABLE]
    __attribute__((aligned(4096)));

/* Guest paging (ordinary long-mode PTEs) for a long-mode VMX guest that builds
 * its own CR3 -- e.g. the VMX-1 smoke test. Microtests supply their own
 * guest_cr3 (like the SVM side), so these back only the smoke path. */
static hype_pte_t g_vmx_guest_pml4[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_pte_t g_vmx_guest_pdpt[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_pte_t g_vmx_guest_pd[HYPE_PAGING_MAX_GB][HYPE_PAGING_ENTRIES_PER_TABLE]
    __attribute__((aligned(4096)));

/*
 * The VMX VM-entry/exit trampoline (vmx_run.S). Loads ctx->gprs into the real
 * GPRs, VMWRITEs HOST_RSP/HOST_RIP to return into itself, then VMLAUNCH (first
 * entry, launched=0) or VMRESUME (launched=1). Returns 0 on a clean VM-exit
 * (guest GPRs saved back into ctx), 1 on VM-entry failure (guest never ran).
 */
extern uint64_t hype_vmx_launch(hype_vcpu_ctx_t *ctx, uint64_t launched);

/*
 * VMX vCPU context. Mirrors the SVM struct's gprs[] contract (x86 register
 * encoding order, 0=RAX..15=R15, index 4=RSP unused since RSP lives in the
 * VMCS) so the same MMIO-decode register indexing works vendor-agnostically.
 * gprs[] MUST be first: vmx_run.S addresses it at a fixed zero offset. Unlike
 * SVM's VMCB (which auto-manages RAX/RSP/RIP/RFLAGS), VMX saves/restores NO
 * GPRs across VM-entry -- the trampoline moves all of them by hand.
 */
struct hype_vcpu_ctx {
    uint64_t gprs[16];
    int launched; /* 0 until the first successful VMLAUNCH; VMRESUME thereafter. */
};

/* Single test vCPU: the M2-M4-5 microtests run sequentially on the BSP, so
 * one static slot suffices (contrast the SVM pool, which runs two guests on
 * two cores concurrently). Revisit if VMX ever dispatches concurrent guests. */
static struct hype_vcpu_ctx g_vmx_ctx;

static void hype_vmx_host_exit_stub(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

/*
 * VMREAD (M2-8): read a VMCS field. AT&T operand order is the reverse of
 * VMWRITE's -- Intel's "VMREAD r/m64, r64" reads the field named by the
 * second (source-position) operand into the first (dest). In AT&T that's
 * "vmread field, dest". Returns the field value; sets *ok to 0 on failure
 * (CF/ZF set, e.g. unsupported field or no current VMCS).
 */
static inline uint64_t vmread(uint64_t field, int *ok) {
    uint64_t value = 0;
    uint8_t fail_zf, fail_cf;
    __asm__ volatile("vmread %3, %0\n\t"
                      "setz %1\n\t"
                      "setc %2"
                      : "=r"(value), "=q"(fail_zf), "=q"(fail_cf)
                      : "r"(field)
                      : "cc");
    if (ok) {
        *ok = (fail_zf || fail_cf) ? 0 : 1;
    }
    return value;
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

/*
 * Shared VMCS builder. long_mode=0 builds a real-mode guest entering at
 * cs_base:rip (CS.base = cs_base); long_mode=1 builds a flat 64-bit guest at
 * linear rip with paging root guest_cr3 (cs_base ignored). Public wrappers
 * hype_vmx_vmcs_build_guest / _long_mode_guest pin the mode.
 */
static int build_guest_common(uint64_t cs_base, uint64_t rip, uint64_t stack_phys, uint64_t eptp,
                              int long_mode, uint64_t guest_cr3) {
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

    /* These MUST be uint64_t: hype_vmx_adjust_controls() reads the allowed-1
     * mask from the HIGH 32 bits (allowed-0 from the low 32). Truncating the
     * capability MSR to uint32_t here zeroed the allowed-1 half, so
     * (desired|allowed0)&allowed1 collapsed every control field to 0 -- the
     * missing required-1 bits made VM-entry fail with instruction-error 7.
     * (Latent until M2-8: the M2-6 build only VMWROTE the VMCS, never
     * launched it.) */
    uint64_t pin_cap = rdmsr(have_true_ctls ? HYPE_MSR_IA32_VMX_TRUE_PINBASED_CTLS
                                            : HYPE_MSR_IA32_VMX_PINBASED_CTLS);
    uint64_t proc_cap = rdmsr(have_true_ctls ? HYPE_MSR_IA32_VMX_TRUE_PROCBASED_CTLS
                                             : HYPE_MSR_IA32_VMX_PROCBASED_CTLS);
    uint64_t proc2_cap = rdmsr(HYPE_MSR_IA32_VMX_PROCBASED_CTLS2);
    uint64_t exit_cap = rdmsr(have_true_ctls ? HYPE_MSR_IA32_VMX_TRUE_EXIT_CTLS
                                             : HYPE_MSR_IA32_VMX_EXIT_CTLS);
    uint64_t entry_cap = rdmsr(have_true_ctls ? HYPE_MSR_IA32_VMX_TRUE_ENTRY_CTLS
                                              : HYPE_MSR_IA32_VMX_ENTRY_CTLS);

    uint32_t pin_ctls = hype_vmx_adjust_controls(0, pin_cap);
    /* M2-8: a launchable guest. Primary controls activate the secondary
     * controls (for EPT + unrestricted guest below) and enable HLT-exiting
     * (so a guest HLT returns to hype). The APICv secondary bits the M2-4
     * struct-only build set (APIC_REGISTER_VIRT / VIRTUAL_INTERRUPT_DELIVERY,
     * and USE_TPR_SHADOW) are dropped here: they require a fully wired
     * virtual-APIC/posted-interrupt setup to pass VM-entry control checks,
     * and none of the M2-M4-5 test guests exercise APICv -- keeping the
     * control set minimal removes VM-entry failure surface. */
    uint32_t proc_ctls = hype_vmx_adjust_controls(
        HYPE_VMX_PROCBASED_ACTIVATE_SECONDARY_CONTROLS | HYPE_VMX_PROCBASED_HLT_EXITING, proc_cap);
    /* Unrestricted guest (lets the guest run with CR0.PE=0 / CR0.PG=0, i.e.
     * real mode) architecturally REQUIRES EPT -- so both bits go together. */
    uint32_t proc2_ctls = hype_vmx_adjust_controls(
        HYPE_VMX_PROCBASED2_ENABLE_EPT | HYPE_VMX_PROCBASED2_UNRESTRICTED_GUEST, proc2_cap);
    /* Host address-space size MUST be set: hype's host is 64-bit (see the
     * constant's comment) -- omitting it is the classic error-7 VM-entry
     * failure. Entry controls stay 0 (real-mode guest, not IA-32e). */
    uint32_t exit_ctls = hype_vmx_adjust_controls(HYPE_VMX_EXIT_HOST_ADDR_SPACE_SIZE, exit_cap);
    /* A long-mode guest needs IA-32e-mode-guest + load-IA32_EFER so the CPU
     * establishes EFER.LME/LMA consistently with CR0.PG/CR4.PAE. A real-mode
     * guest needs neither (entry controls stay just the required-1 bits). */
    uint32_t entry_desired =
        long_mode ? (HYPE_VMX_ENTRY_IA32E_MODE_GUEST | HYPE_VMX_ENTRY_LOAD_IA32_EFER) : 0;
    uint32_t entry_ctls = hype_vmx_adjust_controls(entry_desired, entry_cap);

    rc |= vmwrite(HYPE_VMCS_PIN_BASED_VM_EXEC_CONTROL, pin_ctls);
    rc |= vmwrite(HYPE_VMCS_CPU_BASED_VM_EXEC_CONTROL, proc_ctls);
    rc |= vmwrite(HYPE_VMCS_SECONDARY_VM_EXEC_CONTROL, proc2_ctls);
    rc |= vmwrite(HYPE_VMCS_VM_EXIT_CONTROLS, exit_ctls);
    rc |= vmwrite(HYPE_VMCS_VM_ENTRY_CONTROLS, entry_ctls);
    rc |= vmwrite(HYPE_VMCS_EXCEPTION_BITMAP, 0);

    /* EPT pointer (M2-8/M3-1): required now that ENABLE_EPT is set. Caller
     * passes the fully-formed EPTP (PML4 phys | memtype WB | walk-length-1 |
     * flags -- see hype_vmx_make_eptp()). */
    rc |= vmwrite(HYPE_VMCS_EPT_POINTER, eptp);

    /* TPR shadow/APICv (M2-4): only takes effect if the capability
     * negotiation above actually granted USE_TPR_SHADOW (older CPUs
     * without it will simply ignore VIRTUAL_APIC_PAGE_ADDR/
     * TPR_THRESHOLD). 0 threshold = no TPR-masking VM-exits. */
    rc |= vmwrite(HYPE_VMCS_VIRTUAL_APIC_PAGE_ADDR, (uint64_t)(uintptr_t)g_virtual_apic_page);
    rc |= vmwrite(HYPE_VMCS_TPR_THRESHOLD, 0);

    /* Guest segments. Long mode: flat 64-bit -- base 0, 4GB limit, CS is a
     * long-mode code segment (AR 0xA09B: type=exec/read/accessed, S, P, L,
     * G), data segments AR 0xC093 (type=RW/accessed, S, P, D/B, G). Real mode:
     * CS.base = cs_base (written manually so an entry base beyond a 16-bit
     * selector*16 works under unrestricted guest), DS/ES/SS/FS/GS base 0. */
    if (long_mode) {
        rc |= vmwrite(HYPE_VMCS_GUEST_CS_SELECTOR, 0x08u);
        rc |= vmwrite(HYPE_VMCS_GUEST_CS_BASE, 0);
        rc |= vmwrite(HYPE_VMCS_GUEST_CS_LIMIT, 0xFFFFFFFFu);
        rc |= vmwrite(HYPE_VMCS_GUEST_CS_AR_BYTES, 0xA09Bu);
        struct {
            uint32_t sel, base, limit, ar;
        } dseg[5] = {
            {HYPE_VMCS_GUEST_DS_SELECTOR, HYPE_VMCS_GUEST_DS_BASE, HYPE_VMCS_GUEST_DS_LIMIT,
             HYPE_VMCS_GUEST_DS_AR_BYTES},
            {HYPE_VMCS_GUEST_ES_SELECTOR, HYPE_VMCS_GUEST_ES_BASE, HYPE_VMCS_GUEST_ES_LIMIT,
             HYPE_VMCS_GUEST_ES_AR_BYTES},
            {HYPE_VMCS_GUEST_SS_SELECTOR, HYPE_VMCS_GUEST_SS_BASE, HYPE_VMCS_GUEST_SS_LIMIT,
             HYPE_VMCS_GUEST_SS_AR_BYTES},
            {HYPE_VMCS_GUEST_FS_SELECTOR, HYPE_VMCS_GUEST_FS_BASE, HYPE_VMCS_GUEST_FS_LIMIT,
             HYPE_VMCS_GUEST_FS_AR_BYTES},
            {HYPE_VMCS_GUEST_GS_SELECTOR, HYPE_VMCS_GUEST_GS_BASE, HYPE_VMCS_GUEST_GS_LIMIT,
             HYPE_VMCS_GUEST_GS_AR_BYTES},
        };
        for (unsigned s = 0; s < 5; s++) {
            rc |= vmwrite(dseg[s].sel, 0x10u);
            rc |= vmwrite(dseg[s].base, 0);
            rc |= vmwrite(dseg[s].limit, 0xFFFFFFFFu);
            rc |= vmwrite(dseg[s].ar, 0xC093u);
        }
    } else {
        rc |= vmwrite(HYPE_VMCS_GUEST_CS_SELECTOR, (cs_base >> 4) & 0xFFFFu);
        rc |= vmwrite(HYPE_VMCS_GUEST_CS_BASE, cs_base);
        rc |= vmwrite(HYPE_VMCS_GUEST_CS_LIMIT, 0xFFFFu);
        rc |= vmwrite(HYPE_VMCS_GUEST_CS_AR_BYTES, 0x9Bu);
        rc |= write_realmode_guest_segment(HYPE_VMCS_GUEST_DS_SELECTOR, HYPE_VMCS_GUEST_DS_BASE,
                                           HYPE_VMCS_GUEST_DS_LIMIT, HYPE_VMCS_GUEST_DS_AR_BYTES, 0,
                                           0);
        rc |= write_realmode_guest_segment(HYPE_VMCS_GUEST_ES_SELECTOR, HYPE_VMCS_GUEST_ES_BASE,
                                           HYPE_VMCS_GUEST_ES_LIMIT, HYPE_VMCS_GUEST_ES_AR_BYTES, 0,
                                           0);
        rc |= write_realmode_guest_segment(HYPE_VMCS_GUEST_SS_SELECTOR, HYPE_VMCS_GUEST_SS_BASE,
                                           HYPE_VMCS_GUEST_SS_LIMIT, HYPE_VMCS_GUEST_SS_AR_BYTES, 0,
                                           0);
        rc |= write_realmode_guest_segment(HYPE_VMCS_GUEST_FS_SELECTOR, HYPE_VMCS_GUEST_FS_BASE,
                                           HYPE_VMCS_GUEST_FS_LIMIT, HYPE_VMCS_GUEST_FS_AR_BYTES, 0,
                                           0);
        rc |= write_realmode_guest_segment(HYPE_VMCS_GUEST_GS_SELECTOR, HYPE_VMCS_GUEST_GS_BASE,
                                           HYPE_VMCS_GUEST_GS_LIMIT, HYPE_VMCS_GUEST_GS_AR_BYTES, 0,
                                           0);
    }

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

    /* Guest CR0/CR3/CR4/EFER must satisfy the VMX fixed-bit MSRs (observed on
     * this Intel box: CR0_FIXED0=0x80000021 -> PE|NE|PG required; CR4_FIXED0
     * =0x2000 -> VMXE required). Unrestricted guest relaxes only CR0.PE and
     * CR0.PG, so NE (bit5) and CR4.VMXE (bit13) are mandatory in BOTH modes.
     *   real mode: CR0 = ET|NE (PE=PG=0, allowed by unrestricted guest).
     *   long mode: CR0 = PG|PE|NE|ET; CR4 += PAE; CR3 = guest paging root;
     *              EFER = LME|LMA (loaded via the entry control set above). */
    uint64_t guest_cr0 = long_mode ? 0x80000031ull /* PG|NE|ET|PE */ : 0x00000030ull /* ET|NE */;
    uint64_t guest_cr4 = long_mode ? 0x00002020ull /* PAE|VMXE */ : 0x00002000ull /* VMXE */;
    rc |= vmwrite(HYPE_VMCS_GUEST_CR0, guest_cr0);
    rc |= vmwrite(HYPE_VMCS_GUEST_CR3, long_mode ? guest_cr3 : 0);
    rc |= vmwrite(HYPE_VMCS_GUEST_CR4, guest_cr4);
    if (long_mode) {
        rc |= vmwrite(HYPE_VMCS_GUEST_IA32_EFER, 0x500ull); /* LME|LMA */
    }
    rc |= vmwrite(HYPE_VMCS_CR0_GUEST_HOST_MASK, 0);
    rc |= vmwrite(HYPE_VMCS_CR4_GUEST_HOST_MASK, 0);
    rc |= vmwrite(HYPE_VMCS_CR0_READ_SHADOW, guest_cr0);
    rc |= vmwrite(HYPE_VMCS_CR4_READ_SHADOW, guest_cr4);
    rc |= vmwrite(HYPE_VMCS_GUEST_DR7, 0x400u);
    rc |= vmwrite(HYPE_VMCS_GUEST_RSP, stack_phys);
    rc |= vmwrite(HYPE_VMCS_GUEST_RIP, rip);
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
    uint16_t host_cs = read_cs() & 0xF8u;
    uint16_t host_tr = read_tr() & 0xF8u;
    /* VMX host-state check: the TR selector cannot be null (SDM 26.2.3). hype
     * never executes LTR post-EBS, so TR is often 0 here -> VM-instruction-
     * error 8 (invalid host-state field). On VM-exit the host TR *base* comes
     * from HOST_TR_BASE (below), limit is forced to 0x67, and the GDT is not
     * consulted (SDM 27.5.2) -- so any non-null selector with RPL=TI=0 works;
     * hype never uses the TSS during a guest run. Borrow the (valid, non-null)
     * host CS selector when TR is null. */
    if (host_tr == 0) {
        host_tr = host_cs;
    }
    /* One-shot: the host state is identical for every vCPU, so log it once
     * (VMX-2 builds many VMCSes) -- enough to diagnose a host-state VM-entry
     * failure without spamming the log per microtest. */
    {
        static int host_diag_printed;
        if (!host_diag_printed) {
            host_diag_printed = 1;
            hype_debug_print("vmx: host sel cs=0x%x ss=0x%x ds=0x%x tr(raw=0x%x used=0x%x) "
                             "cr0=0x%llx cr4=0x%llx\n",
                             (unsigned)host_cs, (unsigned)(read_ss() & 0xF8u),
                             (unsigned)(read_ds() & 0xF8u), (unsigned)(read_tr() & 0xF8u),
                             (unsigned)host_tr, (unsigned long long)read_cr0(),
                             (unsigned long long)read_cr4());
        }
    }

    rc |= vmwrite(HYPE_VMCS_HOST_CR0, read_cr0());
    rc |= vmwrite(HYPE_VMCS_HOST_CR3, read_cr3());
    rc |= vmwrite(HYPE_VMCS_HOST_CR4, read_cr4());
    rc |= vmwrite(HYPE_VMCS_HOST_CS_SELECTOR, host_cs);
    rc |= vmwrite(HYPE_VMCS_HOST_SS_SELECTOR, read_ss() & 0xF8u);
    rc |= vmwrite(HYPE_VMCS_HOST_DS_SELECTOR, read_ds() & 0xF8u);
    rc |= vmwrite(HYPE_VMCS_HOST_ES_SELECTOR, read_es() & 0xF8u);
    rc |= vmwrite(HYPE_VMCS_HOST_FS_SELECTOR, read_fs() & 0xF8u);
    rc |= vmwrite(HYPE_VMCS_HOST_GS_SELECTOR, read_gs() & 0xF8u);
    rc |= vmwrite(HYPE_VMCS_HOST_TR_SELECTOR, host_tr);
    rc |= vmwrite(HYPE_VMCS_HOST_FS_BASE, 0);
    rc |= vmwrite(HYPE_VMCS_HOST_GS_BASE, 0);
    rc |= vmwrite(HYPE_VMCS_HOST_TR_BASE, 0);
    rc |= vmwrite(HYPE_VMCS_HOST_GDTR_BASE, read_gdtr_base());
    rc |= vmwrite(HYPE_VMCS_HOST_IDTR_BASE, read_idtr_base());
    rc |= vmwrite(HYPE_VMCS_HOST_IA32_SYSENTER_CS, 0);
    rc |= vmwrite(HYPE_VMCS_HOST_RSP, (uint64_t)&g_vmcs_region[sizeof(g_vmcs_region)]);
    /* HOST_RIP/HOST_RSP are placeholders here: hype_vmx_vcpu_run()'s trampoline
     * (vmx_run.S) VMWRITEs the real values (its own .Lvmexit label + live stack)
     * on every entry, overriding these. The stub only keeps the field non-zero
     * for a build that never launches (M2-6 struct validation). */
    rc |= vmwrite(HYPE_VMCS_HOST_RIP, (uint64_t)&hype_vmx_host_exit_stub);

    return rc;
}

/* Public builders: real-mode guest at cs_base:rip; flat 64-bit guest at linear
 * entry_rip with paging root guest_cr3. Both take a prebuilt EPT pointer. */
int hype_vmx_vmcs_build_guest(uint64_t cs_base, uint64_t rip, uint64_t stack_phys, uint64_t eptp) {
    return build_guest_common(cs_base, rip, stack_phys, eptp, 0, 0);
}

int hype_vmx_vmcs_build_long_mode_guest(uint64_t entry_rip, uint64_t guest_cr3, uint64_t stack_phys,
                                        uint64_t eptp) {
    return build_guest_common(0, entry_rip, stack_phys, eptp, 1, guest_cr3);
}

/*
 * Assembles an EPT pointer from a PML4 physical address: memory type WB (6),
 * page-walk length 4 encoded as (4-1)<<3 = 0x18, giving a low byte of 0x1E.
 * Accessed/dirty flags left disabled (bit 6 = 0). Intel SDM Vol 3C, EPTP.
 */
uint64_t hype_vmx_make_eptp(uint64_t pml4_phys) {
    return (pml4_phys & ~0xFFFULL) | 0x1EULL;
}

/*
 * Punch an MMIO hole in the (internal, identity) EPT: clear the 2MB EPT PDE
 * covering `gpa` so a guest access there causes an EPT violation (reason 48)
 * instead of silently hitting RAM -- the VMX analogue of
 * hype_npt_mark_not_present. Call AFTER vcpu_create_long_mode (which rebuilds
 * the identity EPT). The device must own its own 2MB-aligned page, same
 * granularity constraint the NPT side already relies on.
 */
void hype_vmx_ept_mark_mmio_hole(uint64_t gpa) {
    unsigned gb = (unsigned)(gpa / HYPE_PAGING_1GB);
    unsigned pd_idx = (unsigned)((gpa % HYPE_PAGING_1GB) / HYPE_PAGING_2MB);
    if (gb < HYPE_EPT_MAX_GB) {
        g_ept_pd[gb][pd_idx] = 0; /* R/W/X all clear -> not present */
    }
}

/*
 * VMX vcpu_create (M2-8, VMX-1). Builds an identity EPT (guest-physical ==
 * host-physical, matching the SVM NPT identity map) and a launchable VMCS for
 * a real-mode guest entering at physical guest_rip with stack guest_rsp, then
 * returns the (single, static) vCPU context.
 *
 * ept_or_npt_root is currently ignored: the ops contract passes whatever root
 * the caller built, but the M2-M4-5 microtests build it with the SVM NPT
 * helper (ordinary long-mode PTEs), which is NOT a valid EPT structure (EPT
 * uses a distinct entry format -- see ept.h). Rather than misinterpret it,
 * VMX builds its own identity EPT here. Revisit if a caller ever hands VMX a
 * real EPT root.
 */
hype_vcpu_ctx_t *hype_vmx_vcpu_create(uint64_t guest_rip, uint64_t guest_rsp,
                                      uint64_t ept_or_npt_root) {
    struct hype_vcpu_ctx *ctx = &g_vmx_ctx;
    uint64_t eptp;
    unsigned i;

    (void)ept_or_npt_root;

    hype_ept_build_identity(g_ept_pml4, g_ept_pdpt, g_ept_pd, HYPE_EPT_MAX_GB);
    eptp = hype_vmx_make_eptp((uint64_t)(uintptr_t)g_ept_pml4);

    /* cs_base = guest_rip, rip = 0: the guest starts executing at physical
     * guest_rip in real mode (CS.base:IP = guest_rip:0). */
    if (hype_vmx_vmcs_build_guest(guest_rip, 0, guest_rsp, eptp) != 0) {
        return 0;
    }

    for (i = 0; i < 16; i++) {
        ctx->gprs[i] = 0;
    }
    ctx->launched = 0;
    return (hype_vcpu_ctx_t *)ctx;
}

/*
 * VMX vcpu_create_long_mode (M2-8, VMX-2). The VMX mirror of
 * hype_svm_vcpu_create_long_mode(): builds an identity EPT and a flat 64-bit
 * guest VMCS entering at linear entry_rip with the caller-supplied guest CR3
 * (the caller builds guest paging, exactly as the SVM microtests already do).
 * This is what the M2-M4-5 microtests use; the real-mode vcpu_create() above
 * is only the ops-vtable/FW-1 entry point. ept_or_npt_root is ignored for the
 * same reason as vcpu_create() (VMX builds its own EPT).
 */
hype_vcpu_ctx_t *hype_vmx_vcpu_create_long_mode(uint64_t entry_rip, uint64_t guest_cr3,
                                                uint64_t guest_rsp, uint64_t ept_or_npt_root) {
    struct hype_vcpu_ctx *ctx = &g_vmx_ctx;
    uint64_t eptp;
    unsigned i;

    (void)ept_or_npt_root;

    hype_ept_build_identity(g_ept_pml4, g_ept_pdpt, g_ept_pd, HYPE_EPT_MAX_GB);
    eptp = hype_vmx_make_eptp((uint64_t)(uintptr_t)g_ept_pml4);

    if (hype_vmx_vmcs_build_long_mode_guest(entry_rip, guest_cr3, guest_rsp, eptp) != 0) {
        return 0;
    }

    for (i = 0; i < 16; i++) {
        ctx->gprs[i] = 0;
    }
    ctx->launched = 0;
    return (hype_vcpu_ctx_t *)ctx;
}

/*
 * VMX vcpu_run (M2-8, VMX-1). Enters the guest via the VMLAUNCH/VMRESUME
 * trampoline and, on a clean VM-exit, populates *info from the VMCS
 * (VM_EXIT_REASON / EXIT_QUALIFICATION / GUEST_RIP) via VMREAD. Returns 0 on a
 * VM-exit, -1 on a VM-entry failure (VMLAUNCH/VMRESUME faulted before the guest
 * ran -- info->reason then carries the VM-instruction-error code with bit 63
 * set as a marker so the caller can tell it apart from a real exit reason).
 *
 * Note a VM-entry failure due to invalid guest state does NOT fault the
 * instruction; it causes a normal VM-exit to HOST_RIP whose reason has bit 31
 * set (HYPE_VMX_EXIT_ENTRY_FAILURE) -- surfaced verbatim in info->reason.
 */
int hype_vmx_vcpu_run(hype_vcpu_ctx_t *ctx, hype_vmexit_info_t *info) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    uint64_t failed;
    int ok;

    failed = hype_vmx_launch(ctx, (uint64_t)real->launched);
    if (failed) {
        uint64_t err = vmread(HYPE_VMCS_VM_INSTRUCTION_ERROR, &ok);
        info->reason = (1ULL << 63) | (ok ? err : 0);
        info->qualification = 0;
        info->guest_rip = 0;
        return -1;
    }

    real->launched = 1;
    info->reason = vmread(HYPE_VMCS_VM_EXIT_REASON, &ok);
    info->qualification = vmread(HYPE_VMCS_EXIT_QUALIFICATION, &ok);
    info->guest_rip = vmread(HYPE_VMCS_GUEST_RIP, &ok);
    return 0;
}

/* Advance guest RIP past the instruction that caused the exit, using the exact
 * length the CPU recorded (VM_EXIT_INSTRUCTION_LEN) -- the VMX analogue of
 * SVM's "rip += 2" for CPUID/RDMSR/WRMSR (all coincidentally 2 bytes, but the
 * VMCS field is authoritative and works for any emulated instruction). */
static void vmx_advance_rip(void) {
    int ok;
    uint64_t rip = vmread(HYPE_VMCS_GUEST_RIP, &ok);
    uint64_t len = vmread(HYPE_VMCS_VM_EXIT_INSTRUCTION_LEN, &ok);
    vmwrite(HYPE_VMCS_GUEST_RIP, rip + len);
}

static inline void vmx_real_cpuid(uint32_t eax, uint32_t ecx, hype_cpuid_result_t *out) {
    __asm__ volatile("cpuid"
                     : "=a"(out->eax), "=b"(out->ebx), "=c"(out->ecx), "=d"(out->edx)
                     : "a"(eax), "c"(ecx));
}

/*
 * VMX CPUID handler (VMX-2): mirror of hype_svm_vcpu_handle_cpuid. Guest GPRs
 * live in ctx->gprs after the exit (0=RAX,1=RCX,2=RDX,3=RBX). Read the CPUID
 * input (EAX/ECX), synthesize via the shared hype_cpuid_emulate(), write the
 * four result registers back, and advance past the 2-byte CPUID.
 */
void hype_vmx_vcpu_handle_cpuid(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    uint32_t eax_in = (uint32_t)real->gprs[0];
    uint32_t ecx_in = (uint32_t)real->gprs[1];
    hype_cpuid_result_t host_real, out;

    vmx_real_cpuid(eax_in, ecx_in, &host_real);
    hype_cpuid_emulate(eax_in, ecx_in, &host_real, &out);

    real->gprs[0] = out.eax; /* RAX */
    real->gprs[3] = out.ebx; /* RBX */
    real->gprs[1] = out.ecx; /* RCX */
    real->gprs[2] = out.edx; /* RDX */
    vmx_advance_rip();
}

/*
 * VMX MSR handler (VMX-2): mirror of hype_svm_vcpu_handle_msr's general path.
 * is_write distinguishes WRMSR (exit reason 32) from RDMSR (31) -- the VMX
 * analogue of SVM's exitinfo1 bit 0. Reuses the vendor-neutral
 * hype_msr_decide(); MSR number in ECX (gprs[1]), value in EDX:EAX
 * (gprs[2]:gprs[0]). Guest EFER lives in the VMCS GUEST_IA32_EFER field (not a
 * GPR), so it is VMREAD/VMWRITE'd. Returns 0 if handled, -1 to reject.
 *
 * The pvclock / MTRR / PAT special-casing the SVM handler carries is omitted:
 * the M2-M4-5 microtests (the VMX validation set) touch only APIC_BASE + EFER;
 * a full guest OS on VMX would need those ported too (future work).
 */
int hype_vmx_vcpu_handle_msr(hype_vcpu_ctx_t *ctx, int is_write) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    uint32_t msr_number = (uint32_t)real->gprs[1];
    hype_msr_action_t action = hype_msr_decide(msr_number, is_write);
    int ok;

    switch (action) {
    case HYPE_MSR_ACTION_READ_APIC_BASE: {
        uint64_t value = hype_msr_apic_base_value();
        real->gprs[0] = (uint64_t)(uint32_t)value;
        real->gprs[2] = (uint64_t)(uint32_t)(value >> 32);
        break;
    }
    case HYPE_MSR_ACTION_READWRITE_EFER:
        if (is_write) {
            uint64_t value =
                ((uint64_t)(uint32_t)real->gprs[2] << 32) | (uint64_t)(uint32_t)real->gprs[0];
            vmwrite(HYPE_VMCS_GUEST_IA32_EFER, value);
        } else {
            uint64_t efer = vmread(HYPE_VMCS_GUEST_IA32_EFER, &ok);
            real->gprs[0] = (uint64_t)(uint32_t)efer;
            real->gprs[2] = (uint64_t)(uint32_t)(efer >> 32);
        }
        break;
    case HYPE_MSR_ACTION_READ_TSC: {
        uint64_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        real->gprs[0] = (uint64_t)(uint32_t)lo;
        real->gprs[2] = (uint64_t)(uint32_t)hi;
        break;
    }
    case HYPE_MSR_ACTION_REJECT:
    default:
        return -1;
    }
    vmx_advance_rip();
    return 0;
}

/* VMX set_rsi (VMX-2): mirror of hype_svm_vcpu_set_rsi. RSI is index 6 in the
 * ctx GPR array; the trampoline loads it into the real RSI at VM-entry (e.g.
 * the Linux boot protocol's zero-page pointer for m3-5). */
void hype_vmx_vcpu_set_rsi(hype_vcpu_ctx_t *ctx, uint64_t rsi) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    real->gprs[6] = rsi;
}

/*
 * VMX IOIO handler (VMX-2): mirror of hype_svm_vcpu_handle_ioio. The IO exit
 * (reason 30) records port/direction/size in EXIT_QUALIFICATION (bits 31:16 =
 * port, bit 3 = 1 for IN, bits 2:0 = size-1) rather than SVM's EXITINFO1. The
 * I/O value is the low byte of RAX (gprs[0]) -- these ports (PIC 0x20/0x21/
 * 0xA0/0xA1, PIT 0x40-0x43, port 0x61) are all byte-wide, same as the SVM
 * side. Dispatches to the identical PIC/PIT device models, then advances RIP.
 * Returns 0 if handled, -1 for an unmodelled port.
 */
int hype_vmx_vcpu_handle_ioio(hype_vcpu_ctx_t *ctx, hype_pic_emu_t *pic, hype_pit_emu_t *pit) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    int ok, rc;
    uint64_t qual = vmread(HYPE_VMCS_EXIT_QUALIFICATION, &ok);
    uint16_t port = (uint16_t)((qual >> 16) & 0xFFFFu);
    int is_in = (int)((qual >> 3) & 1u);
    uint8_t rax = (uint8_t)(real->gprs[0] & 0xFFu);

    if (port == 0x20u || port == 0x21u || port == 0xA0u || port == 0xA1u) {
        if (is_in) {
            uint8_t value = 0;
            rc = hype_pic_emu_io_read(pic, port, &value);
            if (rc == 0) {
                real->gprs[0] = (real->gprs[0] & ~0xFFULL) | value;
            }
        } else {
            rc = hype_pic_emu_io_write(pic, port, rax);
        }
    } else if (port >= 0x40u && port <= 0x43u) {
        if (is_in) {
            uint8_t value = 0;
            rc = hype_pit_emu_io_read(pit, port, &value);
            if (rc == 0) {
                real->gprs[0] = (real->gprs[0] & ~0xFFULL) | value;
            }
        } else {
            rc = hype_pit_emu_io_write(pit, port, rax);
        }
    } else if (port == 0x61u) {
        if (is_in) {
            real->gprs[0] = (real->gprs[0] & ~0xFFULL) | hype_pit_emu_port61_read(pit);
        } else {
            hype_pit_emu_port61_write(pit, rax);
        }
        rc = 0;
    } else {
        return -1;
    }

    if (rc != 0) {
        return -1;
    }
    vmx_advance_rip();
    return 0;
}

/* Guest GPR by ModRM.reg encoding, VMX flavour. Unlike SVM (RAX in the VMCB),
 * every VMX guest GPR lives in ctx->gprs, so this is a straight index -- except
 * RSP (index 4), which no MMIO operand can legally be (reject, matching SVM). */
static uint64_t *vmx_gpr_ptr(struct hype_vcpu_ctx *real, uint8_t reg) {
    if (reg == 4u || reg >= 16u) {
        return 0;
    }
    return &real->gprs[reg];
}

/*
 * VMX MMIO handler for the emulated pflash (VMX-2): mirror of
 * hype_svm_vcpu_handle_npf, driven by an EPT violation (reason 48) instead of
 * an NPF. The faulting GPA comes from GUEST_PHYSICAL_ADDRESS, the write bit
 * from EXIT_QUALIFICATION bit 1. The faulting instruction is decoded straight
 * out of guest memory at GUEST_RIP -- valid as a host pointer because the test
 * guests are a flat identity map (guest-linear == guest-physical == host, via
 * identity guest paging + identity EPT), the same assumption the SVM NPF path
 * and the microtests' payload-write already rely on. Returns 0 if handled, -1
 * to reject (fault outside the pflash window, decode failure, or dir mismatch).
 */
int hype_vmx_vcpu_handle_pflash_npf(hype_vcpu_ctx_t *ctx, hype_pflash_t *pf,
                                    uint64_t pf_base_phys) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_mmio_decode_t decoded;
    uint64_t *reg;
    int ok;
    uint64_t gpa = vmread(HYPE_VMCS_GUEST_PHYSICAL_ADDRESS, &ok);
    uint64_t qual = vmread(HYPE_VMCS_EXIT_QUALIFICATION, &ok);
    uint64_t rip = vmread(HYPE_VMCS_GUEST_RIP, &ok);
    int is_write = (int)((qual >> 1) & 1u);
    uint32_t offset;
    const uint8_t *guest_bytes;

    if (gpa < pf_base_phys) {
        return -1;
    }
    offset = (uint32_t)(gpa - pf_base_phys);

    guest_bytes = (const uint8_t *)(uintptr_t)rip;
    if (hype_mmio_decode(guest_bytes, HYPE_VMX_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (decoded.is_write != is_write) {
        return -1;
    }

    reg = vmx_gpr_ptr(real, decoded.reg);
    if (reg == 0) {
        return -1;
    }

    if (decoded.is_write) {
        uint32_t value = hype_mmio_extract_write_value(*reg, decoded.size_bytes);
        if (hype_pflash_write(pf, offset, decoded.size_bytes, value) != 0) {
            return -1;
        }
    } else {
        uint32_t value = 0;
        if (hype_pflash_read(pf, offset, decoded.size_bytes, &value) != 0) {
            return -1;
        }
        *reg = hype_mmio_merge_read_value(*reg, value, decoded.size_bytes, decoded.zero_extend);
    }

    vmwrite(HYPE_VMCS_GUEST_RIP, rip + decoded.instr_len);
    return 0;
}

/*
 * VMX-1 self-contained smoke test. Proves the whole VM-entry/exit round trip
 * (vcpu_create -> EPT + launchable VMCS -> hype_vmx_launch VMLAUNCH -> VM-exit
 * -> exit info -> VMRESUME) on real VMX hardware, independent of the M2-M4-5
 * microtest guest ABI (that's VMX-2). The guest is three bytes at a static,
 * EPT-identity-mapped address: CPUID (0F A2), then HLT (F4). CPUID exits
 * unconditionally (no control needed); HLT exits because HLT-exiting is set.
 * Expected: run 0 -> reason 10 (CPUID), advance RIP, run 1 -> reason 12 (HLT).
 * Returns 0 on that exact sequence, -1 otherwise. Gated off in normal boot;
 * called only when boot/main.c requests it on an Intel/VMX backend.
 */
static uint8_t g_smoke_guest[16] __attribute__((aligned(16)));
static uint8_t g_smoke_stack[4096] __attribute__((aligned(16)));

int hype_vmx_smoke_test(void) {
    hype_vcpu_ctx_t *ctx;
    hype_vmexit_info_t info;
    int i, ok;

    uint64_t guest_cr3;

    g_smoke_guest[0] = 0x0F; /* CPUID */
    g_smoke_guest[1] = 0xA2;
    g_smoke_guest[2] = 0xF4; /* HLT  */

    /* Long-mode guest: flat 64-bit, so the guest RIP can be the blob's actual
     * (high) load address -- real mode's CS.base==selector<<4 rule caps that at
     * ~1MB. Guest paging identity-maps linear==physical so the RIP resolves. */
    hype_paging_build_identity(g_vmx_guest_pml4, g_vmx_guest_pdpt, g_vmx_guest_pd,
                               HYPE_PAGING_MAX_GB);
    guest_cr3 = (uint64_t)(uintptr_t)g_vmx_guest_pml4;

    ctx = hype_vmx_vcpu_create_long_mode((uint64_t)(uintptr_t)g_smoke_guest, guest_cr3,
                                         (uint64_t)(uintptr_t)&g_smoke_stack[sizeof(g_smoke_stack)],
                                         0);
    if (ctx == 0) {
        hype_debug_print("vmx-smoke: vcpu_create FAILED (VMCS build error)\n");
        return -1;
    }
    hype_debug_print("vmx-smoke: guest at 0x%llx, launching...\n",
                     (unsigned long long)(uintptr_t)g_smoke_guest);

    for (i = 0; i < 4; i++) {
        int rc = hype_vmx_vcpu_run(ctx, &info);
        uint64_t reason = info.reason & 0xFFFFu;
        hype_debug_print("vmx-smoke: run%d rc=%d reason=0x%llx qual=0x%llx rip=0x%llx\n", i, rc,
                         (unsigned long long)info.reason, (unsigned long long)info.qualification,
                         (unsigned long long)info.guest_rip);
        if (rc < 0) {
            hype_debug_print("vmx-smoke: VM-ENTRY FAILURE (instr err=0x%llx)\n",
                             (unsigned long long)(info.reason & ~(1ULL << 63)));
            return -1;
        }
        if (reason == HYPE_VMX_EXIT_REASON_HLT) {
            hype_debug_print("vmx-smoke: HLT exit -- VMX round trip PASS\n");
            return 0;
        }
        if (reason == HYPE_VMX_EXIT_REASON_CPUID) {
            uint64_t len = vmread(HYPE_VMCS_VM_EXIT_INSTRUCTION_LEN, &ok);
            vmwrite(HYPE_VMCS_GUEST_RIP, info.guest_rip + len);
            continue;
        }
        hype_debug_print("vmx-smoke: unexpected exit reason -- FAIL\n");
        return -1;
    }
    hype_debug_print("vmx-smoke: no HLT within 4 exits -- FAIL\n");
    return -1;
}
