#include "vmcs.h"
#include "../cpu/fpu_state.h"
#include "vmx.h"

#include "../../../core/blk_backend.h"
#include "../../../core/fatal.h"
#include "../cpu/isr.h"
#include "../../../core/guest_mem.h"
#include "../../../devices/ahci.h"
#include "../../../devices/atapi.h"
#include "../../../devices/bochs_vbe.h"
#include "../../../devices/fw_cfg.h"
#include "../../../devices/pci.h"
#include "../../../devices/virtio_blk.h"
#include "../../../devices/cmos.h"
#include "../../../devices/guest_lapic.h"
#include "../../../devices/guest_uart.h"
#include "../../../devices/ioapic.h"
#include "../../../devices/pflash.h"
#include "../../../devices/pvclock.h" /* VMX-4: hype_pvclock_calc_scale */
#include "../../../devices/pic.h"
#include "../../../devices/pit.h"
#include "../../../devices/ps2_keyboard.h"
#include "../../../devices/ps2_mouse.h"
#include "../cpu/cpuid_emulate.h"
#include "../cpu/mmio_decode.h"
#include "../cpu/msr_emulate.h"
#include "../cpu/paging.h"
#include "../cpu/vmm_ops.h"
/*
 * VMX-4 (#236): for the pending-interrupt IRR bit helpers (hype_svm_irr_set/
 * clear/any/highest) and hype_svm_can_accept_interrupt(). These are pure,
 * unit-tested bit logic with no VMCB access, and the VMX interrupt path needs
 * exactly the same rules -- so it calls them rather than growing a second
 * implementation to drift out of sync. #242 was a duplicated vendor path that
 * diverged, and re-deriving interrupt priority per backend is the same trap.
 * (Follow-up worth doing: relocate them to a vendor-neutral arch/x86_64/cpu
 * home so VMX need not include an SVM header at all.)
 */
#include "../svm/vmcb.h"
#include "ept.h"

#define HYPE_VMX_MMIO_MAX_INSTR_BYTES 15u

/* UNVALIDATED -- see vmx.h and vmcs.h. */

/*
 * #271: per-vCPU VMCS pool. Was a single g_vmcs_region, which was correct while
 * only the sequential BSP-only microtests used VMX. It stops being correct the
 * moment run_fw_1_test() dispatches each VM to its own AP: two cores would share
 * one VMCS.
 *
 * Sized for the CONCURRENT guests. The sequential self-test battery allocates
 * first, so hype_vmx_vcpu_pool_reset() hands the slots back before the concurrent
 * guests are created -- without that the battery drains the pool and the real VMs
 * both clamp to the same slot, which is #237 exactly.
 */
#define HYPE_VMX_MAX_VCPUS 4u
static uint8_t g_vmcs_pool[HYPE_VMX_MAX_VCPUS][4096] __attribute__((aligned(4096)));
/* #277: one virtual-APIC page PER vCPU slot. A single shared page went into every
 * VMCS as VIRTUAL_APIC_PAGE_ADDR, so two guests would share one TPR the moment the
 * capability negotiation granted USE_TPR_SHADOW. Same shape as #276's MSR areas. */
static uint8_t g_virtual_apic_page[HYPE_VMX_MAX_VCPUS][4096] __attribute__((aligned(4096)));

/* #248: did the CPU actually grant acknowledge-interrupt-on-exit? Set from the
 * ADJUSTED exit controls in hype_vmx_vcpu_create(), never from what was
 * requested -- adjust_controls() silently drops unsupported bits, and an L0
 * hypervisor need not offer this one. It selects which of the two
 * interrupt-consumption paths hype_vmx_vcpu_run() must take, and guessing wrong
 * either loses every host timer tick or reinstates the exit storm. */
static int g_vmx_ack_intr_on_exit = 0;

/* #248: defined next to the CR-access handler, but the EFER WRMSR path above it
 * needs it too -- either of CR0.PG and EFER.LME changing can flip long mode. */
static void vmx_sync_long_mode(void);
static void vmx_make_fs_gs_usable(void);
/* #251: defined beside hype_vmx_vcpu_set_pvclock(), which owns the scale globals
 * they read; the MSR handler above needs them. */
static void vmx_pvclock_arm_system_time(struct hype_vcpu_ctx *real, uint64_t msr_value);
static void vmx_pvclock_arm_wall_clock(struct hype_vcpu_ctx *real, uint64_t msr_value);

/* #248: the VM-entry-controls capability MSR, cached at VMCS setup so a later
 * mode transition can re-adjust the controls instead of writing them raw.
 * Writing a raw value is how the first version of vmx_sync_long_mode() produced
 * VM-instruction-error 7 (invalid control fields): hype_vmx_adjust_controls()
 * forces the required-1 bits and masks off anything the CPU -- or an L0
 * hypervisor, when nested -- does not allow, and skipping it means writing a
 * combination the hardware rejects at the next entry. */
static uint64_t g_vmx_entry_cap = 0;

/*
 * #251: guest XCR0. XSETBV always exits on VMX, and there is no VMCS field for
 * XCR0, so hype has to hold the guest's value itself and swap it around each
 * entry/exit -- otherwise the guest's XSAVE configuration would leak into host
 * context, where hype's own FPU/XSAVE state is interpreted under it.
 *
 * g_vmx_host_xcr0 is captured once at VMCS build. The swap only happens after the
 * guest has actually executed an XSETBV (ctx->guest_xcr0_valid, #277), so the common
 * case costs nothing.
 */
static uint64_t g_vmx_host_xcr0 = 0;

/* Defined further down; needed by the XCR0 helpers below. */
static inline uint64_t read_cr4(void);
static void vmx_real_cpuid(uint32_t leaf, uint32_t subleaf, hype_cpuid_result_t *out);

/*
 * #251: XGETBV/XSETBV are #UD unless CR4.OSXSAVE (bit 18) is set -- and that is a
 * per-context bit. The GUEST sets it in its own CR4 before using XSETBV; hype's
 * host CR4 is separate and does not have it, so hype executing XSETBV to service
 * the guest faulted #UD in HOST context and took hype down (observed:
 * "unhandled interrupt: vector=6 (Invalid Opcode) rip=0x14002b15a cs=0x8").
 *
 * A hypervisor that virtualises XSAVE has to be able to touch XCR0, so enable
 * OSXSAVE for hype once, on demand, when the CPU reports XSAVE support
 * (CPUID.1:ECX bit 26). Enabling it only permits XGETBV/XSETBV; it changes
 * nothing else about how hype runs. Returns 0 if XCR0 cannot be managed at all,
 * in which case the caller must NOT execute either instruction.
 */
static int vmx_ensure_osxsave(void) {
    hype_cpuid_result_t c1;
    uint64_t cr4 = read_cr4();

    if ((cr4 & (1ull << 18)) != 0ull) {
        return 1;
    }
    vmx_real_cpuid(1u, 0u, &c1);
    if ((c1.ecx & (1u << 26)) == 0u) { /* no XSAVE on this CPU */
        return 0;
    }
    cr4 |= (1ull << 18);
    __asm__ volatile("mov %0, %%cr4" ::"r"(cr4) : "memory");
    return 1;
}

static inline uint64_t xgetbv0(void) {
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline void xsetbv0(uint64_t val) {
    __asm__ volatile("xsetbv" ::"a"((uint32_t)val), "d"((uint32_t)(val >> 32)), "c"(0) : "memory");
}

/*
 * #251 slice 2: the VM-entry/exit MSR areas.
 *
 * Layout is fixed by the SDM (Table 24-14): 32-bit MSR index, 32 reserved bits,
 * then the 64-bit value; the area must be 16-byte aligned.
 *
 * g_vmx_msr_guest is used for BOTH entry-load and exit-store. That is the point:
 * SWAPGS exchanges GS.base with IA32_KERNEL_GS_BASE without causing a VM exit, so
 * hype cannot observe it. Storing on exit into the same table the next entry loads
 * from means the guest's value survives regardless of how it changed.
 *
 * g_vmx_msr_host is exit-load-only, capturing hype's own values so the host does
 * not resume on the guest's SYSCALL targets or per-CPU base.
 */
typedef struct {
    uint32_t index;
    uint32_t reserved;
    uint64_t value;
} hype_vmx_msr_entry_t;

static const uint32_t g_vmx_msr_list[] = {
    HYPE_MSR_IA32_KERNEL_GS_BASE, HYPE_MSR_IA32_STAR, HYPE_MSR_IA32_LSTAR,
    HYPE_MSR_IA32_CSTAR,          HYPE_MSR_IA32_SFMASK,
    /*
     * #270: IA32_TSC_AUX belongs here for exactly the same reason as the SYSCALL
     * MSRs above -- it is guest state the CPU consumes directly (RDTSCP returns it
     * in ECX), so it has to be swapped around every transition rather than
     * emulated on access. Putting it in this area gets that for free: hardware
     * loads the guest's value on entry and stores it back on exit, and
     * vmx_msr_area_slot() makes the RDMSR/WRMSR handler read and write the same
     * slot, so the guest sees what it wrote. Previously the write was absorbed and
     * RDTSCP returned the HOST's value.
     */
    HYPE_MSR_IA32_TSC_AUX,
};
#define HYPE_VMX_MSR_AREA_COUNT (sizeof(g_vmx_msr_list) / sizeof(g_vmx_msr_list[0]))

/*
 * #276: PER vCPU SLOT, not one global pair.
 *
 * These were single arrays whose addresses went into every VMCS, so two
 * concurrent guests shared one MSR-load/store area. Guest A's exit STORED its
 * IA32_KERNEL_GS_BASE here and guest B's entry then LOADED it, resuming B on
 * A's per-CPU base; the first swapgs-dependent entry dereferenced another
 * guest's per-CPU data, faulted on the entry stack, and double-faulted. Both
 * Intel guests died that way just after /init, when userspace starts issuing
 * syscalls. Indexed exactly like g_vmcs_pool/g_vmx_ctx_pool (#271) -- pooling
 * the VMCS is not enough on its own if what the VMCS POINTS AT stays shared.
 */
static hype_vmx_msr_entry_t g_vmx_msr_guest[HYPE_VMX_MAX_VCPUS][HYPE_VMX_MSR_AREA_COUNT]
    __attribute__((aligned(16)));
static hype_vmx_msr_entry_t g_vmx_msr_host[HYPE_VMX_MAX_VCPUS][HYPE_VMX_MSR_AREA_COUNT]
    __attribute__((aligned(16)));

/* Index into g_vmx_msr_guest for `msr`, or -1. Used by the RDMSR/WRMSR handler so
 * the guest reads back what it wrote even between transitions. */
static int vmx_msr_area_slot(uint32_t msr) {
    unsigned i;
    for (i = 0; i < HYPE_VMX_MSR_AREA_COUNT; i++) {
        if (g_vmx_msr_list[i] == msr) {
            return (int)i;
        }
    }
    return -1;
}

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
    /* #260: guest x87/SSE state. Neither VMX nor SVM saves it for us and hype's
     * own handlers use XMM, so it is saved/restored around VM entry. Per-vCPU. */
    hype_fpu_area_t fpu;
    int launched; /* 0 until the first successful VMLAUNCH; VMRESUME thereafter. */
    /*
     * #273: the VPID actually programmed into this vCPU's VMCS, or 0 when the
     * CPU's EPT_VPID_CAP did not support enabling VPID (in which case the guest
     * genuinely runs on VPID 0 and every VM-entry flushes it). Recorded rather
     * than recomputed from the slot so it reports what the hardware will use,
     * not what was intended, and readable outside vcpu_run where the VMCS is not
     * current so vmread is unavailable.
     */
    uint16_t vpid;
    /*
     * Pending external interrupts (INT-1/INT-2 on VMX), staged into
     * VM_ENTRY_INTR_INFO once the guest can accept one.
     *
     * VMX-4 (#236): this was a single `intr_pending`/`intr_vector` slot, which
     * was fine for the microtests (one IRQ source, one vector in flight) but
     * silently WRONG for a live guest: FW-1 has five sources -- PIT IRQ0,
     * keyboard IRQ1, COM1 IRQ4, mouse IRQ12 and the LAPIC timer -- so a second
     * vector arriving before the first was injected would overwrite it and the
     * interrupt would be lost. Now a 256-bit IRR, matching the SVM ctx.
     */
    uint32_t pending_irr[8];
    /* M4-6b1: the guest's pvclock shared page, if it enabled one. */
    const hype_gpa_map_t *pvclock_map;
    /* #251: last value the guest wrote to each pvclock MSR, so a RDMSR reads back
     * what it set. Mirrors the SVM ctx's fields of the same name. */
    uint64_t pvclock_system_msr;
    uint64_t pvclock_wall_msr;
    /*
     * #277: this guest's XCR0, and whether it has executed an XSETBV yet. Was a
     * file-global pair, so with two guests whichever set XCR0 last decided the
     * feature set BOTH ran under and both saw reported by CPUID leaf 0xD. That
     * corrupts vector state silently rather than crashing -- the #260 failure
     * mode -- so it belongs in the ctx like the pvclock fields above.
     * The HOST's XCR0 stays global: there is genuinely only one of it.
     */
    uint64_t guest_xcr0;
    int guest_xcr0_valid;
};

/*
 * Single test vCPU: the M2-M4-5 microtests run sequentially on the BSP, so
 * one static slot suffices (contrast the SVM pool, which runs two guests on
 * two cores concurrently).
 *
 * VMX-4 (#236): this is the VMX analogue of #237 and MUST be fixed before two
 * concurrent Intel guests. #237 was the SVM slot pool silently clamping so vm0
 * and vm1 shared one VMCB on two cores -- here there is not even a pool, just
 * one ctx and one VMCS, so two guests would collide outright. A single live
 * guest (this ticket's first bar) is safe; two is not. Fixing it means a pool
 * of ctx + VMCS regions AND per-VM EPT roots + VPID, since the EPT here is one
 * global identity map.
 */
static struct hype_vcpu_ctx g_vmx_ctx_pool[HYPE_VMX_MAX_VCPUS];
static unsigned g_vmx_vcpu_count = 0;

/*
 * #271/#237: allocate a slot, and be LOUD when there are none left. #237's SVM pool
 * clamped silently, so two guests quietly shared one control block and it presented
 * on real hardware as a dashboard freeze with no panic -- nothing downstream can
 * detect a shared VMCS, so it has to be said here.
 */
static unsigned vmx_alloc_slot(void) {
    unsigned slot = __atomic_fetch_add(&g_vmx_vcpu_count, 1u, __ATOMIC_SEQ_CST);
    if (slot < HYPE_VMX_MAX_VCPUS) {
        return slot;
    }
    hype_debug_print("vmx: vCPU slot pool EXHAUSTED (%u slots) -- slot %u aliased to %u. Safe ONLY "
                     "if these guests never run concurrently (see #237/#245)\n",
                     HYPE_VMX_MAX_VCPUS, slot, HYPE_VMX_MAX_VCPUS - 1u);
    return HYPE_VMX_MAX_VCPUS - 1u;
}

/*
 * #271: hand every slot back. The pool is sized for the concurrent guests; the
 * battery runs strictly earlier and strictly sequentially, so resetting once it
 * finishes restores the invariant the pool was designed around. Mirrors
 * hype_svm_vcpu_pool_reset(). Not for use while any vCPU is live.
 */
void hype_vmx_vcpu_pool_reset(void) {
    __atomic_store_n(&g_vmx_vcpu_count, 0u, __ATOMIC_SEQ_CST);
}

/*
 * #276: which pool slot a ctx came from, so the MSR handler reaches THIS guest's
 * MSR area rather than a fixed one. The ctx is always &g_vmx_ctx_pool[slot], so
 * the index is recoverable by pointer arithmetic; anything outside the pool
 * (never expected) falls back to slot 0 rather than indexing out of bounds.
 */
static unsigned vmx_ctx_slot(const struct hype_vcpu_ctx *ctx) {
    if (ctx >= &g_vmx_ctx_pool[0] && ctx < &g_vmx_ctx_pool[HYPE_VMX_MAX_VCPUS]) {
        return (unsigned)(ctx - &g_vmx_ctx_pool[0]);
    }
    return 0;
}

/* Clear the per-entry pending state a fresh vCPU must not inherit. */
static void vmx_ctx_reset_pending(struct hype_vcpu_ctx *ctx) {
    unsigned i;
    for (i = 0; i < 8u; i++) {
        ctx->pending_irr[i] = 0;
    }
    ctx->pvclock_map = 0;
    /* A slot reused by a later guest must not inherit a prior guest's armed
     * pvclock pages -- same reasoning as the SVM path's reset. */
    ctx->pvclock_system_msr = 0;
    ctx->pvclock_wall_msr = 0;
    /* #277: a recycled slot must not inherit the previous guest's XCR0 -- the
     * architectural reset value is x87-only, which `valid == 0` stands for. */
    ctx->guest_xcr0 = 0;
    ctx->guest_xcr0_valid = 0;
}

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

/*
 * INVVPID, single-context (#273): drop every linear and combined mapping tagged
 * with `vpid`. Issued when a pool slot is handed to a new guest -- the slot's
 * VPID is stable, so without this a fresh guest could inherit its predecessor's
 * translations. The descriptor is 128 bits: VPID in bits 15:0, bits 127:64 a
 * linear address the single-context type ignores.
 */
static inline int invvpid_single_context(uint16_t vpid) {
    struct {
        uint64_t vpid_and_reserved;
        uint64_t linear_address;
    } desc;
    uint8_t fail_zf, fail_cf;

    /* Field-by-field, never whole-struct assignment: this is a freestanding
     * build with no libc, and an aggregate copy emits a memcpy call that fails
     * to link (see AGENTS.md). */
    desc.vpid_and_reserved = (uint64_t)vpid;
    desc.linear_address = 0;

    __asm__ volatile("invvpid %2, %3\n\t"
                      "setz %0\n\t"
                      "setc %1"
                      : "=q"(fail_zf), "=q"(fail_cf)
                      : "m"(desc), "r"(HYPE_VMX_INVVPID_SINGLE_CONTEXT)
                      : "cc", "memory");
    return (fail_zf || fail_cf) ? -1 : 0;
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
                              int long_mode, uint64_t guest_cr3, uint8_t *vmcs_region,
                              unsigned slot) {
    int rc = 0;
    /* #276: ONE index drives every per-guest resource -- VMCS region, MSR areas,
     * VPID. Deriving them separately is how the MSR areas stayed shared while the
     * VMCS pool looked done. */
    uint16_t vpid = hype_vmx_vpid_for_slot(slot, HYPE_VMX_MAX_VCPUS);
    /* #271: the region is passed in, not taken from a "current slot" global -- two
     * APs can be building their own vCPUs at the same time, and a shared global
     * would be the very race this pool exists to remove. VMPTRLD makes it current
     * for THIS logical CPU, which is per-core state, so each AP lands on its own.
     *
     * Pass `vmcs_region` to vmclear/vmptrld, NOT its address: those helpers bind
     * "=m" to the PARAMETER, so the pointer's VALUE is the operand the instruction
     * reads as the VMCS address. Passing &local adds a level of indirection and the
     * CPU treats the stack slot's address as the VMCS -- VMCS build then fails and
     * vcpu_create panics. */
    for (unsigned i = 0; i < 4096u; i++) {
        vmcs_region[i] = 0;
    }

    uint64_t vmx_basic = rdmsr(HYPE_MSR_IA32_VMX_BASIC);
    uint32_t revision_id = (uint32_t)(vmx_basic & 0x7FFFFFFFu);
    *(uint32_t *)vmcs_region = revision_id;

    if (vmclear(vmcs_region) != 0) {
        return -1;
    }
    if (vmptrld(vmcs_region) != 0) {
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
    uint32_t proc_ctls = hype_vmx_adjust_controls(HYPE_VMX_PROCBASED_ACTIVATE_SECONDARY_CONTROLS |
                                                      HYPE_VMX_PROCBASED_HLT_EXITING |
                                                      HYPE_VMX_PROCBASED_UNCOND_IO_EXITING,
                                                  proc_cap);
    /* Unrestricted guest (lets the guest run with CR0.PE=0 / CR0.PG=0, i.e.
     * real mode) architecturally REQUIRES EPT -- so both bits go together. */
    /* #273: request VPID only when this CPU can also invalidate it, and only for
     * a non-zero VPID -- ENABLE_VPID with VPID 0000H fails VM entry outright. */
    int want_vpid = (vpid != 0u) && hype_vmx_vpid_usable(rdmsr(HYPE_MSR_IA32_VMX_EPT_VPID_CAP));
    uint32_t proc2_ctls = hype_vmx_adjust_controls(
        HYPE_VMX_PROCBASED2_ENABLE_EPT | HYPE_VMX_PROCBASED2_UNRESTRICTED_GUEST |
            HYPE_VMX_PROCBASED2_ENABLE_INVPCID | HYPE_VMX_PROCBASED2_ENABLE_RDTSCP |
            (want_vpid ? HYPE_VMX_PROCBASED2_ENABLE_VPID : 0u),
        proc2_cap);
    /* Read back what adjust_controls() actually granted rather than what was
     * asked for: if the capability MSR forbids VPID the bit is gone, and writing
     * a VPID (or issuing INVVPID) on that basis would be reasoning from a
     * request instead of from the machine. Same discipline as ack-intr-on-exit. */
    int vpid_enabled = (proc2_ctls & HYPE_VMX_PROCBASED2_ENABLE_VPID) != 0u;
    /* Host address-space size MUST be set: hype's host is 64-bit (see the
     * constant's comment) -- omitting it is the classic error-7 VM-entry
     * failure. Entry controls stay 0 (real-mode guest, not IA-32e). */
    /* #248: also request acknowledge-interrupt-on-exit, so the CPU performs the
     * interrupt-acknowledge cycle itself and reports the vector in
     * VM_EXIT_INTR_INFO. adjust_controls() drops it if this CPU (or the L0
     * hypervisor, when nested) does not support it -- hence the read-back below
     * rather than assuming it took. */
    uint32_t exit_ctls = hype_vmx_adjust_controls(
        HYPE_VMX_EXIT_HOST_ADDR_SPACE_SIZE | HYPE_VMX_EXIT_ACK_INTR_ON_EXIT |
            HYPE_VMX_EXIT_SAVE_IA32_EFER | HYPE_VMX_EXIT_LOAD_IA32_EFER |
            HYPE_VMX_EXIT_SAVE_IA32_PAT | HYPE_VMX_EXIT_LOAD_IA32_PAT,
        exit_cap);
    g_vmx_ack_intr_on_exit = (exit_ctls & HYPE_VMX_EXIT_ACK_INTR_ON_EXIT) != 0u;
    /* Say which interrupt-consumption path is live. Without this the two are
     * indistinguishable in a log -- both stop the storm -- and there would be no
     * way to tell whether the CPU (or L0, when nested) granted the control or
     * quietly dropped it, leaving the dispatch path as dead code. */
    hype_debug_print("vmx: ack-intr-on-exit=%s (exit_ctls=0x%x) -- interrupts consumed by %s\n",
                     g_vmx_ack_intr_on_exit ? "yes" : "no", (unsigned int)exit_ctls,
                     g_vmx_ack_intr_on_exit ? "explicit dispatch" : "STI window");
    /*
     * IA-32e-mode-guest depends on the mode the guest is in; load-IA32_EFER does
     * NOT and is now requested unconditionally (#248).
     *
     * The old "a real-mode guest needs neither" was wrong in a way that only a
     * guest which CHANGES mode could expose. Without load-IA32_EFER the CPU never
     * loads EFER from GUEST_IA32_EFER on entry, so the guest runs on the HOST's
     * EFER -- and hype's host is 64-bit, i.e. EFER.LMA=1. The guest was therefore
     * executing with LMA set while its own CR0.PG was 0, an architecturally
     * impossible pair, and OVMF's long-mode trampoline took #GP the moment it
     * tried to set CR0.PG. hype's WRMSR handler had been dutifully recording the
     * guest's EFER writes into GUEST_IA32_EFER all along; nothing was loading
     * them.
     *
     * IA-32e-mode-guest stays mode-dependent and is kept in step from then on by
     * vmx_sync_long_mode() on every CR0.PG or EFER.LME change.
     */
    uint32_t entry_desired = HYPE_VMX_ENTRY_LOAD_IA32_EFER | HYPE_VMX_ENTRY_LOAD_IA32_PAT |
                             (long_mode ? HYPE_VMX_ENTRY_IA32E_MODE_GUEST : 0u);
    uint32_t entry_ctls = hype_vmx_adjust_controls(entry_desired, entry_cap);
    g_vmx_entry_cap = entry_cap; /* #248: for vmx_sync_long_mode()'s re-adjust */

    rc |= vmwrite(HYPE_VMCS_PIN_BASED_VM_EXEC_CONTROL, pin_ctls);
    rc |= vmwrite(HYPE_VMCS_CPU_BASED_VM_EXEC_CONTROL, proc_ctls);
    rc |= vmwrite(HYPE_VMCS_SECONDARY_VM_EXEC_CONTROL, proc2_ctls);
    rc |= vmwrite(HYPE_VMCS_VM_EXIT_CONTROLS, exit_ctls);
    rc |= vmwrite(HYPE_VMCS_VM_ENTRY_CONTROLS, entry_ctls);
    rc |= vmwrite(HYPE_VMCS_EXCEPTION_BITMAP, 0);

    /*
     * #273: tag this guest, then flush the tag. The flush matters because the
     * VPID comes from a POOL SLOT, so a slot recycled after
     * hype_vmx_vcpu_pool_reset() hands the new guest its predecessor's VPID; the
     * automatic every-entry flush that made VPID 0 safe is exactly what enabling
     * this control gives up.
     */
    /*
     * Record the EFFECTIVE tag for this slot -- the value the hardware will use,
     * which is 0 when the CPU would not let us enable VPID at all. Written via
     * the slot index (the ctx pool is indexed by it) rather than through a
     * "current vCPU" global, so two APs building their own vCPUs concurrently
     * cannot clobber each other -- the same reasoning that made vmcs_region a
     * parameter here.
     */
    g_vmx_ctx_pool[slot].vpid = vpid_enabled ? vpid : 0u;
    if (vpid_enabled) {
        rc |= vmwrite(HYPE_VMCS_VIRTUAL_PROCESSOR_ID, vpid);
        if (invvpid_single_context(vpid) != 0) {
            /* The capability MSR said this type exists, so a failure here means
             * the CPU and its own capability report disagree -- refuse to launch
             * rather than run a guest on translations that may not be its own. */
            hype_debug_print("vmx: INVVPID(single, vpid=%u) FAILED despite EPT_VPID_CAP "
                             "advertising it -- refusing to launch (#273)\n",
                             (unsigned int)vpid);
            return -1;
        }
    }
    /* Printed per vCPU on purpose: #274 needs to show two guests carrying two
     * DIFFERENT VPIDs, and "we asked for one" is not evidence of that. */
    hype_debug_print("vmx: vpid=%u enabled=%s (proc2=0x%x)\n", (unsigned int)vpid,
                     vpid_enabled ? "yes" : "no", (unsigned int)proc2_ctls);

    /* EPT pointer (M2-8/M3-1): required now that ENABLE_EPT is set. Caller
     * passes the fully-formed EPTP (PML4 phys | memtype WB | walk-length-1 |
     * flags -- see hype_vmx_make_eptp()). */
    rc |= vmwrite(HYPE_VMCS_EPT_POINTER, eptp);

    /* TPR shadow/APICv (M2-4): only takes effect if the capability
     * negotiation above actually granted USE_TPR_SHADOW (older CPUs
     * without it will simply ignore VIRTUAL_APIC_PAGE_ADDR/
     * TPR_THRESHOLD). 0 threshold = no TPR-masking VM-exits. */
    rc |= vmwrite(HYPE_VMCS_VIRTUAL_APIC_PAGE_ADDR, (uint64_t)(uintptr_t)g_virtual_apic_page[slot]);
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
    /* #248: write GUEST_IA32_EFER in BOTH cases now that load-IA32_EFER is always
     * on. A real-mode guest must start from a clean EFER=0 -- leaving the field
     * unwritten would have the guest inherit whatever it held, and the whole
     * point of this change is that the guest no longer runs on the host's EFER. */
    rc |= vmwrite(HYPE_VMCS_GUEST_IA32_EFER,
                  long_mode ? (HYPE_VMX_EFER_LME | HYPE_VMX_EFER_LMA) : 0ull);
    /* Source for the exit-side restore. Read the live host value rather than
     * synthesising one: hype's own EFER carries NXE/SCE that its page tables and
     * syscall path depend on. */
    rc |= vmwrite(HYPE_VMCS_HOST_IA32_EFER, rdmsr(HYPE_MSR_IA32_EFER));
    /* #251: start the guest with a cacheable PAT rather than 0, and restore hype's
     * own on exit -- PAT is not context-switched for us the way SVM's VMSAVE
     * handles other MSRs. */
    rc |= vmwrite(HYPE_VMCS_GUEST_IA32_PAT, HYPE_VMX_PAT_RESET_VALUE);
    rc |= vmwrite(HYPE_VMCS_HOST_IA32_PAT, rdmsr(HYPE_MSR_IA32_PAT));
    /* #251: remember hype's own XCR0 so an XSETBV-ing guest can be swapped in and
     * out around VM entry. Guarded on CR4.OSXSAVE -- XGETBV faults without it. */
    if (vmx_ensure_osxsave()) {
        g_vmx_host_xcr0 = xgetbv0();
    }

    /*
     * #251 slice 2: populate and wire the MSR areas.
     *
     * The HOST side is snapshotted from the live MSRs so a VM exit restores hype's
     * own per-CPU base and SYSCALL targets. The GUEST side starts at 0 -- a fresh
     * guest has no per-CPU area or syscall handlers, and 0 is what real hardware
     * presents after reset.
     *
     * Without this, IA32_KERNEL_GS_BASE simply is not virtualised: the guest and
     * hype share the physical MSR, so a guest SWAPGS installs whatever hype last
     * put there (measured: 0). A Linux guest's early per-CPU access then reads
     * through a zero base and faults before its IDT exists -- which is how it ends
     * up parked in `hlt; jmp` with no console output.
     */
    {
        unsigned i;
        for (i = 0; i < HYPE_VMX_MSR_AREA_COUNT; i++) {
            g_vmx_msr_host[slot][i].index = g_vmx_msr_list[i];
            g_vmx_msr_host[slot][i].reserved = 0;
            g_vmx_msr_host[slot][i].value = rdmsr(g_vmx_msr_list[i]);
            g_vmx_msr_guest[slot][i].index = g_vmx_msr_list[i];
            g_vmx_msr_guest[slot][i].reserved = 0;
            g_vmx_msr_guest[slot][i].value = 0;
        }
    }
    rc |= vmwrite(HYPE_VMCS_VM_ENTRY_MSR_LOAD_ADDR, (uint64_t)(uintptr_t)g_vmx_msr_guest[slot]);
    rc |= vmwrite(HYPE_VMCS_VM_ENTRY_MSR_LOAD_COUNT, (uint64_t)HYPE_VMX_MSR_AREA_COUNT);
    /* Same area as the entry list on purpose -- see the declaration: this is what
     * lets a SWAPGS-driven change survive, since that instruction never exits. */
    rc |= vmwrite(HYPE_VMCS_VM_EXIT_MSR_STORE_ADDR, (uint64_t)(uintptr_t)g_vmx_msr_guest[slot]);
    rc |= vmwrite(HYPE_VMCS_VM_EXIT_MSR_STORE_COUNT, (uint64_t)HYPE_VMX_MSR_AREA_COUNT);
    rc |= vmwrite(HYPE_VMCS_VM_EXIT_MSR_LOAD_ADDR, (uint64_t)(uintptr_t)g_vmx_msr_host[slot]);
    rc |= vmwrite(HYPE_VMCS_VM_EXIT_MSR_LOAD_COUNT, (uint64_t)HYPE_VMX_MSR_AREA_COUNT);
    /*
     * #248: the host must OWN the fixed bits, not merely set them once here.
     * Satisfying the fixed-bit MSRs for the initial VMCS is not enough -- the
     * guest goes on to write these registers itself. Real firmware does not know
     * it is virtualised, so OVMF's reset vector writes CR4=0x640
     * (MCE|OSFXSR|OSXMMEXCPT) with VMXE clear; with a mask of 0 that value
     * reached GUEST_CR4 directly, violated CR4_FIXED0's VMXE requirement and
     * raised #GP(0) on the very first CR4 load.
     *
     * Owning the bit routes guest writes through a CR-access VM exit
     * (hype_vmx_vcpu_handle_cr_access), which re-adds the required bit to
     * GUEST_CR* and records what the guest *thinks* it wrote in the read
     * shadow. The guest reads back its own value, exactly as on real hardware.
     *
     * The read shadows therefore start WITHOUT the host-owned bits: a guest that
     * reads CR4 here must not see VMXE, or it would conclude the CPU is already
     * in VMX operation.
     *
     * CR0.PG is owned too, for a different reason from CR0.NE: not because the
     * hardware requires a value, but because hype needs to SEE the guest change
     * it. Enabling paging with EFER.LME set is the long-mode transition, and both
     * EFER.LMA and the IA-32e-mode-guest entry control have to move with it
     * (#248). An unowned PG loads silently and hype would never know. PG is NOT
     * masked out of the read shadow below -- unlike VMXE and NE it is the guest's
     * own bit, so it must read back exactly as the guest set it.
     */
    rc |= vmwrite(HYPE_VMCS_CR0_GUEST_HOST_MASK, HYPE_VMX_CR0_NE | HYPE_VMX_CR0_PG);
    rc |= vmwrite(HYPE_VMCS_CR4_GUEST_HOST_MASK, HYPE_VMX_CR4_VMXE);
    rc |= vmwrite(HYPE_VMCS_CR0_READ_SHADOW, guest_cr0 & ~HYPE_VMX_CR0_NE);
    rc |= vmwrite(HYPE_VMCS_CR4_READ_SHADOW, guest_cr4 & ~HYPE_VMX_CR4_VMXE);
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
    rc |= vmwrite(HYPE_VMCS_HOST_RSP, (uint64_t)&vmcs_region[4096]);
    /* HOST_RIP/HOST_RSP are placeholders here: hype_vmx_vcpu_run()'s trampoline
     * (vmx_run.S) VMWRITEs the real values (its own .Lvmexit label + live stack)
     * on every entry, overriding these. The stub only keeps the field non-zero
     * for a build that never launches (M2-6 struct validation). */
    rc |= vmwrite(HYPE_VMCS_HOST_RIP, (uint64_t)&hype_vmx_host_exit_stub);

    return rc;
}

/* Public builders: real-mode guest at cs_base:rip; flat 64-bit guest at linear
 * entry_rip with paging root guest_cr3. Both take a prebuilt EPT pointer. */
int hype_vmx_vmcs_build_guest(uint64_t cs_base, uint64_t rip, uint64_t stack_phys, uint64_t eptp,
                              uint8_t *vmcs_region, unsigned slot) {
    return build_guest_common(cs_base, rip, stack_phys, eptp, 0, 0, vmcs_region, slot);
}

int hype_vmx_vmcs_build_long_mode_guest(uint64_t entry_rip, uint64_t guest_cr3, uint64_t stack_phys,
                                        uint64_t eptp, uint8_t *vmcs_region, unsigned slot) {
    return build_guest_common(0, entry_rip, stack_phys, eptp, 1, guest_cr3, vmcs_region, slot);
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
    unsigned slot = vmx_alloc_slot(); /* #271 */
    struct hype_vcpu_ctx *ctx = &g_vmx_ctx_pool[slot];
    uint64_t eptp;
    unsigned i;

    /*
     * VMX-4 (#236): a NON-ZERO ept_or_npt_root is the caller's own EPT PML4
     * physical address, used verbatim. That is how the FW-1 live guest gets a
     * non-identity address space -- its RAM sits wherever UEFI allocated it but
     * must appear at guest-physical 0, which an identity EPT cannot express.
     * Zero keeps the built-in identity EPT, which is what the microtests want
     * (their guests genuinely are identity-mapped).
     *
     * The parameter is named for SVM's NPT root because it is a shared vtable
     * slot; on VMX the value must be an EPT PML4, never an NPT one -- the two
     * encodings are not interchangeable, so the caller picks per backend.
     */
    if (ept_or_npt_root != 0) {
        eptp = hype_vmx_make_eptp(ept_or_npt_root);
    } else {
        hype_ept_build_identity(g_ept_pml4, g_ept_pdpt, g_ept_pd, HYPE_EPT_MAX_GB);
        eptp = hype_vmx_make_eptp((uint64_t)(uintptr_t)g_ept_pml4);
    }

    /* cs_base = guest_rip, rip = 0: the guest starts executing at physical
     * guest_rip in real mode (CS.base:IP = guest_rip:0). */
    if (hype_vmx_vmcs_build_guest(guest_rip, 0, guest_rsp, eptp, g_vmcs_pool[slot], slot) != 0) {
        return 0;
    }

    for (i = 0; i < 16; i++) {
        ctx->gprs[i] = 0;
    }
    /* #260: architectural reset image, not zeros -- a zeroed FXSAVE image sets
     * MXCSR=0, which unmasks every SIMD exception. */
    hype_fpu_area_reset(&ctx->fpu);
    ctx->launched = 0;
    vmx_ctx_reset_pending(ctx);
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
    unsigned slot = vmx_alloc_slot(); /* #271 */
    struct hype_vcpu_ctx *ctx = &g_vmx_ctx_pool[slot];
    uint64_t eptp;
    unsigned i;

    (void)ept_or_npt_root;

    hype_ept_build_identity(g_ept_pml4, g_ept_pdpt, g_ept_pd, HYPE_EPT_MAX_GB);
    eptp = hype_vmx_make_eptp((uint64_t)(uintptr_t)g_ept_pml4);

    if (hype_vmx_vmcs_build_long_mode_guest(entry_rip, guest_cr3, guest_rsp, eptp,
                                            g_vmcs_pool[slot], slot) != 0) {
        return 0;
    }

    for (i = 0; i < 16; i++) {
        ctx->gprs[i] = 0;
    }
    ctx->launched = 0;
    vmx_ctx_reset_pending(ctx);
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
/*
 * #248: hand a pending external interrupt to hype's own IDT.
 *
 * On VM exit the CPU forces host RFLAGS to 0x2, so IF=0 and the interrupt that
 * caused the exit stays PENDING. Without "acknowledge interrupt on exit" set in
 * the VM-exit controls, nothing acknowledges it either -- so re-entering the
 * guest exits again immediately on the same interrupt, forever. That is exactly
 * what happened on Intel: EXHIST total=13813772 with intr=13813771 and guest RIP
 * pinned. This is the VMX counterpart of the SVM path's post-VMRUN stgi(), which
 * lets hype_timer_isr run with host IF=1.
 *
 * The NOP is load-bearing. STI's effect is delayed by one instruction (its
 * interrupt shadow), so `sti; cli` back-to-back would re-mask before any
 * interrupt could ever be delivered -- a window that looks open and never is.
 */
static inline void vmx_take_pending_host_interrupt(void) {
    __asm__ volatile("sti\n\tnop\n\tcli" ::: "memory");
}

/*
 * #248: the acknowledge-interrupt-on-exit route. When that VM-exit control is
 * active the CPU has already acknowledged the interrupt by the time we get here,
 * so it is NOT pending any more and the STI window above would find nothing to
 * deliver -- hype must call the handler itself, using the vector the CPU recorded
 * in VM_EXIT_INTR_INFO (valid only when bit 31 is set).
 *
 * An unregistered vector is not an error: the hardware acknowledged an interrupt
 * hype has no handler for, and the correct response is to carry on rather than
 * panic. It is logged nowhere on purpose -- this runs on every host tick.
 */
static void vmx_dispatch_acked_interrupt(void) {
    int ok;
    uint64_t intr_info = vmread(HYPE_VMCS_VM_EXIT_INTR_INFO, &ok);

    if (!ok || (intr_info & (1ull << 31)) == 0ull) {
        return;
    }
    (void)hype_isr_dispatch_vector((uint8_t)(intr_info & 0xFFull));
}


/*
 * #273: the VPID this vCPU actually runs under (0 = none, every entry flushes).
 * See hype_vmm_ops_t.vcpu_tlb_tag for why this is a vtable entry.
 */
uint32_t hype_vmx_vcpu_tlb_tag(hype_vcpu_ctx_t *ctx) {
    const struct hype_vcpu_ctx *real = (const struct hype_vcpu_ctx *)ctx;
    if (real == 0) {
        return 0u;
    }
    return (uint32_t)real->vpid;
}

int hype_vmx_vcpu_run(hype_vcpu_ctx_t *ctx, hype_vmexit_info_t *info) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    uint64_t failed;
    int ok;

    /* #251: run the guest under its own XCR0, and put hype's back afterwards. */
    if (real->guest_xcr0_valid && vmx_ensure_osxsave()) {
        xsetbv0(real->guest_xcr0);
    }
    /* #260: restore AFTER the XCR0 switch (XSETBV can reinitialise state
     * components, discarding whatever we had just loaded) and save BEFORE
     * switching XCR0 back, for the same reason. Nothing between the restore and
     * the launch may touch vector registers. */
    hype_fpu_restore(&real->fpu);
    failed = hype_vmx_launch(ctx, (uint64_t)real->launched);
    hype_fpu_save(&real->fpu);
    if (real->guest_xcr0_valid && g_vmx_host_xcr0 != 0ull) {
        xsetbv0(g_vmx_host_xcr0);
    }
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

    /*
     * #248: consume the interrupt that caused this exit before the caller can
     * resume the guest, or it re-exits on the same one indefinitely. Placed here
     * rather than in the FW-1 loop so every VMX guest benefits, matching where
     * the SVM backend does its stgi().
     *
     * Which mechanism applies is decided by whether the CPU actually granted
     * acknowledge-interrupt-on-exit, not by preference: if it did, the interrupt
     * is already acknowledged and only an explicit dispatch can run its handler;
     * if it did not, the interrupt is still pending and only opening an interrupt
     * window can deliver it. Doing the wrong one silently loses host timer ticks.
     */
    if (info->reason == HYPE_VMX_EXIT_REASON_EXTERNAL_INTERRUPT) {
        if (g_vmx_ack_intr_on_exit) {
            vmx_dispatch_acked_interrupt();
        } else {
            vmx_take_pending_host_interrupt();
        }
    }
    return 0;
}

/* Advance guest RIP past the instruction that caused the exit, using the exact
 * length the CPU recorded (VM_EXIT_INSTRUCTION_LEN) -- the VMX analogue of
 * SVM's "rip += 2" for CPUID/RDMSR/WRMSR (all coincidentally 2 bytes, but the
 * VMCS field is authoritative and works for any emulated instruction). */
/* Host TSC. The ACPI PM timer scales from this, as SVM's real_rdtsc() does. */
static inline uint64_t vmx_real_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

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
/*
 * #251: ring of the most recent CPUIDs, dumped when the guest is detected idle.
 *
 * A flat cap like the MSR trace is useless here -- the guest issues ~35k CPUID
 * exits, so the first 48 are all firmware. What matters is the LAST few before it
 * parks: the transition ring shows it enumerating CPUID at one RIP and then
 * halting ~0x4a bytes later, which is the shape of a feature check that fails.
 * hype_cpuid_emulate() is shared between backends but starts from the REAL host
 * CPUID, so what hype hands back genuinely differs on Intel.
 */
#define HYPE_VMX_CPUID_RING 12u
static struct {
    uint32_t eax_in, ecx_in, eax, ebx, ecx, edx;
    uint64_t rip;
    uint64_t xcr0; /* #252: XCR0 IN FORCE for this CPUID -- leaf 0xD EBX depends on it,
                    * so a reading is meaningless without the context it was taken in.
                    * Pairing them by eye from the WARNING's registers produced two
                    * wrong root causes; record it instead of inferring it. */
} g_vmx_cpuid_ring[HYPE_VMX_CPUID_RING];
static unsigned g_vmx_cpuid_ring_head = 0;
static unsigned g_vmx_cpuid_ring_n = 0;

/*
 * #251: emulate XSETBV for the guest.
 *
 * The guest's requested XCR0 is masked to what the host actually supports (CPUID
 * leaf 0xD, EDX:EAX) rather than passed through blindly: XSETBV with an
 * unsupported bit raises #GP, and faulting the guest for asking is worse than
 * giving it the intersection. In practice they are equal, because hype passes the
 * host's XSAVE CPUID through, so the guest only ever asks for what exists -- a
 * divergence is logged rather than silently narrowed.
 *
 * Returns 0 if emulated. Only XCR0 (ECX=0) is defined; anything else is left
 * unhandled for the caller to report rather than quietly skipped.
 */
int hype_vmx_vcpu_handle_xsetbv(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    uint64_t requested, supported;
    hype_cpuid_result_t xs;

    if ((uint32_t)real->gprs[1] != 0u) { /* ECX: only XCR0 exists */
        return -1;
    }
    if (!vmx_ensure_osxsave()) {
        /* Cannot touch XCR0 at all on this host -- report unhandled rather than
         * faulting hype, and leave RIP unadvanced so the caller says so. */
        return -1;
    }
    requested = ((uint64_t)(uint32_t)real->gprs[2] << 32) | (uint64_t)(uint32_t)real->gprs[0];

    vmx_real_cpuid(0xDu, 0u, &xs);
    supported = ((uint64_t)xs.edx << 32) | (uint64_t)xs.eax;
    if ((requested & ~supported) != 0ull) {
        hype_debug_print("vmx XSETBV: guest asked for 0x%llx, host supports 0x%llx -- masking\n",
                         (unsigned long long)requested, (unsigned long long)supported);
    }
    /* XCR0.x87 (bit 0) must be set; a value clearing it would #GP. */
    real->guest_xcr0 = (requested & supported) | 1ull;
    real->guest_xcr0_valid = 1;
    xsetbv0(real->guest_xcr0);
    vmx_advance_rip();
    return 0;
}

void hype_vmx_vcpu_dump_cpuid_ring(void) {
    unsigned i;
    for (i = 0; i < g_vmx_cpuid_ring_n; i++) {
        unsigned idx =
            (g_vmx_cpuid_ring_head + HYPE_VMX_CPUID_RING - g_vmx_cpuid_ring_n + i) %
            HYPE_VMX_CPUID_RING;
        hype_debug_print("vmx CPUIDRING#%02u: leaf=0x%x sub=0x%x -> eax=0x%x ebx=0x%x ecx=0x%x "
                         "edx=0x%x xcr0=0x%llx rip=0x%llx\n",
                         i, (unsigned int)g_vmx_cpuid_ring[idx].eax_in,
                         (unsigned int)g_vmx_cpuid_ring[idx].ecx_in,
                         (unsigned int)g_vmx_cpuid_ring[idx].eax,
                         (unsigned int)g_vmx_cpuid_ring[idx].ebx,
                         (unsigned int)g_vmx_cpuid_ring[idx].ecx,
                         (unsigned int)g_vmx_cpuid_ring[idx].edx,
                         (unsigned long long)g_vmx_cpuid_ring[idx].xcr0,
                         (unsigned long long)g_vmx_cpuid_ring[idx].rip);
    }
}

void hype_vmx_vcpu_handle_cpuid(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    uint32_t eax_in = (uint32_t)real->gprs[0];
    uint32_t ecx_in = (uint32_t)real->gprs[1];
    hype_cpuid_result_t host_real, out;

    /*
     * #251: CPUID leaf 0xD reports XSAVE area sizes FOR THE CURRENTLY ACTIVE XCR0
     * (EBX especially). hype runs the real CPUID in host context, where hype's own
     * XCR0 is loaded -- so a guest that has set its own XCR0 would be told a size
     * computed for the wrong feature set, and Linux's fpu__init_system_xstate()
     * cross-checks that size and dies when it disagrees. Load the guest's XCR0 for
     * the duration of this one CPUID so the answer describes the guest.
     */
    /*
     * #252: ...but the swap must ALSO happen before the guest's first XSETBV. It used
     * to be gated on the guest-XCR0-valid flag, so until then hype read the leaf under
     * the HOST's live XCR0 and handed the guest a size describing the host. The
     * guest's XCR0 before its first XSETBV is not "whatever the host has" -- it is
     * the architectural reset value, 1 (x87 only). Linux reads this leaf during
     * fpu__init_system_xstate BEFORE enabling anything, so that early read is exactly
     * the one that was wrong.
     */
    if (eax_in == 0xDu && g_vmx_host_xcr0 != 0ull) {
        uint64_t guest_xcr0 = real->guest_xcr0_valid ? real->guest_xcr0 : 1ull;
        xsetbv0(guest_xcr0);
        vmx_real_cpuid(eax_in, ecx_in, &host_real);
        xsetbv0(g_vmx_host_xcr0);
    } else {
        vmx_real_cpuid(eax_in, ecx_in, &host_real);
    }
    hype_cpuid_emulate(eax_in, ecx_in, &host_real, &out);

    real->gprs[0] = out.eax; /* RAX */
    real->gprs[3] = out.ebx; /* RBX */
    real->gprs[1] = out.ecx; /* RCX */
    real->gprs[2] = out.edx; /* RDX */
    {
        int ok2;
        unsigned h = g_vmx_cpuid_ring_head;
        g_vmx_cpuid_ring[h].eax_in = eax_in;
        g_vmx_cpuid_ring[h].ecx_in = ecx_in;
        g_vmx_cpuid_ring[h].eax = out.eax;
        g_vmx_cpuid_ring[h].ebx = out.ebx;
        g_vmx_cpuid_ring[h].ecx = out.ecx;
        g_vmx_cpuid_ring[h].edx = out.edx;
        g_vmx_cpuid_ring[h].xcr0 = real->guest_xcr0_valid ? real->guest_xcr0 : 1ull;
        g_vmx_cpuid_ring[h].rip = vmread(HYPE_VMCS_GUEST_RIP, &ok2);
        g_vmx_cpuid_ring_head = (h + 1u) % HYPE_VMX_CPUID_RING;
        if (g_vmx_cpuid_ring_n < HYPE_VMX_CPUID_RING) { g_vmx_cpuid_ring_n++; }
    }
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
    int area_slot = vmx_msr_area_slot(msr_number);

    /*
     * #251 slice 2: MSRs carried in the VM-entry/exit areas are serviced from that
     * same table, ahead of the action switch.
     *
     * They are deliberately absent from msr_emulate's action list: hardware loads
     * and stores them around every transition, and hype only sees the accesses at
     * all because there is no MSR bitmap yet, so every RDMSR/WRMSR exits. Reading
     * and writing the table keeps ONE source of truth -- letting these fall to the
     * absorb path instead would discard a guest write that the next entry-load
     * would then contradict, and satisfy a guest read with 0 while the hardware
     * held something else.
     */
    if (area_slot >= 0) {
        /* #276: this guest's area, not a shared one. */
        hype_vmx_msr_entry_t *area = g_vmx_msr_guest[vmx_ctx_slot(real)];
        if (is_write) {
            area[area_slot].value =
                ((uint64_t)(uint32_t)real->gprs[2] << 32) | (uint64_t)(uint32_t)real->gprs[0];
        } else {
            uint64_t v = area[area_slot].value;
            real->gprs[0] = (uint64_t)(uint32_t)v;
            real->gprs[2] = (uint64_t)(uint32_t)(v >> 32);
        }
        vmx_advance_rip();
        return 0;
    }

    /*
     * PVCLOCK (kvmclock) -- #251/#236.
     *
     * hype advertises the KVM pvclock feature in CPUID leaf 0x40000001, whose own
     * comment promises "the guest enables nothing hype doesn't back". That promise
     * was SVM-only: these two MSRs were special-cased in svm_vcpu.c and nowhere
     * else, so on VMX the guest armed kvmclock, the write fell through to the
     * absorb path, and the time-info page was never filled -- PVCLOCK arm_count=0,
     * a guest reading time from a page nothing writes.
     *
     * hype_vmx_vcpu_set_pvclock() already established the map and scale, so only
     * the arming half was missing. Handled ahead of the action switch for the same
     * reason SVM does it: these are not in msr_emulate's table.
     */
    if (msr_number == HYPE_MSR_KVM_SYSTEM_TIME_NEW || msr_number == HYPE_MSR_KVM_SYSTEM_TIME_OLD) {
        if (is_write) {
            real->pvclock_system_msr =
                ((uint64_t)(uint32_t)real->gprs[2] << 32) | (uint64_t)(uint32_t)real->gprs[0];
            vmx_pvclock_arm_system_time(real, real->pvclock_system_msr);
        } else {
            real->gprs[0] = (uint64_t)(uint32_t)real->pvclock_system_msr;
            real->gprs[2] = (uint64_t)(uint32_t)(real->pvclock_system_msr >> 32);
        }
        vmx_advance_rip();
        return 0;
    }
    if (msr_number == HYPE_MSR_KVM_WALL_CLOCK_NEW || msr_number == HYPE_MSR_KVM_WALL_CLOCK_OLD) {
        if (is_write) {
            real->pvclock_wall_msr =
                ((uint64_t)(uint32_t)real->gprs[2] << 32) | (uint64_t)(uint32_t)real->gprs[0];
            vmx_pvclock_arm_wall_clock(real, real->pvclock_wall_msr);
        } else {
            real->gprs[0] = (uint64_t)(uint32_t)real->pvclock_wall_msr;
            real->gprs[2] = (uint64_t)(uint32_t)(real->pvclock_wall_msr >> 32);
        }
        vmx_advance_rip();
        return 0;
    }

    /* #251: IA32_PAT into the VMCS field the CPU loads, mirroring what SVM does
     * with save.g_pat. Must stay intercepted rather than passed through: PAT is
     * not restored for hype automatically, and a guest write reaching the physical
     * MSR would corrupt the host's own page-attribute table. */
    if (msr_number == HYPE_MSR_IA32_PAT) {
        if (is_write) {
            vmwrite(HYPE_VMCS_GUEST_IA32_PAT,
                    ((uint64_t)(uint32_t)real->gprs[2] << 32) | (uint64_t)(uint32_t)real->gprs[0]);
        } else {
            uint64_t pat = vmread(HYPE_VMCS_GUEST_IA32_PAT, &ok);
            real->gprs[0] = (uint64_t)(uint32_t)pat;
            real->gprs[2] = (uint64_t)(uint32_t)(pat >> 32);
        }
        vmx_advance_rip();
        return 0;
    }

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
            /* #248: LME may just have changed. Long mode is CR0.PG && EFER.LME,
             * so the same recompute the CR0 path does is needed here -- the guest
             * sets LME first and PG second, and either order must leave the VMCS
             * self-consistent. Also re-derives LMA, so a guest writing LME|LMA by
             * hand cannot claim long mode before enabling paging. */
            vmx_sync_long_mode();
        } else {
            uint64_t efer = vmread(HYPE_VMCS_GUEST_IA32_EFER, &ok);
            real->gprs[0] = (uint64_t)(uint32_t)efer;
            real->gprs[2] = (uint64_t)(uint32_t)(efer >> 32);
        }
        break;
    /*
     * #251: apply the guest's FS/GS base to the VMCS field that actually takes
     * effect. Absorbing these writes is what left a long-mode guest faulting on
     * its first `MOV RAX, GS:[0x28]`.
     */
    case HYPE_MSR_ACTION_READWRITE_FS_BASE:
    case HYPE_MSR_ACTION_READWRITE_GS_BASE: {
        uint64_t field = (action == HYPE_MSR_ACTION_READWRITE_FS_BASE)
                             ? HYPE_VMCS_GUEST_FS_BASE
                             : HYPE_VMCS_GUEST_GS_BASE;
        if (is_write) {
            uint64_t value =
                ((uint64_t)(uint32_t)real->gprs[2] << 32) | (uint64_t)(uint32_t)real->gprs[0];
            vmwrite(field, value);
            /* A base is only usable if the segment itself is: hype set FS/GS up
             * for a real-mode guest and marked them unusable, and an access
             * through an unusable segment raises #GP(0) whatever the base says. */
            vmx_make_fs_gs_usable();
        } else {
            uint64_t base = vmread(field, &ok);
            real->gprs[0] = (uint64_t)(uint32_t)base;
            real->gprs[2] = (uint64_t)(uint32_t)(base >> 32);
        }
        break;
    }
    case HYPE_MSR_ACTION_READ_TSC: {
        uint64_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        real->gprs[0] = (uint64_t)(uint32_t)lo;
        real->gprs[2] = (uint64_t)(uint32_t)hi;
        break;
    }
    case HYPE_MSR_ACTION_REJECT:
    default:
        /*
         * Absorb the unmodelled MSR, matching the SVM path: WRMSR ignored,
         * RDMSR returns 0. This returned -1 (fatal) unconditionally, so the
         * first MSR a live Intel guest touched that hype does not model would
         * have killed it -- Linux reads IA32_BIOS_SIGN_ID (0x8b) during
         * microcode init, so that was reachable immediately. Reading 0 is what
         * a CPU without the feature would report.
         */
        /*
         * Name the MSRs being absorbed. "Absorb and continue" is the right
         * default -- it is what keeps a guest alive past MSRs hype does not model
         * -- but it is also silent, and a guest that reads a required MSR as 0 can
         * decide to give up. The Intel guest halts in a `hlt; jmp` loop a few
         * instructions after an RDMSR/WRMSR pair (visible in the transition ring),
         * with no console output to say why, so the MSR number is the missing
         * fact. SVM has had hype_svm_set_msr_trace() for this; VMX had nothing.
         *
         * Capped, and logs the WRMSR value too: for a write, what the guest was
         * trying to set is usually more informative than the register number.
         */
        {
            static unsigned msrtrace_n = 0;
            if (msrtrace_n < 48u) {
                msrtrace_n++;
                hype_debug_print("vmx MSRTRACE: %s msr=0x%x value=0x%llx rip=0x%llx (absorbed)\n",
                                 is_write ? "WRMSR" : "RDMSR", (unsigned int)msr_number,
                                 (unsigned long long)(((uint64_t)(uint32_t)real->gprs[2] << 32) |
                                                      (uint64_t)(uint32_t)real->gprs[0]),
                                 (unsigned long long)vmread(HYPE_VMCS_GUEST_RIP, &ok));
            }
        }
        if (!is_write) {
            /* Only on a READ: a WRMSR's RAX/RDX hold the value the guest is
             * writing, and zeroing them would corrupt guest state rather than
             * merely ignoring the write. */
            real->gprs[0] = 0; /* RAX */
            real->gprs[2] = 0; /* RDX */
        }
        break;
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
 * VMX MMIO handler for the PCI ECAM window (VMX-2): mirror of
 * hype_svm_vcpu_handle_pci_ecam_npf, driven by an EPT violation. Same
 * decode-at-RIP + RIP-advance shape as the pflash handler; here the faulting
 * GPA's offset into the ECAM window selects a PCI config address, dispatched
 * to hype_pci_config_read/write. Bounds-checked to [ecam_base, +BUS0_SIZE).
 */
int hype_vmx_vcpu_handle_pci_ecam_npf(hype_vcpu_ctx_t *ctx, hype_pci_t *pci,
                                      uint64_t ecam_base_phys) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_mmio_decode_t decoded;
    hype_pci_ecam_addr_t addr;
    uint64_t *reg;
    int ok;
    uint64_t gpa = vmread(HYPE_VMCS_GUEST_PHYSICAL_ADDRESS, &ok);
    uint64_t qual = vmread(HYPE_VMCS_EXIT_QUALIFICATION, &ok);
    uint64_t rip = vmread(HYPE_VMCS_GUEST_RIP, &ok);
    int is_write = (int)((qual >> 1) & 1u);

    if (gpa < ecam_base_phys || gpa >= ecam_base_phys + HYPE_PCI_ECAM_BUS0_SIZE) {
        return -1;
    }
    if (hype_mmio_decode((const uint8_t *)(uintptr_t)rip, HYPE_VMX_MMIO_MAX_INSTR_BYTES, &decoded) !=
        0) {
        return -1;
    }
    if (decoded.is_write != is_write) {
        return -1;
    }
    reg = vmx_gpr_ptr(real, decoded.reg);
    if (reg == 0) {
        return -1;
    }

    hype_pci_decode_ecam_offset(gpa - ecam_base_phys, &addr);
    if (decoded.is_write) {
        uint32_t value = hype_mmio_extract_write_value(*reg, decoded.size_bytes);
        hype_pci_config_write(pci, &addr, decoded.size_bytes, value);
    } else {
        uint32_t value = 0;
        hype_pci_config_read(pci, &addr, decoded.size_bytes, &value);
        *reg = hype_mmio_merge_read_value(*reg, value, decoded.size_bytes, decoded.zero_extend);
    }

    vmwrite(HYPE_VMCS_GUEST_RIP, rip + decoded.instr_len);
    return 0;
}

/*
 * VMX MMIO handler for the AHCI HBA (VMX-2): mirror of hype_svm_vcpu_handle_
 * ahci_npf. Same EPT-violation decode-at-RIP shape; on a write to PxCI (command
 * issue) it runs each issued slot through the shared, vendor-neutral
 * process_ahci_command_slot() (command-list/PRDT/FIS DMA -- dma_map 0 = identity
 * for the identity-mapped test guest). atapi carries the ATAPI/SCSI semantics.
 */
int hype_vmx_vcpu_handle_ahci_npf(hype_vcpu_ctx_t *ctx, hype_ahci_t *ahci, hype_atapi_t *atapi,
                                  uint64_t ahci_base_phys) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_mmio_decode_t decoded;
    uint64_t *reg;
    int ok;
    uint64_t gpa = vmread(HYPE_VMCS_GUEST_PHYSICAL_ADDRESS, &ok);
    uint64_t qual = vmread(HYPE_VMCS_EXIT_QUALIFICATION, &ok);
    uint64_t rip = vmread(HYPE_VMCS_GUEST_RIP, &ok);
    int is_write = (int)((qual >> 1) & 1u);
    uint32_t offset;

    if (gpa < ahci_base_phys || gpa >= ahci_base_phys + HYPE_AHCI_MMIO_SIZE) {
        return -1;
    }
    offset = (uint32_t)(gpa - ahci_base_phys);
    if (hype_mmio_decode((const uint8_t *)(uintptr_t)rip, HYPE_VMX_MMIO_MAX_INSTR_BYTES, &decoded) !=
        0) {
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
        if (hype_ahci_mmio_write(ahci, offset, decoded.size_bytes, value) != 0) {
            return -1;
        }
        if (offset == HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CI && ahci->p_ci != 0) {
            unsigned slot;
            for (slot = 0; slot < 32u; slot++) {
                if ((ahci->p_ci & (1u << slot)) != 0) {
                    if (process_ahci_command_slot(ahci, atapi, 0, slot) != 0) {
                        return -1;
                    }
                }
            }
        }
    } else {
        uint32_t value = 0;
        if (hype_ahci_mmio_read(ahci, offset, decoded.size_bytes, &value) != 0) {
            return -1;
        }
        *reg = hype_mmio_merge_read_value(*reg, value, decoded.size_bytes, decoded.zero_extend);
    }

    vmwrite(HYPE_VMCS_GUEST_RIP, rip + decoded.instr_len);
    return 0;
}

/*
 * VMX PS/2 keyboard IOIO handler (VMX-2, input-1): mirror of
 * hype_svm_vcpu_handle_ps2_kbd_ioio. Byte IN/OUT to the 0x60/0x64 ports routed
 * to the keyboard model; value in RAX (gprs[0] low byte).
 */
int hype_vmx_vcpu_handle_ps2_kbd_ioio(hype_vcpu_ctx_t *ctx, hype_ps2_kbd_t *kbd) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    int ok, rc;
    uint64_t qual = vmread(HYPE_VMCS_EXIT_QUALIFICATION, &ok);
    uint16_t port = (uint16_t)((qual >> 16) & 0xFFFFu);
    int is_in = (int)((qual >> 3) & 1u);

    if (is_in) {
        uint8_t value = 0;
        rc = hype_ps2_kbd_io_read(kbd, port, &value);
        if (rc == 0) {
            real->gprs[0] = (real->gprs[0] & ~0xFFULL) | value;
        }
    } else {
        rc = hype_ps2_kbd_io_write(kbd, port, (uint8_t)(real->gprs[0] & 0xFFu));
    }
    if (rc != 0) {
        return -1;
    }
    vmx_advance_rip();
    return 0;
}

/*
 * VMX PS/2 keyboard+mouse IOIO handler (VMX-2, input-2): mirror of
 * hype_svm_vcpu_handle_ps2_ioio. The 0x60 data port returns/consumes a mouse
 * byte when the mouse has one (or the aux-data-write flag is set), else the
 * keyboard; the 0x64 status port merges the mouse-pending bits into the kbd
 * status. out_kbd_wait (FW-1 idle detection) is unused by the microtest.
 */
int hype_vmx_vcpu_handle_ps2_ioio(hype_vcpu_ctx_t *ctx, hype_ps2_kbd_t *kbd, hype_ps2_mouse_t *mouse,
                                  int *out_kbd_wait) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    int ok;
    uint64_t qual = vmread(HYPE_VMCS_EXIT_QUALIFICATION, &ok);
    uint16_t port = (uint16_t)((qual >> 16) & 0xFFFFu);
    int is_in = (int)((qual >> 3) & 1u);

    if (out_kbd_wait != 0) {
        *out_kbd_wait = 0;
    }

    if (port == HYPE_PS2_PORT_DATA) {
        if (is_in) {
            uint8_t value;
            if (hype_ps2_mouse_has_pending_byte(mouse)) {
                value = hype_ps2_mouse_read_byte(mouse);
            } else {
                hype_ps2_kbd_io_read(kbd, HYPE_PS2_PORT_DATA, &value);
            }
            real->gprs[0] = (real->gprs[0] & ~0xFFULL) | value;
        } else {
            uint8_t value = (uint8_t)(real->gprs[0] & 0xFFu);
            if (hype_ps2_kbd_take_aux_data_write(kbd)) {
                hype_ps2_mouse_write_command(mouse, value);
            } else {
                hype_ps2_kbd_io_write(kbd, HYPE_PS2_PORT_DATA, value);
            }
        }
    } else if (port == HYPE_PS2_PORT_STATUS_COMMAND) {
        if (is_in) {
            uint8_t status = 0;
            hype_ps2_kbd_io_read(kbd, HYPE_PS2_PORT_STATUS_COMMAND, &status);
            if (hype_ps2_mouse_has_pending_byte(mouse)) {
                status |= HYPE_PS2_STATUS_OUTPUT_FULL | HYPE_PS2_STATUS_AUX_DATA;
            }
            if (out_kbd_wait != 0 && (status & HYPE_PS2_STATUS_OUTPUT_FULL) == 0) {
                *out_kbd_wait = 1;
            }
            real->gprs[0] = (real->gprs[0] & ~0xFFULL) | status;
        } else {
            hype_ps2_kbd_io_write(kbd, HYPE_PS2_PORT_STATUS_COMMAND, (uint8_t)(real->gprs[0] & 0xFFu));
        }
    } else {
        return -1;
    }

    vmx_advance_rip();
    return 0;
}

/*
 * VMX PIC IRQ delivery (VMX-2, input-1/2): mirror of
 * hype_svm_vcpu_deliver_pic_irq. Raises the line on the emulated PIC chip,
 * acknowledges the highest-priority pending IRQ, and queues that vector for
 * injection (via the interrupt-window path in hype_vmx_vcpu_request_interrupt).
 */
void hype_vmx_vcpu_deliver_pic_irq(hype_vcpu_ctx_t *ctx, hype_pic_emu_chip_t *chip, uint8_t irq) {
    uint8_t vector;
    hype_pic_emu_raise_irq(chip, irq);
    if (hype_pic_emu_acknowledge_highest_priority(chip, &vector)) {
        hype_vmx_vcpu_request_interrupt(ctx, vector);
    }
}

/* Guest-DMA address translation, VMX flavour: dma_map == 0 means the guest is
 * identity-mapped (guest-physical == host), the common case for the test guests;
 * otherwise bounds-check + translate via the map. Mirrors svm_vcpu.c's
 * guest_dma_xlate without exposing that static helper. */
static uint64_t vmx_dma_xlate(const hype_gpa_map_t *map, uint64_t gpa, uint64_t len) {
    if (map == 0) {
        return gpa;
    }
    return hype_gpa_to_host(map, gpa, len);
}

/*
 * VMX fw_cfg IOIO handler (VMX-2): mirror of hype_svm_vcpu_handle_fw_cfg_ioio,
 * DMA-interface subset (the M4-4 / video-2 test guests use the DMA path, not
 * the classic string-read port). 0x510 selects a key; 0x511 reads one data
 * byte; 0x514 latches the DMA address high dword; 0x518 latches the low dword,
 * reads the 16-byte DMA access struct from guest RAM, executes the transfer
 * against guest RAM, and writes the big-endian result back into the struct's
 * control field. Reuses the vendor-neutral hype_fw_cfg_dma_* helpers. String
 * INS/OUTS to fw_cfg is rejected (not needed by these tests).
 */
int hype_vmx_vcpu_handle_fw_cfg_ioio(hype_vcpu_ctx_t *ctx, hype_fw_cfg_t *fw,
                                     const hype_gpa_map_t *dma_map) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    int ok;
    uint64_t qual = vmread(HYPE_VMCS_EXIT_QUALIFICATION, &ok);
    uint16_t port = (uint16_t)((qual >> 16) & 0xFFFFu);
    int is_in = (int)((qual >> 3) & 1u);
    int is_string = (int)((qual >> 4) & 1u);
    uint64_t rax = real->gprs[0];

    if (is_string) {
        return -1;
    }

    if (port == 0x510u) {
        if (is_in) {
            return -1;
        }
        hype_fw_cfg_select(fw, (uint16_t)(rax & 0xFFFFu));
    } else if (port == 0x511u) {
        if (!is_in) {
            return -1;
        }
        real->gprs[0] = (rax & ~0xFFULL) | hype_fw_cfg_read_byte(fw);
    } else if (port == 0x514u) {
        if (is_in) {
            return -1;
        }
        hype_fw_cfg_dma_addr_high(fw, (uint32_t)(rax & 0xFFFFFFFFu));
    } else if (port == 0x518u) {
        uint64_t access_phys, access_host;
        uint8_t raw[16];
        hype_fw_cfg_dma_op_t op;
        uint8_t *control_bytes;
        uint32_t result;
        int i;

        if (is_in) {
            return -1;
        }
        access_phys = hype_fw_cfg_dma_addr_low(fw, (uint32_t)(rax & 0xFFFFFFFFu));
        access_host = vmx_dma_xlate(dma_map, access_phys, 16);
        if (access_host == 0) {
            return -1;
        }
        for (i = 0; i < 16; i++) {
            raw[i] = ((const uint8_t *)(uintptr_t)access_host)[i];
        }
        hype_fw_cfg_dma_decode(raw, &op);
        if (op.length != 0) {
            uint64_t data_host = vmx_dma_xlate(dma_map, op.address, op.length);
            result = (data_host == 0) ? HYPE_FW_CFG_DMA_CTL_ERROR
                                      : hype_fw_cfg_dma_execute(fw, &op, (uint8_t *)(uintptr_t)data_host);
        } else {
            result = hype_fw_cfg_dma_execute(fw, &op, 0);
        }
        control_bytes = (uint8_t *)(uintptr_t)access_host;
        control_bytes[0] = (uint8_t)(result >> 24);
        control_bytes[1] = (uint8_t)(result >> 16);
        control_bytes[2] = (uint8_t)(result >> 8);
        control_bytes[3] = (uint8_t)result;
    } else {
        return -1;
    }

    vmx_advance_rip();
    return 0;
}

/*
 * Common EPT-violation MMIO decode (VMX-2): reads the faulting GPA, write-bit,
 * and RIP; bounds-checks [base, base+size); decodes the instruction at RIP and
 * resolves the operand register. Fills the vmx_mmio_access fields on success
 * (0); returns -1 to reject. Factors the shared preamble out of the
 * device MMIO handlers below (the earlier pflash/ecam/ahci handlers predate it
 * and open-code the same steps).
 */
struct vmx_mmio_access {
    uint32_t offset;
    int is_write;
    uint64_t *reg;
    hype_mmio_decode_t decoded;
    uint64_t rip;
};
static int vmx_mmio_begin_insn(struct hype_vcpu_ctx *real, uint64_t base, uint64_t size,
                               const uint8_t *insn, struct vmx_mmio_access *m);

/*
 * EPT-violation preamble for a guest whose linear==physical==host (the
 * microtests): decode the faulting instruction straight out of guest memory at
 * GUEST_RIP. FW-1's live guest is NOT such a guest -- it remaps guest RAM and
 * the flash window away from identity -- so that path uses
 * vmx_mmio_begin_insn() with caller-resolved bytes instead (VMX-4, #236).
 */
static int vmx_mmio_begin(struct hype_vcpu_ctx *real, uint64_t base, uint64_t size,
                          struct vmx_mmio_access *m) {
    int ok;
    uint64_t rip = vmread(HYPE_VMCS_GUEST_RIP, &ok);
    return vmx_mmio_begin_insn(real, base, size, (const uint8_t *)(uintptr_t)rip, m);
}
static void vmx_mmio_end(struct vmx_mmio_access *m) {
    vmwrite(HYPE_VMCS_GUEST_RIP, m->rip + m->decoded.instr_len);
}

/* VMX MMIO handler for a SATA disk behind AHCI (VMX-2): mirror of
 * hype_svm_vcpu_handle_ahci_disk_npf. On a PxCI slot-0 write, runs the shared
 * process_ahci_ata_command_slot() (SATA command + guest-RAM DMA). */
int hype_vmx_vcpu_handle_ahci_disk_npf(hype_vcpu_ctx_t *ctx, hype_ahci_t *ahci,
                                       hype_ata_disk_t *disk, uint64_t ahci_base_phys) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    struct vmx_mmio_access m;
    if (vmx_mmio_begin(real, ahci_base_phys, HYPE_AHCI_MMIO_SIZE, &m) != 0) {
        return -1;
    }
    if (m.decoded.is_write) {
        uint32_t value = hype_mmio_extract_write_value(*m.reg, m.decoded.size_bytes);
        if (hype_ahci_mmio_write(ahci, m.offset, m.decoded.size_bytes, value) != 0) {
            return -1;
        }
        if (m.offset == HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CI && ahci->p_ci != 0) {
            /* 0 = identity: this is the non-map handler, used only by M5-2's
             * identity-mapped microtest. The FW-1 guest goes through the _map
             * variant, which passes its real DMA map. */
            unsigned slot;
            for (slot = 0; slot < 32u; slot++) {
                if ((ahci->p_ci & (1u << slot)) != 0) {
                    if (process_ahci_ata_command_slot(ahci, disk, 0, slot) != 0) {
                        return -1;
                    }
                }
            }
        }
    } else {
        uint32_t value = 0;
        if (hype_ahci_mmio_read(ahci, m.offset, m.decoded.size_bytes, &value) != 0) {
            return -1;
        }
        *m.reg = hype_mmio_merge_read_value(*m.reg, value, m.decoded.size_bytes, m.decoded.zero_extend);
    }
    vmx_mmio_end(&m);
    return 0;
}

/*
 * #262 slice 3: the remapped-guest variant, mirroring
 * hype_vmx_vcpu_handle_ahci_npf_map for the ATAPI controller. vmx_mmio_begin_insn
 * takes the already-fetched instruction bytes so nothing dereferences a guest RIP as
 * a host pointer, and dma_map translates every guest-physical address the command
 * carries.
 */
int hype_vmx_vcpu_handle_ahci_disk_npf_map(hype_vcpu_ctx_t *ctx, hype_ahci_t *ahci,
                                           hype_ata_disk_t *disk, uint64_t ahci_base_phys,
                                           const hype_gpa_map_t *dma_map,
                                           const uint8_t *guest_insn_bytes) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    struct vmx_mmio_access m;

    if (vmx_mmio_begin_insn(real, ahci_base_phys, HYPE_AHCI_MMIO_SIZE, guest_insn_bytes, &m) != 0) {
        return -1;
    }
    if (m.decoded.is_write) {
        uint32_t value = hype_mmio_extract_write_value(*m.reg, m.decoded.size_bytes);
        if (hype_ahci_mmio_write(ahci, m.offset, m.decoded.size_bytes, value) != 0) {
            return -1;
        }
        if (m.offset == HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CI && ahci->p_ci != 0) {
            unsigned slot;
            for (slot = 0; slot < 32u; slot++) {
                if ((ahci->p_ci & (1u << slot)) != 0) {
                    if (process_ahci_ata_command_slot(ahci, disk, dma_map, slot) != 0) {
                        return -1;
                    }
                }
            }
        }
    } else {
        uint32_t value = 0;
        if (hype_ahci_mmio_read(ahci, m.offset, m.decoded.size_bytes, &value) != 0) {
            return -1;
        }
        *m.reg =
            hype_mmio_merge_read_value(*m.reg, value, m.decoded.size_bytes, m.decoded.zero_extend);
    }
    vmx_mmio_end(&m);
    return 0;
}

/* VMX MMIO handler for the Bochs VBE (DISPI) display (VMX-2): mirror of
 * hype_svm_vcpu_handle_bochs_vbe_npf. DISPI registers are 16-bit only. */
int hype_vmx_vcpu_handle_bochs_vbe_npf(hype_vcpu_ctx_t *ctx, hype_bochs_vbe_t *dev,
                                       uint64_t mmio_base_phys) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    struct vmx_mmio_access m;
    if (vmx_mmio_begin(real, mmio_base_phys, HYPE_BOCHS_VBE_MMIO_SIZE, &m) != 0) {
        return -1;
    }
    if (m.decoded.size_bytes != 2u) {
        return -1; /* DISPI registers are architecturally 16-bit only */
    }
    if (m.offset < HYPE_BOCHS_VBE_DISPI_OFFSET ||
        m.offset >= HYPE_BOCHS_VBE_DISPI_OFFSET + HYPE_BOCHS_VBE_DISPI_SIZE) {
        if (!m.decoded.is_write) {
            *m.reg = hype_mmio_merge_read_value(*m.reg, 0, m.decoded.size_bytes, m.decoded.zero_extend);
        }
        vmx_mmio_end(&m);
        return 0;
    }
    if (m.decoded.is_write) {
        uint32_t value = hype_mmio_extract_write_value(*m.reg, m.decoded.size_bytes);
        if (hype_bochs_vbe_mmio_write(dev, m.offset - HYPE_BOCHS_VBE_DISPI_OFFSET, (uint16_t)value) !=
            0) {
            return -1;
        }
    } else {
        uint16_t value = 0;
        if (hype_bochs_vbe_mmio_read(dev, m.offset - HYPE_BOCHS_VBE_DISPI_OFFSET, &value) != 0) {
            return -1;
        }
        *m.reg = hype_mmio_merge_read_value(*m.reg, value, m.decoded.size_bytes, m.decoded.zero_extend);
    }
    vmx_mmio_end(&m);
    return 0;
}

/* VMX MMIO handler for the virtio-blk BAR (VMX-2): mirror of
 * hype_svm_vcpu_handle_virtio_blk_npf. Routes the BAR offset to the virtio
 * common/notify/ISR/device-config regions; a notify write kicks the queue,
 * drained by the shared process_virtio_blk_queue() (dma_map 0 = identity). */
/*
 * Common virtio-blk MMIO body (VMX-4, #236). Was the whole of
 * hype_vmx_vcpu_handle_virtio_blk_npf; now shared with the live-guest entry
 * point below, which differs only in supplying real instruction bytes and a
 * real dma_map. Mirrors how the SVM side shares hype_svm_ahci_atapi_npf_common.
 */
static int vmx_virtio_blk_npf_common(struct hype_vcpu_ctx *real, hype_virtio_blk_t *dev,
                                     const hype_blk_backend_t *be, const hype_gpa_map_t *dma_map,
                                     uint64_t mmio_base_phys, const uint8_t *insn) {
    struct vmx_mmio_access m;
    uint32_t off;
    if (vmx_mmio_begin_insn(real, mmio_base_phys, HYPE_VIRTIO_BLK_BAR_SIZE, insn, &m) != 0) {
        return -1;
    }
    off = m.offset;

    if (off >= HYPE_VIRTIO_BLK_BAR_COMMON_CFG_OFFSET &&
        off < HYPE_VIRTIO_BLK_BAR_COMMON_CFG_OFFSET + HYPE_VIRTIO_COMMON_CFG_SIZE) {
        uint32_t ro = off - HYPE_VIRTIO_BLK_BAR_COMMON_CFG_OFFSET;
        if (m.decoded.is_write) {
            uint32_t value = hype_mmio_extract_write_value(*m.reg, m.decoded.size_bytes);
            if (hype_virtio_blk_common_cfg_write(dev, ro, m.decoded.size_bytes, value) != 0) {
                return -1;
            }
        } else {
            uint32_t value = 0;
            if (hype_virtio_blk_common_cfg_read(dev, ro, m.decoded.size_bytes, &value) != 0) {
                return -1;
            }
            *m.reg =
                hype_mmio_merge_read_value(*m.reg, value, m.decoded.size_bytes, m.decoded.zero_extend);
        }
    } else if (off >= HYPE_VIRTIO_BLK_BAR_NOTIFY_CFG_OFFSET &&
               off < HYPE_VIRTIO_BLK_BAR_NOTIFY_CFG_OFFSET + 4u) {
        if (m.decoded.is_write) {
            if (hype_virtio_blk_is_queue_ready(dev)) {
                if (process_virtio_blk_queue(dev, be, dma_map) != 0) {
                    return -1;
                }
            }
        } else {
            *m.reg = hype_mmio_merge_read_value(*m.reg, 0, m.decoded.size_bytes, m.decoded.zero_extend);
        }
    } else if (off == HYPE_VIRTIO_BLK_BAR_ISR_CFG_OFFSET) {
        if (!m.decoded.is_write) {
            uint8_t value = hype_virtio_blk_isr_read(dev);
            *m.reg =
                hype_mmio_merge_read_value(*m.reg, value, m.decoded.size_bytes, m.decoded.zero_extend);
        }
    } else if (off >= HYPE_VIRTIO_BLK_BAR_DEVICE_CFG_OFFSET &&
               off < HYPE_VIRTIO_BLK_BAR_DEVICE_CFG_OFFSET + HYPE_VIRTIO_BLK_CFG_SIZE) {
        if (!m.decoded.is_write) {
            uint32_t value = 0;
            uint32_t ro = off - HYPE_VIRTIO_BLK_BAR_DEVICE_CFG_OFFSET;
            if (hype_virtio_blk_device_cfg_read(dev, ro, m.decoded.size_bytes, &value) != 0) {
                return -1;
            }
            *m.reg =
                hype_mmio_merge_read_value(*m.reg, value, m.decoded.size_bytes, m.decoded.zero_extend);
        }
    } else {
        if (!m.decoded.is_write) {
            *m.reg = hype_mmio_merge_read_value(*m.reg, 0, m.decoded.size_bytes, m.decoded.zero_extend);
        }
    }

    vmx_mmio_end(&m);
    return 0;
}

/* VMX guest GDTR/IDTR setup (VMX-2, INT): mirrors of hype_svm_vcpu_set_gdt/idt.
 * Real interrupt delivery reloads CS from the guest GDT and vectors through the
 * guest IDT, so both must point at real tables (VMWRITE base+limit). */
void hype_vmx_vcpu_set_gdt(hype_vcpu_ctx_t *ctx, uint64_t base, uint16_t limit) {
    (void)ctx;
    vmwrite(HYPE_VMCS_GUEST_GDTR_BASE, base);
    vmwrite(HYPE_VMCS_GUEST_GDTR_LIMIT, limit);
}
void hype_vmx_vcpu_set_idt(hype_vcpu_ctx_t *ctx, uint64_t base, uint16_t limit) {
    (void)ctx;
    vmwrite(HYPE_VMCS_GUEST_IDTR_BASE, base);
    vmwrite(HYPE_VMCS_GUEST_IDTR_LIMIT, limit);
}

/* Arm/disarm interrupt-window exiting in the live VMCS's primary proc-based
 * controls (read-modify-write, preserving every other bit). */
static void vmx_set_intr_window(int on) {
    int ok;
    uint64_t ctls = vmread(HYPE_VMCS_CPU_BASED_VM_EXEC_CONTROL, &ok);
    if (on) {
        ctls |= HYPE_VMX_PROCBASED_INTERRUPT_WINDOW_EXITING;
    } else {
        ctls &= ~(uint64_t)HYPE_VMX_PROCBASED_INTERRUPT_WINDOW_EXITING;
    }
    vmwrite(HYPE_VMCS_CPU_BASED_VM_EXEC_CONTROL, ctls);
}

/*
 * VMX interrupt request (VMX-2, INT-1/INT-2): mirror of
 * hype_svm_vcpu_request_interrupt. Records one pending vector and arms
 * interrupt-window exiting so the vector is injected as soon as the guest can
 * accept it (RFLAGS.IF=1, no shadow) -- surfaced as an interrupt-window VM-exit
 * (reason 7), handled by hype_vmx_vcpu_handle_intr_window below. Deferring
 * unconditionally (rather than trying to inject inline) is correct because the
 * guest's RFLAGS at request time is its initial state (IF=0), before it STIs.
 */
/* VM_ENTRY_INTR_INFO for an external interrupt: valid (bit 31) | type 0 | vector. */
#define HYPE_VMX_ENTRY_INTR_EXT(vec) (0x80000000u | (uint32_t)(vec))
/* Is an event already staged for the next VM-entry? Must never be clobbered. */
static int vmx_entry_event_staged(void) {
    int ok;
    return (int)((vmread(HYPE_VMCS_VM_ENTRY_INTR_INFO_FIELD, &ok) >> 31) & 1u);
}
/* Can the guest accept an external interrupt right now? Shares SVM's
 * unit-tested predicate rather than re-deriving the IF/shadow rules here --
 * #242 was precisely a duplicated-and-diverged vendor path. */
static int vmx_can_accept_interrupt(void) {
    int ok;
    uint64_t rflags = vmread(HYPE_VMCS_GUEST_RFLAGS, &ok);
    uint64_t block = vmread(HYPE_VMCS_GUEST_INTERRUPTIBILITY_STATE, &ok);
    return hype_svm_can_accept_interrupt(rflags, block);
}

/*
 * #248/#252: interrupt-delivery counters, the VMX half of what SVM has had since
 * INT-1/INT-2. vmm_get_int_diag() short-circuited for VMX and returned 0, so the
 * INTDIAG line printed all-zeros on Intel no matter what happened -- the one
 * instrument that says whether interrupts reach the guest reported nothing on the
 * vendor where that is the open question. Semantics deliberately match
 * hype_svm_vcpu_get_int_diag()'s so the same line means the same thing on both:
 *   eventinj   -- injected immediately, guest was ready
 *   defer      -- queued in the IRR instead of injected
 *   window     -- injected later, drained once the guest could accept it
 *   overwrite  -- vector was already pending -> coalesced (one delivery per IRR bit)
 *   collision  -- wanted to inject but an event was already staged for VM-entry
 */
static unsigned long long g_vmx_int_eventinj = 0;
static unsigned long long g_vmx_int_defer = 0;
static unsigned long long g_vmx_int_window = 0;
static unsigned long long g_vmx_int_overwrite = 0;
static unsigned long long g_vmx_int_collision = 0;

void hype_vmx_vcpu_get_int_diag(unsigned long long *eventinj, unsigned long long *defer,
                                unsigned long long *window, unsigned long long *overwrite) {
    *eventinj = g_vmx_int_eventinj;
    *defer = g_vmx_int_defer;
    *window = g_vmx_int_window;
    *overwrite = g_vmx_int_overwrite;
}

unsigned long long hype_vmx_vcpu_get_eventinj_collisions(void) {
    return g_vmx_int_collision;
}

void hype_vmx_vcpu_request_interrupt(hype_vcpu_ctx_t *ctx, uint8_t vector) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    int staged = vmx_entry_event_staged();
    int ready = vmx_can_accept_interrupt();
    /* Sampled BEFORE the IRR set, or "already pending" is always true of the bit
     * we are about to set and the coalesce count is meaningless. */
    int already_pending =
        (real->pending_irr[vector >> 5] & ((uint32_t)1u << (vector & 31u))) != 0;

    /* VMX-4: queue into the IRR instead of overwriting a single slot, then
     * inject immediately if the guest is ready (INT-1) or arm an
     * interrupt-window exit to inject later (INT-2). Same shape as SVM's
     * hype_svm_vcpu_request_interrupt. */
    hype_svm_irr_set(real->pending_irr, vector);
    if (!staged && ready) {
        int v = hype_svm_irr_highest(real->pending_irr);
        if (v >= 0) {
            hype_svm_irr_clear(real->pending_irr, (uint8_t)v);
            vmwrite(HYPE_VMCS_VM_ENTRY_INTR_INFO_FIELD, HYPE_VMX_ENTRY_INTR_EXT(v));
            g_vmx_int_eventinj++;
        }
    } else {
        if (staged) {
            g_vmx_int_collision++;
        }
        if (already_pending) {
            g_vmx_int_overwrite++;
        }
        g_vmx_int_defer++;
    }
    /* Keep the window armed while anything remains queued. */
    vmx_set_intr_window(hype_svm_irr_any(real->pending_irr) ? 1 : 0);
}

/*
 * VMX-4: the analogue of hype_svm_vcpu_deliver_pending_if_ready -- poll-inject
 * a queued vector the moment the guest can take it, rather than relying solely
 * on the interrupt-window exit firing. On SVM this fixed a real wedge (a
 * deferred PIT IRQ0 stranded while the guest was ready, freezing jiffies and
 * hanging libata's post-COMRESET msleep); the same failure mode applies here.
 * Returns 1 if it injected.
 */
int hype_vmx_vcpu_deliver_pending_if_ready(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    int v;

    if (!hype_svm_irr_any(real->pending_irr)) {
        return 0;
    }
    if (vmx_entry_event_staged()) {
        return 0; /* an event is already staged for the next VM-entry */
    }
    if (!vmx_can_accept_interrupt()) {
        return 0;
    }
    v = hype_svm_irr_highest(real->pending_irr);
    if (v < 0) {
        return 0;
    }
    hype_svm_irr_clear(real->pending_irr, (uint8_t)v);
    vmwrite(HYPE_VMCS_VM_ENTRY_INTR_INFO_FIELD, HYPE_VMX_ENTRY_INTR_EXT(v));
    g_vmx_int_window++; /* drained from the queue once the guest could accept it */
    vmx_set_intr_window(hype_svm_irr_any(real->pending_irr) ? 1 : 0);
    return 1;
}

/*
 * VMX interrupt-window handler (VMX-2): mirror of
 * hype_svm_vcpu_handle_vintr_window. The window fired -> the guest can accept
 * an interrupt now, so stage the pending vector into VM_ENTRY_INTR_INFO (valid
 * | type 0 external | vector) for the next VM-entry and disarm the window. The
 * CPU delivers it through the guest IDT on VMRESUME.
 */
void hype_vmx_vcpu_handle_intr_window(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    int v = hype_svm_irr_highest(real->pending_irr);
    if (v >= 0 && !vmx_entry_event_staged()) {
        hype_svm_irr_clear(real->pending_irr, (uint8_t)v);
        vmwrite(HYPE_VMCS_VM_ENTRY_INTR_INFO_FIELD, HYPE_VMX_ENTRY_INTR_EXT(v));
    }
    /* VMX-4: stay armed if more vectors are queued -- disarming unconditionally
     * here would strand every vector after the first. */
    vmx_set_intr_window(hype_svm_irr_any(real->pending_irr) ? 1 : 0);
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

/* ===================================================================== *
 * VMX-4 (#236): the vCPU state accessors the FW-1 live-guest loop needs.
 *
 * Each is the VMX counterpart of an identically-named hype_svm_vcpu_*
 * function, reached through the vmm_* shims in boot/main.c. Where SVM reads
 * or writes a VMCB field directly, VMX uses VMREAD/VMWRITE on the equivalent
 * VMCS field; the semantics the caller sees are identical.
 * ===================================================================== */

uint64_t hype_vmx_vcpu_get_cr3(hype_vcpu_ctx_t *ctx) {
    int ok;
    (void)ctx;
    return vmread(HYPE_VMCS_GUEST_CR3, &ok);
}

void hype_vmx_vcpu_set_rip(hype_vcpu_ctx_t *ctx, uint64_t rip) {
    (void)ctx;
    vmwrite(HYPE_VMCS_GUEST_RIP, rip);
}

/*
 * No Decode Assist on VMX: the VMCS has no analogue of SVM's
 * num_bytes_fetched/guest_instruction_bytes, so report "none fetched" and let
 * the caller fall back to its page-table walk (fw_1_insn_bytes_via_ptwalk()).
 * That is not a loss -- svm_vcpu.c does not trust Decode Assist either (its own
 * comment records it as not reliably populated under nested SVM even when CPUID
 * advertises it), so the ptwalk path is the one both backends actually use.
 */
const uint8_t *hype_vmx_vcpu_guest_insn_bytes(hype_vcpu_ctx_t *ctx, uint8_t *out_num) {
    (void)ctx;
    if (out_num != 0) {
        *out_num = 0;
    }
    return 0;
}

/* The EPT violation that caused this exit, in the vendor-neutral shape. */
void hype_vmx_vcpu_get_last_npf(hype_vcpu_ctx_t *ctx, hype_vmm_npf_t *out) {
    int ok;
    (void)ctx;
    out->guest_phys_addr = vmread(HYPE_VMCS_GUEST_PHYSICAL_ADDRESS, &ok);
    out->is_write = (int)((vmread(HYPE_VMCS_EXIT_QUALIFICATION, &ok) >> 1) & 1u);
}

/*
 * Decode the I/O exit qualification (SDM Table 28-5) without consuming the
 * exit: bits 2:0 size (0/1/3 => 1/2/4 bytes), bit 3 direction (1=IN), bit 4
 * string, bit 5 REP, bits 31:16 port.
 *
 * Note VMX reports the *operand* size here but not the address size, which SVM
 * does carry (ADDR16/32/64 in EXITINFO1). For string I/O the address size is
 * needed to index (E/R)SI/(E/R)DI, so derive it from the guest's current mode
 * rather than guessing: 64-bit if EFER.LMA, else 32 if CS.D, else 16.
 */
static void vmx_decode_ioio(hype_vmm_ioio_t *out) {
    int ok;
    uint64_t qual = vmread(HYPE_VMCS_EXIT_QUALIFICATION, &ok);
    uint32_t sz = (uint32_t)(qual & 7u);
    out->port = (uint16_t)((qual >> 16) & 0xFFFFu);
    out->is_in = (int)((qual >> 3) & 1u);
    out->is_string = (int)((qual >> 4) & 1u);
    out->is_rep = (int)((qual >> 5) & 1u);
    out->size_bytes = (uint8_t)(sz == 0u ? 1u : (sz == 1u ? 2u : 4u));
    {
        uint64_t efer = vmread(HYPE_VMCS_GUEST_IA32_EFER, &ok);
        uint64_t cs_ar = vmread(HYPE_VMCS_GUEST_CS_AR_BYTES, &ok);
        if ((efer & (1ULL << 10)) != 0) { /* EFER.LMA: 64-bit mode */
            out->addr_size_bytes = 8u;
        } else if ((cs_ar & (1ULL << 14)) != 0) { /* CS.D */
            out->addr_size_bytes = 4u;
        } else {
            out->addr_size_bytes = 2u;
        }
    }
}

void hype_vmx_vcpu_peek_ioio(hype_vcpu_ctx_t *ctx, hype_vmm_ioio_t *out) {
    (void)ctx;
    vmx_decode_ioio(out);
}

/* Unhandled port I/O: report it and step over the instruction so the guest
 * makes progress (GLADDER-1's absorb-rather-than-die posture). */
void hype_vmx_vcpu_handle_unknown_ioio(hype_vcpu_ctx_t *ctx, hype_vmm_ioio_t *out) {
    (void)ctx;
    vmx_decode_ioio(out);
    if (out->is_in) {
        /* An unmodelled port reads as all-ones, the same "nothing there" answer
         * real hardware gives on an unclaimed port. */
        struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
        uint64_t mask = (out->size_bytes == 1u) ? 0xFFULL
                                               : ((out->size_bytes == 2u) ? 0xFFFFULL : 0xFFFFFFFFULL);
        real->gprs[0] |= mask;
    }
    vmx_advance_rip();
}

void hype_vmx_vcpu_set_exception_intercepts(hype_vcpu_ctx_t *ctx, uint32_t mask) {
    (void)ctx;
    vmwrite(HYPE_VMCS_EXCEPTION_BITMAP, mask);
}

/*
 * Exit on an external interrupt, so host timekeeping stays alive while a guest
 * runs (SVM's INTR intercept). Read-modify-write: the pin-based controls also
 * carry bits the VMCS build set, and clobbering them would fail VM-entry.
 */
void hype_vmx_vcpu_enable_intr_intercept(hype_vcpu_ctx_t *ctx) {
    int ok;
    uint64_t pin = vmread(HYPE_VMCS_PIN_BASED_VM_EXEC_CONTROL, &ok);
    (void)ctx;
    vmwrite(HYPE_VMCS_PIN_BASED_VM_EXEC_CONTROL, pin | HYPE_VMX_PINBASED_EXT_INTR_EXITING);
}

/*
 * PAUSE-loop exiting, the VMX analogue of SVM's pause filter.
 *
 * The units genuinely differ and are not interchangeable: SVM counts PAUSE
 * *executions* (count, with an inter-PAUSE threshold), while VMX counts TSC
 * ticks (PLE_WINDOW, with PLE_GAP as the max inter-PAUSE gap). So this does not
 * pass the caller's numbers through -- it takes them as "SVM-shaped intent" and
 * applies VMX-appropriate values instead. `count` being SVM's saturated 65535
 * (i.e. "effectively never filter") maps to leaving PLE off entirely, which is
 * the honest translation rather than inventing a tick count.
 */
void hype_vmx_vcpu_enable_pause_filter(hype_vcpu_ctx_t *ctx, uint16_t count, uint16_t threshold) {
    int ok;
    uint64_t proc, proc2;
    (void)ctx;
    if (count == 0xFFFFu) {
        return; /* caller asked for no effective filtering */
    }
    proc = vmread(HYPE_VMCS_CPU_BASED_VM_EXEC_CONTROL, &ok);
    proc2 = vmread(HYPE_VMCS_SECONDARY_VM_EXEC_CONTROL, &ok);
    vmwrite(HYPE_VMCS_PLE_GAP, threshold);
    vmwrite(HYPE_VMCS_PLE_WINDOW, (uint64_t)count);
    vmwrite(HYPE_VMCS_CPU_BASED_VM_EXEC_CONTROL, proc | HYPE_VMX_PROCBASED_PAUSE_EXITING);
    vmwrite(HYPE_VMCS_SECONDARY_VM_EXEC_CONTROL, proc2 | HYPE_VMX_PROCBASED2_PAUSE_LOOP_EXITING);
}

/*
 * Re-inject an intercepted exception so the guest takes it through its own IDT.
 * VMX splits what SVM packs into one EVENTINJ across three fields: the info
 * field (valid | type 3 = hardware exception | vector, plus bit 11 when an
 * error code is present), the error code itself, and the instruction length.
 */
void hype_vmx_vcpu_reinject_exception(hype_vcpu_ctx_t *ctx, uint8_t vector, int has_error_code,
                                      uint32_t error_code) {
    int ok;
    uint32_t info = 0x80000000u | (3u << 8) | (uint32_t)vector;
    (void)ctx;
    if (has_error_code) {
        info |= (1u << 11); /* deliver-error-code */
        vmwrite(HYPE_VMCS_VM_ENTRY_EXCEPTION_ERROR_CODE, error_code);
    }
    vmwrite(HYPE_VMCS_VM_ENTRY_INSTRUCTION_LEN, vmread(HYPE_VMCS_VM_EXIT_INSTRUCTION_LEN, &ok));
    vmwrite(HYPE_VMCS_VM_ENTRY_INTR_INFO_FIELD, info);
}

/*
 * Model an interrupt waking a halted guest. Two things must happen or a
 * `sti; hlt` idle wait deadlocks (the SVM comment explains the failure in
 * full): the HLT must retire so the guest resumes AFTER it, and the STI shadow
 * that covered it must be consumed. HLT is a 1-byte opcode, so past-it is
 * RIP+1. VMX additionally records halted-ness in GUEST_ACTIVITY_STATE, which
 * has to go back to active or VM-entry re-halts immediately.
 */
void hype_vmx_vcpu_wake_hlt(hype_vcpu_ctx_t *ctx) {
    int ok;
    uint64_t rip = vmread(HYPE_VMCS_GUEST_RIP, &ok);
    uint64_t block = vmread(HYPE_VMCS_GUEST_INTERRUPTIBILITY_STATE, &ok);
    (void)ctx;
    vmwrite(HYPE_VMCS_GUEST_RIP, rip + 1u);
    vmwrite(HYPE_VMCS_GUEST_INTERRUPTIBILITY_STATE,
            block & ~(uint64_t)HYPE_VMX_INTERRUPTIBILITY_BLOCKING_BY_STI);
    vmwrite(HYPE_VMCS_GUEST_ACTIVITY_STATE, HYPE_VMX_ACTIVITY_STATE_ACTIVE);
}

void hype_vmx_vcpu_get_intr_state(hype_vcpu_ctx_t *ctx, hype_vmm_intr_state_t *out) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    int ok;
    out->rflags = vmread(HYPE_VMCS_GUEST_RFLAGS, &ok);
    out->interrupt_shadow = vmread(HYPE_VMCS_GUEST_INTERRUPTIBILITY_STATE, &ok);
    out->eventinj = vmread(HYPE_VMCS_VM_ENTRY_INTR_INFO_FIELD, &ok);
    /* "vintr" means "is an interrupt window armed" -- on VMX that is the
     * interrupt-window-exiting control, not a dedicated field. */
    out->vintr = (vmread(HYPE_VMCS_CPU_BASED_VM_EXEC_CONTROL, &ok) &
                  HYPE_VMX_PROCBASED_INTERRUPT_WINDOW_EXITING)
                     ? 1u
                     : 0u;
    out->can_accept = hype_svm_can_accept_interrupt(out->rflags, out->interrupt_shadow);
    out->pending_valid = hype_svm_irr_any(real->pending_irr);
    {
        int hv = hype_svm_irr_highest(real->pending_irr);
        out->pending_vector = (uint8_t)(hv < 0 ? 0 : hv);
    }
}

/*
 * pvclock scaling + the ACPI PM timer's rate. Separate from the SVM backend's
 * equivalents by design: these are per-backend file-scope state, and only one
 * backend is ever live in a given boot.
 */
static uint32_t g_vmx_pvclock_mul;
static int8_t g_vmx_pvclock_shift;
static uint64_t g_vmx_acpi_pm_tsc_hz;

static void vmx_pvclock_arm_system_time(struct hype_vcpu_ctx *real, uint64_t msr_value) {
    uint64_t gpa, host, now, system_ns;
    if ((msr_value & HYPE_KVM_SYSTEM_TIME_ENABLE) == 0 || real->pvclock_map == 0) {
        return;
    }
    gpa = msr_value & HYPE_KVM_MSR_ADDR_MASK;
    host = hype_gpa_to_host(real->pvclock_map, gpa, sizeof(struct hype_pvclock_vcpu_time_info));
    if (host == 0) {
        return;
    }
    now = vmx_real_rdtsc();
    system_ns = hype_pvclock_scale_delta(now, g_vmx_pvclock_mul, g_vmx_pvclock_shift);
    hype_pvclock_write_time_info((volatile struct hype_pvclock_vcpu_time_info *)(uintptr_t)host, now,
                                 system_ns, g_vmx_pvclock_mul, g_vmx_pvclock_shift,
                                 HYPE_PVCLOCK_TSC_STABLE_BIT);
    g_hype_pvclock_arm_count++;
}

static void vmx_pvclock_arm_wall_clock(struct hype_vcpu_ctx *real, uint64_t msr_value) {
    uint64_t gpa, host;
    if (real->pvclock_map == 0) {
        return;
    }
    gpa = msr_value & HYPE_KVM_MSR_ADDR_MASK;
    host = hype_gpa_to_host(real->pvclock_map, gpa, sizeof(struct hype_pvclock_wall_clock));
    if (host == 0) {
        return;
    }
    hype_pvclock_write_wall_clock((volatile struct hype_pvclock_wall_clock *)(uintptr_t)host, 0, 0);
}

void hype_vmx_vcpu_set_pvclock(hype_vcpu_ctx_t *ctx, const hype_gpa_map_t *map, uint64_t tsc_hz) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    real->pvclock_map = map;
    hype_pvclock_calc_scale(tsc_hz, &g_vmx_pvclock_mul, &g_vmx_pvclock_shift);
    g_vmx_acpi_pm_tsc_hz = tsc_hz; /* M4-6b2: also drives the ACPI PM timer's rate */
}

/* ===================================================================== *
 * VMX-4 (#236): the FW-1 device adapters.
 *
 * Each mirrors the identically-named hype_svm_vcpu_* handler. Three
 * differences apply throughout and are not repeated per function:
 *
 *  1. The faulting instruction's bytes are passed IN rather than decoded at
 *     GUEST_RIP. FW-1's EPT/NPT deliberately remaps guest RAM and the firmware
 *     flash window away from an identity map, so treating GUEST_RIP as a host
 *     pointer -- which the microtest handlers above legitimately do, their
 *     guests being flat identity maps -- reads the wrong memory here. The
 *     caller resolves RIP through its own page-table walk first.
 *  2. RAX lives in ctx->gprs[0], not in a save area: VMX saves every guest GPR
 *     through the trampoline, where SVM's VMCB holds RAX specially.
 *  3. Resume RIP comes from VM_EXIT_INSTRUCTION_LEN (vmx_advance_rip()), where
 *     SVM reads EXITINFO2's "next RIP for free". Same result, different source.
 * ===================================================================== */

/*
 * vmx_mmio_begin() with caller-supplied instruction bytes -- see note 1 above.
 * vmx_mmio_begin() itself now delegates here, so the range check / direction
 * cross-check / GPR resolution exist once rather than twice.
 */
static int vmx_mmio_begin_insn(struct hype_vcpu_ctx *real, uint64_t base, uint64_t size,
                               const uint8_t *insn, struct vmx_mmio_access *m) {
    int ok;
    uint64_t gpa = vmread(HYPE_VMCS_GUEST_PHYSICAL_ADDRESS, &ok);
    uint64_t qual = vmread(HYPE_VMCS_EXIT_QUALIFICATION, &ok);
    m->rip = vmread(HYPE_VMCS_GUEST_RIP, &ok);
    m->is_write = (int)((qual >> 1) & 1u);
    if (gpa < base || gpa >= base + size) {
        return -1;
    }
    m->offset = (uint32_t)(gpa - base);
    if (insn == 0 || hype_mmio_decode(insn, HYPE_VMX_MMIO_MAX_INSTR_BYTES, &m->decoded) != 0) {
        return -1;
    }
    if (m->decoded.is_write != m->is_write) {
        return -1;
    }
    m->reg = vmx_gpr_ptr(real, m->decoded.reg);
    return (m->reg == 0) ? -1 : 0;
}

/* Guest Local APIC MMIO. xAPIC registers are 32-bit only, so a non-4-byte
 * access fails closed rather than being half-emulated. */
int hype_vmx_vcpu_handle_lapic_npf(hype_vcpu_ctx_t *ctx, hype_guest_lapic_t *lapic,
                                   uint64_t lapic_base_phys, const uint8_t *guest_insn_bytes) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    struct vmx_mmio_access m;

    if (vmx_mmio_begin_insn(real, lapic_base_phys, HYPE_GUEST_LAPIC_MMIO_SIZE, guest_insn_bytes,
                            &m) != 0) {
        return -1;
    }
    if (m.decoded.size_bytes != 4u) {
        return -1;
    }
    if (m.decoded.is_write) {
        uint32_t value = hype_mmio_extract_write_value(*m.reg, m.decoded.size_bytes);
        if (hype_guest_lapic_write(lapic, m.offset, m.decoded.size_bytes, value) != 0) {
            return -1;
        }
    } else {
        uint32_t value = 0;
        if (hype_guest_lapic_read(lapic, m.offset, m.decoded.size_bytes, &value) != 0) {
            return -1;
        }
        *m.reg = hype_mmio_merge_read_value(*m.reg, value, m.decoded.size_bytes, m.decoded.zero_extend);
    }
    vmx_mmio_end(&m);
    return 0;
}

/* Guest I/O APIC MMIO. IOREGSEL/IOWIN are 32-bit only. */
int hype_vmx_vcpu_handle_ioapic_npf(hype_vcpu_ctx_t *ctx, hype_ioapic_t *ioapic,
                                    uint64_t ioapic_base_phys, const uint8_t *guest_insn_bytes) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    struct vmx_mmio_access m;

    if (vmx_mmio_begin_insn(real, ioapic_base_phys, HYPE_IOAPIC_MMIO_SIZE, guest_insn_bytes, &m) !=
        0) {
        return -1;
    }
    if (m.decoded.size_bytes != 4u) {
        return -1;
    }
    if (m.decoded.is_write) {
        uint32_t value = hype_mmio_extract_write_value(*m.reg, m.decoded.size_bytes);
        if (hype_ioapic_mmio_write(ioapic, m.offset, value) != 0) {
            return -1;
        }
    } else {
        uint32_t value = 0;
        if (hype_ioapic_mmio_read(ioapic, m.offset, &value) != 0) {
            return -1;
        }
        *m.reg = hype_mmio_merge_read_value(*m.reg, value, m.decoded.size_bytes, m.decoded.zero_extend);
    }
    vmx_mmio_end(&m);
    return 0;
}

/*
 * AHCI HBA MMIO for the live guest: like hype_vmx_vcpu_handle_ahci_npf above,
 * but with caller-supplied instruction bytes and a real dma_map, because FW-1's
 * guest-physical addresses are not host-physical. The command-list/PRDT/FIS DMA
 * itself is the shared, vendor-neutral process_ahci_command_slot().
 */
int hype_vmx_vcpu_handle_ahci_npf_map(hype_vcpu_ctx_t *ctx, hype_ahci_t *ahci, hype_atapi_t *atapi,
                                      uint64_t ahci_base_phys, const hype_gpa_map_t *dma_map,
                                      const uint8_t *guest_insn_bytes) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    struct vmx_mmio_access m;

    if (vmx_mmio_begin_insn(real, ahci_base_phys, HYPE_AHCI_MMIO_SIZE, guest_insn_bytes, &m) != 0) {
        return -1;
    }
    if (m.decoded.is_write) {
        uint32_t value = hype_mmio_extract_write_value(*m.reg, m.decoded.size_bytes);
        if (hype_ahci_mmio_write(ahci, m.offset, m.decoded.size_bytes, value) != 0) {
            return -1;
        }
        if (m.offset == HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CI && ahci->p_ci != 0) {
            unsigned slot;
            for (slot = 0; slot < 32u; slot++) {
                if ((ahci->p_ci & (1u << slot)) != 0) {
                    if (process_ahci_command_slot(ahci, atapi, dma_map, slot) != 0) {
                        return -1;
                    }
                }
            }
        }
    } else {
        uint32_t value = 0;
        if (hype_ahci_mmio_read(ahci, m.offset, m.decoded.size_bytes, &value) != 0) {
            return -1;
        }
        *m.reg = hype_mmio_merge_read_value(*m.reg, value, m.decoded.size_bytes, m.decoded.zero_extend);
    }
    vmx_mmio_end(&m);
    return 0;
}

/*
 * GLADDER-1: absorb an EPT violation in a region hype does not model, rather
 * than dying. Reads answer all-ones (what an unclaimed bus returns), writes are
 * dropped, and the guest steps forward. Unlike the handlers above there is no
 * address range to check -- the caller has already tried every modelled region.
 */
int hype_vmx_vcpu_absorb_mmio_npf(hype_vcpu_ctx_t *ctx, const uint8_t *guest_insn_bytes) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_mmio_decode_t decoded;
    int ok;
    uint64_t rip = vmread(HYPE_VMCS_GUEST_RIP, &ok);

    if (guest_insn_bytes == 0 ||
        hype_mmio_decode(guest_insn_bytes, HYPE_VMX_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (!decoded.is_write) {
        uint64_t *reg = vmx_gpr_ptr(real, decoded.reg);
        uint32_t allones;
        if (reg == 0) {
            return -1;
        }
        allones = (decoded.size_bytes >= 4u)  ? 0xFFFFFFFFu
                  : (decoded.size_bytes == 2u) ? 0xFFFFu
                                               : 0xFFu;
        *reg = hype_mmio_merge_read_value(*reg, allones, decoded.size_bytes, decoded.zero_extend);
    }
    /* writes to absent MMIO are dropped */
    vmwrite(HYPE_VMCS_GUEST_RIP, rip + decoded.instr_len);
    return 0;
}

/* ---- port I/O ------------------------------------------------------- *
 * All of these take the port/direction from the I/O exit qualification via
 * vmx_decode_ioio(), operate on RAX in ctx->gprs[0], and advance RIP by
 * VM_EXIT_INSTRUCTION_LEN. Return -1 when the port is not theirs, so the
 * caller's dispatch chain falls through to the next handler.
 * -------------------------------------------------------------------- */

/* Guest 16550 UART. Byte-wide registers at base_port + offset. */
int hype_vmx_vcpu_handle_uart_ioio(hype_vcpu_ctx_t *ctx, hype_guest_uart_t *uart,
                                   uint16_t base_port) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_vmm_ioio_t io;
    uint32_t offset;

    vmx_decode_ioio(&io);
    if (io.port < base_port || io.port >= (uint32_t)base_port + HYPE_GUEST_UART_NREGS) {
        return -1;
    }
    offset = (uint32_t)io.port - base_port;
    if (io.is_in) {
        uint8_t value = hype_guest_uart_read(uart, offset);
        real->gprs[0] = (real->gprs[0] & ~0xFFULL) | value;
    } else {
        hype_guest_uart_write(uart, offset, (uint8_t)(real->gprs[0] & 0xFFu));
    }
    vmx_advance_rip();
    return 0;
}

/* CMOS/RTC index (0x70) + data (0x71). */
int hype_vmx_vcpu_handle_cmos_ioio(hype_vcpu_ctx_t *ctx, hype_cmos_t *cmos) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_vmm_ioio_t io;

    vmx_decode_ioio(&io);
    if (io.port == 0x70u) {
        if (io.is_in) {
            /* Real hardware allows reading the index back; answering is
             * strictly better than failing, even with no caller that does. */
            real->gprs[0] = (real->gprs[0] & ~0xFFULL) | cmos->index;
        } else {
            hype_cmos_index_write(cmos, (uint8_t)(real->gprs[0] & 0xFFu));
        }
    } else if (io.port == 0x71u) {
        if (io.is_in) {
            real->gprs[0] = (real->gprs[0] & ~0xFFULL) | hype_cmos_data_read(cmos);
        } else {
            hype_cmos_data_write(cmos, (uint8_t)(real->gprs[0] & 0xFFu));
        }
    } else {
        return -1;
    }
    vmx_advance_rip();
    return 0;
}

/*
 * ACPI PM1a_CNT. Returns 1 for a read (caller supplied the value), 0 for a
 * write (caller inspects *value / *slp_en), -1 if not this port -- the same
 * three-way contract as the SVM original, which the shutdown path relies on.
 * SLP_EN (bit 13) is write-only and always reads back 0.
 */
int hype_vmx_vcpu_handle_pm1_cnt_ioio(hype_vcpu_ctx_t *ctx, uint16_t port, uint16_t *value,
                                      int *slp_en) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_vmm_ioio_t io;

    vmx_decode_ioio(&io);
    if (io.port != port) {
        return -1;
    }
    if (io.is_in) {
        real->gprs[0] = (real->gprs[0] & ~0xFFFFULL) | ((uint64_t)(*value) & 0xDFFFu);
        vmx_advance_rip();
        return 1;
    }
    {
        uint16_t w = (uint16_t)(real->gprs[0] & 0xFFFFu);
        *slp_en = (w & (1u << 13)) ? 1 : 0;
        *value = (uint16_t)(w & ~(uint16_t)(1u << 13)); /* store without SLP_EN */
    }
    vmx_advance_rip();
    return 0;
}

/* Legacy PCI config access via CF8/CFC. */
int hype_vmx_vcpu_handle_pci_cf8_ioio(hype_vcpu_ctx_t *ctx, hype_pci_t *pci) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_vmm_ioio_t io;

    vmx_decode_ioio(&io);
    if (io.port == HYPE_PCI_CF8_PORT) {
        if (io.is_in) {
            uint32_t value = hype_pci_cf8_read(pci);
            real->gprs[0] =
                hype_mmio_merge_read_value(real->gprs[0], value, io.size_bytes, io.size_bytes == 4);
        } else {
            hype_pci_cf8_write(pci, hype_mmio_extract_write_value(real->gprs[0], io.size_bytes));
        }
    } else if (io.port >= HYPE_PCI_CFC_PORT && io.port <= HYPE_PCI_CFC_PORT + 3) {
        unsigned int byte_offset = io.port - HYPE_PCI_CFC_PORT;
        if (io.is_in) {
            uint32_t value = 0;
            hype_pci_cf8_config_read(pci, byte_offset, io.size_bytes, &value);
            real->gprs[0] =
                hype_mmio_merge_read_value(real->gprs[0], value, io.size_bytes, io.size_bytes == 4);
        } else {
            hype_pci_cf8_config_write(pci, byte_offset, io.size_bytes,
                                      hype_mmio_extract_write_value(real->gprs[0], io.size_bytes));
        }
    } else {
        return -1;
    }
    vmx_advance_rip();
    return 0;
}

/*
 * OVMF's PlatformDebugLibIoPort channel. Returns 0 for a write (caller takes
 * *out_byte), 1 for a read, -1 if not this port. 0xE9 is the presence signature
 * OVMF probes for before enabling the channel.
 */
int hype_vmx_vcpu_handle_debug_port_ioio(hype_vcpu_ctx_t *ctx, uint16_t base_port,
                                         uint8_t *out_byte) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_vmm_ioio_t io;
    int is_write;

    vmx_decode_ioio(&io);
    if (io.port != base_port) {
        return -1;
    }
    is_write = !io.is_in;
    if (io.is_in) {
        real->gprs[0] = (real->gprs[0] & ~0xFFULL) | 0xE9u;
    } else {
        *out_byte = (uint8_t)(real->gprs[0] & 0xFFu);
    }
    vmx_advance_rip();
    return is_write ? 0 : 1;
}

/*
 * ACPI PM timer. Scaled from the host TSC to the architectural 3.579545 MHz
 * rate -- returning the raw ~GHz TSC would mis-scale every firmware delay that
 * reads this port (M4-6b2 fixed exactly that on SVM, ~950x too fast). A write
 * is silently ignored: this register has no writable semantics on real
 * hardware, matching the other "nothing to do" IOIO writes here.
 */
int hype_vmx_vcpu_handle_acpi_pm_timer_ioio(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_vmm_ioio_t io;

    vmx_decode_ioio(&io);
    if (io.port != HYPE_FW_1_ACPI_PM_TIMER_PORT) {
        return -1;
    }
    if (io.is_in) {
        uint32_t value = hype_acpi_pm_timer_scale(vmx_real_rdtsc(), g_vmx_acpi_pm_tsc_hz);
        real->gprs[0] = (real->gprs[0] & ~0xFFFFFFFFULL) | value;
    }
    vmx_advance_rip();
    return 0;
}

/*
 * VMX-4 (#236): which exception did the guest take, and what error code did it
 * push? SVM answers the first from the exit code alone and delivers the second
 * as EXITINFO1; VMX needs VM_EXIT_INTR_INFO (vector in bits 7:0, bit 11 = an
 * error code is present, bit 31 = the field is valid) plus its own error-code
 * field. Returns -1 if no valid interruption is recorded.
 */
int hype_vmx_vcpu_exit_exception_vector(hype_vcpu_ctx_t *ctx) {
    int ok;
    uint64_t info = vmread(HYPE_VMCS_VM_EXIT_INTR_INFO, &ok);
    (void)ctx;
    if (((info >> 31) & 1u) == 0u) {
        return -1;
    }
    return (int)(info & 0xFFu);
}

uint32_t hype_vmx_vcpu_exit_exception_error_code(hype_vcpu_ctx_t *ctx) {
    int ok;
    uint64_t info = vmread(HYPE_VMCS_VM_EXIT_INTR_INFO, &ok);
    (void)ctx;
    if (((info >> 31) & 1u) == 0u || ((info >> 11) & 1u) == 0u) {
        return 0; /* no valid interruption, or this vector pushes no error code */
    }
    return (uint32_t)vmread(HYPE_VMCS_VM_EXIT_INTR_ERROR_CODE, &ok);
}

/*
 * virtio-blk MMIO, microtest flavour: identity-mapped guest, so decode at
 * GUEST_RIP and DMA needs no translation (dma_map 0 = identity).
 */
int hype_vmx_vcpu_handle_virtio_blk_npf(hype_vcpu_ctx_t *ctx, hype_virtio_blk_t *dev,
                                        const hype_blk_backend_t *be, uint64_t mmio_base_phys) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    int ok;
    uint64_t rip = vmread(HYPE_VMCS_GUEST_RIP, &ok);
    return vmx_virtio_blk_npf_common(real, dev, be, 0, mmio_base_phys,
                                     (const uint8_t *)(uintptr_t)rip);
}

/* virtio-blk MMIO, live-guest flavour (VMX-4): caller-resolved instruction
 * bytes and a real dma_map, because FW-1's guest-physical != host-physical. */
int hype_vmx_vcpu_handle_virtio_blk_npf_map(hype_vcpu_ctx_t *ctx, hype_virtio_blk_t *dev,
                                            const hype_blk_backend_t *be,
                                            const hype_gpa_map_t *dma_map, uint64_t mmio_base_phys,
                                            const uint8_t *guest_insn_bytes) {
    return vmx_virtio_blk_npf_common((struct hype_vcpu_ctx *)ctx, dev, be, dma_map, mmio_base_phys,
                                     guest_insn_bytes);
}

/*
 * PCI ECAM config space, live-guest flavour (VMX-4). Same body as
 * hype_vmx_vcpu_handle_pci_ecam_npf but with caller-resolved instruction bytes.
 * ECAM config accesses touch no guest memory, so there is no dma_map here.
 */
int hype_vmx_vcpu_handle_pci_ecam_npf_insn(hype_vcpu_ctx_t *ctx, hype_pci_t *pci,
                                           uint64_t ecam_base_phys,
                                           const uint8_t *guest_insn_bytes) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    struct vmx_mmio_access m;
    hype_pci_ecam_addr_t addr;

    if (vmx_mmio_begin_insn(real, ecam_base_phys, HYPE_PCI_ECAM_BUS0_SIZE, guest_insn_bytes, &m) !=
        0) {
        return -1;
    }
    hype_pci_decode_ecam_offset(m.offset, &addr);
    if (m.decoded.is_write) {
        uint32_t value = hype_mmio_extract_write_value(*m.reg, m.decoded.size_bytes);
        hype_pci_config_write(pci, &addr, m.decoded.size_bytes, value);
    } else {
        uint32_t value = 0;
        hype_pci_config_read(pci, &addr, m.decoded.size_bytes, &value);
        *m.reg = hype_mmio_merge_read_value(*m.reg, value, m.decoded.size_bytes, m.decoded.zero_extend);
    }
    vmx_mmio_end(&m);
    return 0;
}

/*
 * VMX-4 (#236): restart this vCPU as a fresh real-mode guest at
 * guest_rip:0 with the given EPT root -- the counterpart of
 * hype_svm_vcpu_reset_realmode(), used when FW-1 relaunches a VM (a guest
 * reboot, or the second VM's first start) without tearing anything down.
 *
 * Rebuilding the VMCS guest area is exactly what vcpu_create does, so this
 * delegates rather than duplicating the state setup; the extra work is clearing
 * the GPRs and the pending-interrupt state so nothing leaks from the previous
 * incarnation, and forcing launched=0 so the next entry is a VMLAUNCH rather
 * than a VMRESUME of a VMCS that no longer describes the same guest.
 */
void hype_vmx_vcpu_reset_realmode(hype_vcpu_ctx_t *ctx, uint64_t guest_rip, uint64_t guest_rsp,
                                  uint64_t ept_root) {
    (void)ctx; /* single static ctx today -- see #245 */
    (void)hype_vmx_vcpu_create(guest_rip, guest_rsp, ept_root);
}

/* Same as the SVM accessor: the MSR index (guest RCX) at the last MSR exit. */
uint32_t hype_vmx_vcpu_get_msr_index(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    return (uint32_t)real->gprs[1]; /* RCX */
}

/*
 * #248: keep EFER.LMA and the IA-32e-mode-guest entry control in step with the
 * guest's CR0.PG and EFER.LME.
 *
 * Long mode is active exactly when CR0.PG and EFER.LME are both set, and VMX
 * requires the VMCS to agree: VM entry checks that IA-32e-mode-guest matches
 * EFER.LMA (and that CR0.PG is set when it is). The guest performs this
 * transition itself, in stages -- set CR4.PAE, set EFER.LME, then set CR0.PG --
 * so hype has to recompute after each of the two events that can change the
 * answer, not once at creation.
 *
 * Derives LMA rather than trusting whatever the guest wrote into EFER: LMA is
 * hardware-maintained, and a guest setting it directly is not something to
 * honour.
 */
/*
 * #251: make FS and GS usable data segments.
 *
 * hype builds the guest's segments once, for a real-mode guest, and marks FS/GS/SS
 * UNUSABLE (AR byte bit 16). Nothing revisited them as the guest moved to
 * protected and then long mode, so the measured state at the kernel's first
 * per-CPU access was gs_ar=0x1c000 -- unusable, type 0, not present. An access
 * through an unusable segment raises #GP(0) regardless of its base, which is why
 * this is a #GP and not the #PF a merely-wrong base would give.
 *
 * 0xC093 = present, DPL 0, S=1, type 3 (data, read/write, accessed), D/B, G. In
 * 64-bit mode the base comes from the MSR and the limit is ignored, but the
 * descriptor still has to be usable, and VM entry checks the attribute bits
 * against the selector -- so set a properly-formed descriptor rather than only
 * clearing bit 16.
 */
static void vmx_make_fs_gs_usable(void) {
    (void)vmwrite(HYPE_VMCS_GUEST_FS_AR_BYTES, HYPE_VMX_AR_DATA_USABLE);
    (void)vmwrite(HYPE_VMCS_GUEST_GS_AR_BYTES, HYPE_VMX_AR_DATA_USABLE);
    (void)vmwrite(HYPE_VMCS_GUEST_FS_LIMIT, 0xFFFFFFFFu);
    (void)vmwrite(HYPE_VMCS_GUEST_GS_LIMIT, 0xFFFFFFFFu);
}

static void vmx_sync_long_mode(void) {
    int ok;
    uint64_t cr0 = vmread(HYPE_VMCS_GUEST_CR0, &ok);
    uint64_t efer = vmread(HYPE_VMCS_GUEST_IA32_EFER, &ok);
    uint32_t entry = (uint32_t)vmread(HYPE_VMCS_VM_ENTRY_CONTROLS, &ok);
    int lma = ((cr0 & HYPE_VMX_CR0_PG) != 0ull) && ((efer & HYPE_VMX_EFER_LME) != 0ull);

    if (lma) {
        efer |= HYPE_VMX_EFER_LMA;
        entry |= HYPE_VMX_ENTRY_IA32E_MODE_GUEST;
    } else {
        efer &= ~HYPE_VMX_EFER_LMA;
        entry &= ~HYPE_VMX_ENTRY_IA32E_MODE_GUEST;
    }
    (void)vmwrite(HYPE_VMCS_GUEST_IA32_EFER, efer);
    /*
     * Re-adjust rather than writing `entry` straight back. The read-modify-write
     * above preserves whatever was there, but a control field still has to satisfy
     * the capability MSR's allowed-0/allowed-1 masks, and writing an unfiltered
     * value gave VM-instruction-error 7 (invalid control fields) at the next
     * entry. adjust_controls() also re-forces the required-1 bits, so this cannot
     * drift out of spec however often a guest flips modes.
     */
    (void)vmwrite(HYPE_VMCS_VM_ENTRY_CONTROLS,
                  (uint64_t)hype_vmx_adjust_controls(entry, g_vmx_entry_cap));
}

int hype_vmx_vcpu_handle_cr_access(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    int ok;
    uint64_t qual = vmread(HYPE_VMCS_EXIT_QUALIFICATION, &ok);
    unsigned crn = (unsigned)(qual & HYPE_VMX_CR_ACCESS_CR_MASK);
    unsigned type =
        (unsigned)((qual >> HYPE_VMX_CR_ACCESS_TYPE_SHIFT) & HYPE_VMX_CR_ACCESS_TYPE_MASK);
    unsigned gpr = (unsigned)((qual >> HYPE_VMX_CR_ACCESS_GPR_SHIFT) & HYPE_VMX_CR_ACCESS_GPR_MASK);
    uint64_t value;

    /* Only MOV-to-CR is modelled. CLTS/LMSW/MOV-from-CR do not exit with the
     * masks hype sets (reads are served from the read shadow by hardware), so
     * anything else here is unexpected -- report it rather than silently
     * skipping an instruction we did not emulate. */
    if (type != HYPE_VMX_CR_ACCESS_TYPE_MOV_TO_CR) {
        return 0;
    }
    if (real == 0 || gpr >= 16u) {
        return 0;
    }
    value = real->gprs[gpr];

    if (crn == 4u) {
        /* Hardware keeps VMXE; the guest sees precisely what it wrote. */
        (void)vmwrite(HYPE_VMCS_GUEST_CR4, value | HYPE_VMX_CR4_VMXE);
        (void)vmwrite(HYPE_VMCS_CR4_READ_SHADOW, value);
    } else if (crn == 0u) {
        (void)vmwrite(HYPE_VMCS_GUEST_CR0, value | HYPE_VMX_CR0_NE);
        (void)vmwrite(HYPE_VMCS_CR0_READ_SHADOW, value);
        /* PG may just have changed: re-derive long-mode state before the next
         * entry, or entry fails its guest-state checks (#248). */
        vmx_sync_long_mode();
    } else {
        /* CR3 loads are not host-owned (no bit in the CR3 target list here) and
         * CR8 does not apply -- do not pretend to have handled them. */
        return 0;
    }

    vmx_advance_rip();
    return 1;
}

void hype_vmx_vcpu_dump_ept_violation(hype_vcpu_ctx_t *ctx) {
    int ok;
    (void)ctx;
    /* Read the reason back from the VMCS rather than trusting a value threaded
     * through the caller: the whole point is to find out whether this exit really
     * is an EPT violation (48) or something else that reached the NPF path. */
    hype_debug_print("vmx EPTDUMP: reason=%llu qual=0x%llx gpa=0x%llx gla=0x%llx rip=0x%llx\n",
                     (unsigned long long)vmread(HYPE_VMCS_VM_EXIT_REASON, &ok),
                     (unsigned long long)vmread(HYPE_VMCS_EXIT_QUALIFICATION, &ok),
                     (unsigned long long)vmread(HYPE_VMCS_GUEST_PHYSICAL_ADDRESS, &ok),
                     (unsigned long long)vmread(HYPE_VMCS_GUEST_LINEAR_ADDRESS, &ok),
                     (unsigned long long)vmread(HYPE_VMCS_GUEST_RIP, &ok));
    /*
     * #251: the segment bases a kernel per-CPU access depends on. Printed because
     * the #GP on `MOV RAX, GS:[0x28]` is only explained by GS base being wrong,
     * and the value distinguishes the possibilities: 0 would fault as #PF
     * (canonical but unmapped), so a #GP implies a NON-CANONICAL base, i.e. stale
     * garbage rather than a plausible address. host_kgsbase is what SWAPGS would
     * install if the guest shares the host's IA32_KERNEL_GS_BASE -- which it does,
     * since hype neither models that MSR nor uses the entry/exit MSR areas.
     */
    hype_debug_print("vmx EPTDUMP: fs_base=0x%llx gs_base=0x%llx host_kgsbase=0x%llx "
                     "gs_sel=0x%llx gs_ar=0x%llx gs_lim=0x%llx fs_ar=0x%llx ss_ar=0x%llx\n",
                     (unsigned long long)vmread(HYPE_VMCS_GUEST_FS_BASE, &ok),
                     (unsigned long long)vmread(HYPE_VMCS_GUEST_GS_BASE, &ok),
                     (unsigned long long)rdmsr(0xC0000102u),
                     (unsigned long long)vmread(HYPE_VMCS_GUEST_GS_SELECTOR, &ok),
                     (unsigned long long)vmread(HYPE_VMCS_GUEST_GS_AR_BYTES, &ok),
                     (unsigned long long)vmread(HYPE_VMCS_GUEST_GS_LIMIT, &ok),
                     (unsigned long long)vmread(HYPE_VMCS_GUEST_FS_AR_BYTES, &ok),
                     (unsigned long long)vmread(HYPE_VMCS_GUEST_SS_AR_BYTES, &ok));
    hype_debug_print("vmx EPTDUMP: cr0=0x%llx cr3=0x%llx cr4=0x%llx efer=0x%llx "
                     "entry_ctls=0x%llx rflags=0x%llx cs=0x%llx cs_base=0x%llx\n",
                     (unsigned long long)vmread(HYPE_VMCS_GUEST_CR0, &ok),
                     (unsigned long long)vmread(HYPE_VMCS_GUEST_CR3, &ok),
                     (unsigned long long)vmread(HYPE_VMCS_GUEST_CR4, &ok),
                     (unsigned long long)vmread(HYPE_VMCS_GUEST_IA32_EFER, &ok),
                     (unsigned long long)vmread(HYPE_VMCS_VM_ENTRY_CONTROLS, &ok),
                     (unsigned long long)vmread(HYPE_VMCS_GUEST_RFLAGS, &ok),
                     (unsigned long long)vmread(HYPE_VMCS_GUEST_CS_SELECTOR, &ok),
                     (unsigned long long)vmread(HYPE_VMCS_GUEST_CS_BASE, &ok));
}

void hype_vmx_vcpu_get_cr_diag(hype_vcpu_ctx_t *ctx, unsigned gpr, hype_vmx_cr_diag_t *out) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    int ok;

    if (out == 0) {
        return;
    }
    out->attempted = (real != 0 && gpr < 16u) ? real->gprs[gpr] : 0;
    out->guest_cr0 = vmread(HYPE_VMCS_GUEST_CR0, &ok);
    out->guest_cr4 = vmread(HYPE_VMCS_GUEST_CR4, &ok);
    out->cr0_mask = vmread(HYPE_VMCS_CR0_GUEST_HOST_MASK, &ok);
    out->cr4_mask = vmread(HYPE_VMCS_CR4_GUEST_HOST_MASK, &ok);
    out->cr0_shadow = vmread(HYPE_VMCS_CR0_READ_SHADOW, &ok);
    out->cr4_shadow = vmread(HYPE_VMCS_CR4_READ_SHADOW, &ok);
    out->cr0_fixed0 = rdmsr(HYPE_MSR_IA32_VMX_CR0_FIXED0);
    out->cr0_fixed1 = rdmsr(HYPE_MSR_IA32_VMX_CR0_FIXED1);
    out->cr4_fixed0 = rdmsr(HYPE_MSR_IA32_VMX_CR4_FIXED0);
    out->cr4_fixed1 = rdmsr(HYPE_MSR_IA32_VMX_CR4_FIXED1);
}
