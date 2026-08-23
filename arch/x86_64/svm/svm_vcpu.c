#include "svm.h"
#include "../cpu/fpu_state.h"
#include "../cpu/hyperv.h"

#include "../../../core/guest_mem.h"

#include "../../../core/fatal.h"

#include "../../../devices/pvclock.h"
#include "../../../devices/acpi.h" /* #518: HYPE_ACPI_RESET_PORT owns 0xCF9 */

/* Defined below (exempt real-hardware helper); forward-declared because the
 * #436 CALTRACE sites in the IOIO handler run earlier in the file. */
static inline uint64_t real_rdtsc(void);

/*
 * Concrete per-vCPU context for the SVM backend (M2-7). Opaque outside
 * this file per vmm_ops.h's hype_vcpu_ctx_t contract -- the dispatch
 * loop and device model only ever see the pointer. Single static
 * instance: M2's scope is one vCPU (the hand-written M2-7 test guest);
 * real multi-vCPU allocation is M8's job.
 */
struct hype_vcpu_ctx {
    hype_vmcb_t *vmcb;
    /* Not VMCB fields -- VMRUN only loads/saves RAX/RSP/RIP/RFLAGS from
     * the VMCB; every other GPR just passes through whatever value was
     * loaded immediately before VMRUN (see vmrun_full() below), and
     * guest code can freely modify any of them before the next
     * #VMEXIT. vmrun_full() loads every slot here into the matching
     * real register immediately before VMRUN (so e.g. a guest can rely
     * on RSI holding the Linux boot protocol's zero-page address at
     * entry, M3-5) and captures the guest's post-exit value back here
     * immediately after (needed to read a write's source register or
     * patch a read's destination register during MMIO emulation,
     * hype_svm_vcpu_handle_npf() below, M4-3).
     * Indexed by x86-64 register encoding (0=RAX,1=RCX,2=RDX,3=RBX,
     * 4=RSP,5=RBP,6=RSI,7=RDI,8-15=R8-R15); index 0 and 4 are never
     * read/written here (RAX lives in vmcb->save.rax, which VMRUN
     * itself manages; RSP is restored automatically by VMRUN's own
     * host-state save/restore, and no guest register-encoded MMIO
     * operand can legally be RSP anyway) -- left as unused slots rather
     * than a compacted array purely so every other index matches the
     * raw ModRM.reg encoding hype_mmio_decode() reports directly,
     * avoiding a translation table in the NPF/MMIO decode path. */
    uint64_t gprs[16];
    /* #260: the guest's x87/SSE state. SVM does not save it in the VMCB and
     * hype's own handlers use XMM, so it is saved/restored around VMRUN here.
     * Per-vCPU, not file-global: two guests run concurrently on two cores, and a
     * shared area would have each core's exit stomp the other's registers --
     * the same class of bug as #237's shared VMCB slot. */
    hype_fpu_area_t fpu;
    /* PVCLOCK (kvmclock), per-vCPU. M8-0b STEP 2: two guests run concurrently,
     * each with its OWN guest-physical->host map, so the map (and each guest's
     * last KVM SYSTEM_TIME/WALL_CLOCK MSR value) MUST be per-vCPU -- a single
     * shared map made one guest's pvclock writes land in the OTHER guest's RAM,
     * leaving its own page unfilled -> garbage clocksource -> dead-halt. The
     * TSC->ns scale (mul/shift) stays global: all cores share one TSC rate. */
    const hype_gpa_map_t *pvclock_map;
    /*
     * #275: this vCPU's IA32_TSC_AUX. PER-vCPU on purpose: the whole point is that
     * RDTSCP must return THIS guest's CPU encoding, so a shared value would be the
     * same class of bug as #276/#277 and would defeat the fix.
     *
     * SVM has no MSR-load area, unlike VMX where #270 was one list entry. VMSAVE /
     * VMLOAD cover FS, GS, TR, LDTR, KernelGsBase, STAR, LSTAR, CSTAR, SFMASK and the
     * SYSENTER MSRs -- TSC_AUX is not among them -- so it has to be swapped by hand
     * around VMRUN.
     */
    uint64_t tsc_aux;
    int tsc_aux_valid; /* the guest has written it; skip the swap entirely until then */
    uint64_t pvclock_system_msr;
    uint64_t pvclock_wall_msr;
    /*
     * #436: a round-tripping MTRR model. The old stub ignored MTRR writes and returned 0 on
     * reads, but reported MTRRcap with variable MTRRs -- so OVMF's MtrrLib (invoked by Windows
     * winload's SetMemoryAttributes; Linux/BSD never call it) wrote the variable MTRRs, read
     * them back as 0, saw its writes had not taken, and looped in CpuSetMemoryAttributes ->
     * MtrrLibSetMemoryAttributesWorker forever. Storing writes and returning them makes the
     * verify converge. These MTRRs are cosmetic to hype's own NPT memory typing (WB via PAT);
     * they exist so the guest reads back what it wrote. Zeroed at reset. */
    uint64_t mtrr_deftype;   /* IA32_MTRR_DEF_TYPE (0x2FF) */
    uint64_t mtrr_var[16];   /* 8 PHYSBASE/PHYSMASK pairs (0x200..0x20F) */
    uint64_t mtrr_fix[11];   /* 0x250, 0x258, 0x259, 0x268..0x26F */
    /* Deferred-interrupt slot, per-vCPU. M8-0b STEP 2: two guests run
     * concurrently, so a single shared pending-IRQ slot let one guest's
     * deferred vector be overwritten by, or injected into, the OTHER guest ->
     * the owner's IRQ vanished (pending=0) and it wedged waiting on it. One
     * pending vector per vCPU is all this project's single-IRQ-source scope
     * needs (see hype_svm_vcpu_request_interrupt). */
    /* M4-6b2: pending-interrupt IRR bitmap (256 vectors), per-vCPU. Replaces the
     * old single {valid,vector} slot: the run loop can request several vectors
     * (timer + AHCI + serial + PIC) in ONE iteration, and hype can stage only
     * one in EVENTINJ per VMRUN. A single slot -- and request_interrupt's
     * unconditional EVENTINJ overwrite -- dropped every colliding vector but the
     * last, killing a self-re-arming one-shot clockevent that lost its tick.
     * Queue all requests here; drain highest-first, one per VMRUN. */
    uint32_t pending_irr[8];
    /*
     * #512: which pending_irr bits came from the legacy-PIC acknowledge path. The #455 pruner
     * used to cancel any pending vector that NUMERICALLY mapped to a masked PIC line -- but an
     * APIC-mode Linux guest leaves the (fully masked) PIC based at 0x20 while allocating
     * IO-APIC vectors 0x21+, so every deferred keyboard (0x21) and COM1 (0x22) interrupt was
     * silently cancelled as a "masked PIC vector". Measured: 2122 requested, 162 injected,
     * the rest pruned. Only a bit set HERE may be pruned on the PIC's behalf.
     */
    uint32_t pending_pic[8];
    /* M7-1 (#91): the guest's Hyper-V OS identity and hypercall-page MSR values.
     * Per-vCPU for the same reason pvclock_map is: two guests run concurrently and
     * each writes its own. Stored so a read returns what was written; hype services
     * no hypercalls through the page (#300). */
    uint64_t hv_guest_os_id;
    uint64_t hv_hypercall;
    uint64_t hv_ref_tsc; /* #436: HV_X64_MSR_REFERENCE_TSC readback value */
    /* M7-1 (#91): does THIS guest see the Hyper-V hypervisor identity? Per-vCPU, not
     * file-global: VM0 may be Windows while VM1 is Linux, and the two cores take
     * CPUID exits concurrently. */
    int hv_enabled;
    /* #359: per-vector interrupt accounting, per-vCPU. The file-global version
     * summed both guests, so the one diagnostic lead #359 has (a requested-vs-
     * injected gap on one vector) could not be attributed to a VM. */
    uint32_t int_req_by_vec[256];
    uint32_t int_inj_by_vec[256];
    /*
     * #563: the injection-outcome counters, PER vCPU. These were four file-globals
     * (g_int_eventinj / g_int_vintr_defer / g_int_vintr_window / g_int_defer_overwrite) plus a
     * collision count, summed over every vCPU of every VM -- so the counter that would identify
     * WHICH guest is losing an injection was precisely the one that could not. Same reasoning as
     * int_req_by_vec/int_inj_by_vec two lines up, which #359 already moved for the same reason.
     *
     * The VMX backend increments these through the same `real` pointer, so both vendors report
     * the same thing per vCPU and the INTDIAG line keeps meaning one thing on both.
     */
    unsigned long long int_eventinj;  /* accepted immediately (direct EVENTINJ / entry-intr-info) */
    unsigned long long int_defer;     /* could not accept -> queued in the IRR */
    unsigned long long int_window;    /* interrupt window fired -> deferred inject drained */
    unsigned long long int_overwrite; /* vector already pending -> coalesced, not lost */
    unsigned long long int_collision; /* wanted to inject, an event was already staged */
    /*
     * #456: the vectors staged into EVENTINJ since the caller last drained this,
     * as a 256-bit set. The guest's emulated LAPIC ISR must be marked at the
     * moment a vector is COMMITTED to the guest, not when it is requested --
     * requests can be deferred for a long time, cancelled (#455), or coalesced,
     * and a vector re-injected out of pending_irr after the guest EOI'd an
     * earlier delivery gets no fresh request at all. Marking at request time
     * left FreeBSD's `bsr ISR1` stub reading a stale low bit and calling
     * lapic_handle_intr() with a PIC-range vector, which indexes
     * la_ioint_irqs[vector - 48] with a huge unsigned value and page-faults.
     */
    uint32_t inj_notify[8];
    /* SMP-2 (#186): the topology THIS vCPU's guest sees. Per-vCPU, not file-global -- apic_id
     * differs between vCPUs of one VM, and two VMs with different vcpu_counts take CPUID exits
     * concurrently on two cores (the #237/#276 shared-singleton class). */
    hype_cpuid_topology_t cpuid_topo;
};

/* M8-0b-ii: per-vCPU state pool. Was a single g_vmcb/g_ctx (M2's one-vCPU
 * scope); now one slot per concurrent vCPU so a second guest can run on the AP
 * (VM0 on the BSP = slot 0, VM1 on the AP = slot 1). VMCB is architecturally
 * 4KB, so aligning the array to 4KB keeps every element page-aligned (the
 * _Static_assert guards that). iopm/msrpm below stay shared -- they are
 * read-only permission maps and every guest wants the same policy. */
/*
 * #412 step 2: the VMCB and vCPU-ctx pools are runtime-allocated and sized to
 * the VM count (hype_svm_vcpu_pool_alloc, called from boot before any vCPU is
 * created), not a fixed HYPE_SVM_MAX_VCPUS array -- so N VMs get N distinct
 * VMCB/ctx pairs (#237: two vCPUs must never share one VMCB). The VMCB pool is
 * page-allocated because a VMCB is architecturally 4 KiB; sizeof==4096 keeps
 * every element page-aligned off the page-aligned base.
 */
static hype_vmcb_t *g_vmcb_pool;
static struct hype_vcpu_ctx *g_ctx_pool;
static unsigned g_svm_pool_n;
static unsigned g_vcpu_count;
_Static_assert(sizeof(hype_vmcb_t) == 4096, "VMCB must be 4KB for per-element page alignment");

void hype_svm_vcpu_pool_alloc(unsigned count, uint64_t (*alloc_zeroed_pages)(unsigned pages)) {
    unsigned ctx_pages = (unsigned)((count * sizeof(struct hype_vcpu_ctx) + 4095u) / 4096u);
    if (count == 0u) count = 1u;
    g_vmcb_pool = (hype_vmcb_t *)(uintptr_t)alloc_zeroed_pages(count);   /* 1 page per VMCB */
    g_ctx_pool = (struct hype_vcpu_ctx *)(uintptr_t)alloc_zeroed_pages(ctx_pages ? ctx_pages : 1u);
    g_svm_pool_n = count;
    g_vcpu_count = 0u;
}

/* Allocates the next vCPU slot. The two concurrent guests take slots 0 and 1;
 * the gated-off self-test guests run sequentially and safely reuse the last
 * slot beyond that. M8-0b STEP 2: two guests now create their vCPUs on two
 * different cores (AP1, AP2) at essentially the same instant, so the counter
 * bump MUST be atomic -- a plain read-modify-write could hand both cores slot
 * 0, i.e. two cores VMRUNning the same VMCB/ctx pair (memory corruption). A
 * lock-free fetch-add gives each caller a distinct index regardless of timing. */
/*
 * #244: give this vCPU its own ASID, and flush that ASID's stale entries once.
 *
 * AMD-V decides a TLB hit from the ASID tag plus the linear page frame; the nested
 * paging root does NOT participate. Two guests sharing ASID 1 with different nCR3 --
 * which is what hype did until now -- can therefore alias each other's translations.
 * That is the real difference from the VMX side, where EP4TA is part of the tag and
 * distinct EPT roots kept guests apart on their own (see #273, which turned out not
 * to be a correctness fix at all).
 *
 * TLB_CONTROL is set to flush-this-guest for the first entry, because a pool slot --
 * and therefore an ASID -- is recycled across guests, and a reused ASID must not
 * inherit its predecessor's translations. hype_svm_vcpu_run() clears it after the
 * first VMRUN so the flush does not repeat on every entry.
 */
/* #244: which pool slot a ctx came from, so a RESET path (which reuses an existing
 * VMCB and has no slot in hand) still assigns that vCPU's own ASID. Mirrors
 * vmx_ctx_slot() from #276. */
static unsigned svm_ctx_slot(const struct hype_vcpu_ctx *ctx) {
    if (ctx >= &g_ctx_pool[0] && ctx < &g_ctx_pool[g_svm_pool_n]) {
        return (unsigned)(ctx - &g_ctx_pool[0]);
    }
    return 0;
}

static void svm_assign_asid(hype_vmcb_t *vmcb, unsigned slot) {
    uint32_t eax, ebx, ecx, edx;
    uint32_t nasid;
    uint32_t asid;

    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x8000000Au));
    (void)eax; (void)ecx; (void)edx;
    nasid = hype_svm_nasid_from_cpuid_ebx(ebx);
    asid = hype_svm_asid_for_slot(slot, nasid);
    if (asid == 0u) {
        /* Nothing downstream can detect a guest silently sharing the HOST's ASID
         * tag, so it has to be said here. */
        hype_debug_print("svm: CPU reports NASID=%u -- no usable guest ASID; guest would share "
                         "the host's TLB tag (#244)\n",
                         (unsigned)nasid);
        return;
    }
    vmcb->control.guest_asid_tlb_ctl =
        (uint64_t)asid | ((uint64_t)HYPE_SVM_TLB_CTL_FLUSH_GUEST << 32);
    hype_debug_print("svm: slot %u -> ASID %u (NASID=%u), flush-this-guest armed (#244)\n", slot,
                     (unsigned)asid, (unsigned)nasid);
}

static unsigned svm_alloc_vcpu_slot(void) {
    unsigned slot = __atomic_fetch_add(&g_vcpu_count, 1u, __ATOMIC_SEQ_CST);
    if (slot < g_svm_pool_n) {
        return slot;
    }
    /*
     * #237: exhaustion used to clamp SILENTLY, which is how this bug hid. The
     * clamp is only safe for guests that run strictly sequentially, and since
     * #534 retired the in-binary battery NOTHING here does: every VM is
     * concurrent. If two CONCURRENT vCPUs ever land on one slot they
     * VMRUN the same VMCB and ctx from two cores, and the second guest reads
     * garbage -- the exact corruption the atomic above exists to prevent.
     * Nothing downstream can detect that, so say it loudly here.
     */
    hype_debug_print("svm: vCPU slot pool EXHAUSTED (%u slots) -- slot %u aliased to %u. Safe ONLY "
                     "if these guests never run concurrently (see #237)\n",
                     g_svm_pool_n, slot, g_svm_pool_n - 1u);
    return g_svm_pool_n - 1u;
}

/* AMD SDM: 12KB I/O permission map, 8KB MSR permission map -- VMRUN
 * always consults both, for every guest, regardless of whether it
 * ever executes I/O or RDMSR/WRMSR/etc. All-zero (this array's default,
 * BSS-zeroed) would mean "intercept nothing"; both hype_svm_vcpu_create()
 * (real-mode, FW-1 onward) and hype_svm_vcpu_create_long_mode() (M3-5)
 * fill every byte with 0xFF first (see each function's own comment) so
 * every port actually traps instead of silently reaching real
 * hardware. */
static uint8_t g_iopm[12288] __attribute__((aligned(4096)));
static uint8_t g_msrpm[8192] __attribute__((aligned(4096)));

static inline void clgi(void) {
    __asm__ volatile("clgi" ::: "memory");
}

static inline void stgi(void) {
    __asm__ volatile("stgi" ::: "memory");
}

static inline void vmload(uint64_t vmcb_phys) {
    __asm__ volatile("vmload %%rax" : : "a"(vmcb_phys) : "memory");
}

/*
 * VMRUN transfers control to guest code, which can freely modify ANY
 * general-purpose register before the next #VMEXIT -- not just RAX
 * (the VMCB-managed one) or a single register some earlier, narrower
 * version of this function happened to care about. A plain input-only
 * constraint does not by itself tell the compiler a register is
 * clobbered by the instruction -- without saying so, the compiler
 * could keep some *other* live C value (e.g. the caller's `real`
 * pointer) in one of the registers guest code actually stomps,
 * silently corrupting it once the guest runs; confirmed the hard way
 * once already (M3-5), when exactly this gap made
 * `real->vmcb->control.exitcode` read plausible-looking garbage
 * instead of a real SVM exit code, once the guest actually ran code
 * that touched RSI.
 *
 * M4-3 needs strictly more than that fix: MMIO emulation
 * (hype_svm_vcpu_handle_npf() below) must be able to read a write's
 * source register and patch a read's destination register, for *any*
 * GPR the compiled guest code happens to use -- not just detect that
 * one specific register was clobbered. So every GPR this project can
 * reach (RCX/RDX/RBX/RBP/RSI/RDI/R8-R15; RAX excepted, since VMCB
 * already manages it via save.rax; RSP excepted, since VMRUN's own
 * host-state save/restore keeps the *host's* RSP valid across the
 * transition and no legal MMIO operand register is ever RSP) is
 * loaded from g_ctx.gprs[] into the real register immediately before
 * VMRUN and captured back immediately after, via direct "+m" memory
 * operands referencing the file-scope g_ctx directly (safe and
 * simple specifically because there is only ever one static instance,
 * per this backend's single-vCPU scope) -- and, same as the RAX/RSI
 * fix before it, every one of those registers is ALSO listed in the
 * clobber list: the "+m" operand tells the compiler the *memory* may
 * change, the clobber tells it the *register* is destroyed, and both
 * are needed since this template uses each register as fixed scratch
 * space the compiler's own register allocator has no visibility into.
 */
/*
 * #412: guest entry moved to an external asm trampoline (arch/x86_64/svm/svm_run.S),
 * modelled on vmx_run.S. It stashes the ctx pointer on the stack across the guest,
 * so -- unlike the former static-operand HYPE_VMRUN_BODY macro -- the ctx pool may
 * be runtime-allocated and any size. The guest RAX/RSP/RFLAGS live in the VMCB, so
 * the trampoline saves/restores only RCX RDX RBX RBP RSI RDI R8-R15.
 */
void hype_svm_vmrun(struct hype_vcpu_ctx *ctx, uint64_t vmcb_phys);

static inline void vmrun_full(struct hype_vcpu_ctx *ctx, uint64_t vmcb_phys) {
    hype_svm_vmrun(ctx, vmcb_phys);
}

static inline void vmsave(uint64_t vmcb_phys) {
    __asm__ volatile("vmsave %%rax" : : "a"(vmcb_phys) : "memory");
}

static void reset_gprs(struct hype_vcpu_ctx *ctx) {
    unsigned i;
    for (i = 0; i < 16; i++) {
        ctx->gprs[i] = 0;
    }
    /* #260: a zeroed FXSAVE image would set MXCSR=0, unmasking every SIMD
     * exception; load the architectural reset image instead. */
    hype_fpu_area_reset(&ctx->fpu);
    /* Also clear the per-vCPU pvclock state (M8-0b STEP 2): a slot reused by a
     * later guest must not inherit a prior guest's pvclock map/MSR values. */
    ctx->pvclock_map = 0;
    /* #275: a recycled slot must not inherit the previous guest's CPU encoding --
     * RDTSCP would then report the wrong CPU to the new guest. */
    ctx->tsc_aux = 0;
    ctx->tsc_aux_valid = 0;
    ctx->pvclock_system_msr = 0;
    ctx->pvclock_wall_msr = 0;
    {
        /*
         * #436: default MTRR state = "all memory Write-Back, MTRRs enabled". hype forces WB for
         * all guest RAM via the NPT/PAT, so the guest's MTRR view must AGREE: MTRRdefType with
         * E (bit 11) set and default type WB (6) -> 0x806. Starting from 0 instead
         * (MTRRs disabled, default type UC) told a guest that reads MTRRs -- FreeBSD does, Linux/
         * BSD via the MADT do not -- that ALL memory is uncached, and FreeBSD panicked/reset.
         * Fixed MTRRs default to WB (0x06 per byte) for the same reason. Variable MTRRs stay 0
         * (disabled, mask.V=0): with the default already WB, none are needed.
         */
        unsigned mi;
        /*
         * #481: E=1, type=WB, and FE (bit 10) CLEAR. FE used to be set, which a DEBUG OVMF
         * rejects outright -- "ASSERT MemDetect.c: (MtrrSettings.MtrrDefType & 0x400) == 0" --
         * halting the guest firmware in PEI before MP init ever runs. RELEASE builds compile
         * the assert out, so it stayed invisible until a DEBUG firmware was booted.
         *
         * Clearing FE does not change the effective memory type: with the fixed ranges not
         * consulted, the low 1MB falls through to the WB default, which is what the all-WB
         * mtrr_fix[] below already produced. The WB default itself is load-bearing and stays
         * -- see the comment above on FreeBSD panicking when all memory read as uncached.
         */
        ctx->mtrr_deftype = 0x0806u;
        for (mi = 0; mi < 16u; mi++) ctx->mtrr_var[mi] = 0;
        for (mi = 0; mi < 11u; mi++) ctx->mtrr_fix[mi] = 0x0606060606060606ull; /* all WB */
    }
    ctx->hv_guest_os_id = 0;
    ctx->hv_hypercall = 0;
    ctx->hv_ref_tsc = 0;
    ctx->hv_enabled = 0;
    /* SMP-2: a recycled slot must not inherit the previous guest's topology either. */
    ctx->cpuid_topo.apic_id = 0u;
    ctx->cpuid_topo.vcpu_count = 1u;
    ctx->cpuid_topo.threads_per_core = 1u;
    {
        int i;
        for (i = 0; i < 8; i++) {
            ctx->pending_irr[i] = 0;
            ctx->pending_pic[i] = 0; /* #512 */
            ctx->inj_notify[i] = 0; /* #456 */
        }
        /* #563: and the injection-outcome counters, for the same reason -- a recycled slot
         * reporting the previous guest's totals is exactly the mis-attribution this moved them
         * per-vCPU to prevent. */
        ctx->int_eventinj = 0;
        ctx->int_defer = 0;
        ctx->int_window = 0;
        ctx->int_overwrite = 0;
        ctx->int_collision = 0;
        for (i = 0; i < 256; i++) {
            ctx->int_req_by_vec[i] = 0;   /* #359: a recycled slot must not inherit */
            ctx->int_inj_by_vec[i] = 0;   /* the previous guest's interrupt history */
        }
    }
}

/* MSRs that VMSAVE/VMLOAD save+restore around VMRUN (AMD APM Vol 2
 * 15.5.2): FS/GS/KernelGS base, the SYSCALL MSRs (STAR/LSTAR/CSTAR/
 * SFMASK) and the SYSENTER MSRs. Because hype_svm_vcpu_run() vmload/
 * vmsaves the *guest* VMCB around VMRUN, the guest's values for these
 * live in per-guest VMCB state -- and hype itself never uses them --
 * so the guest can read/write them natively with no #VMEXIT, fully
 * isolated. Intercepting them (the CPUMSR-2 blanket 0xFF default) would
 * both cost an exit per access and break a real guest unless each were
 * emulated: a Linux kernel writes GS_BASE in early boot and immediately
 * depends on %gs-relative percpu accesses, so dropping the write faults
 * the kernel instantly. PAT is deliberately NOT here -- it is not in
 * the VMSAVE set, so a native guest PAT write would corrupt the host's
 * PAT; it stays intercepted and emulated into the VMCB's own g_pat. */
static const uint32_t g_msrpm_passthrough[] = {
    0xC0000100u, /* FS_BASE */
    0xC0000101u, /* GS_BASE */
    0xC0000102u, /* KERNEL_GS_BASE */
    0xC0000081u, /* STAR */
    0xC0000082u, /* LSTAR */
    0xC0000083u, /* CSTAR */
    0xC0000084u, /* SFMASK */
    0x00000174u, /* SYSENTER_CS */
    0x00000175u, /* SYSENTER_ESP */
    0x00000176u, /* SYSENTER_EIP */
};

/* Clears the read+write intercept bits for one MSR in the 8KB MSRPM.
 * MSRPM layout (AMD APM 15.11): three covered ranges, 2 bits/MSR
 * (bit0=read, bit1=write). Range 0 (0..0x1FFF) at byte 0, range 1
 * (0xC0000000..) at byte 0x800, range 2 (0xC0010000..) at byte 0x1000.
 * An MSR outside all three ranges is left as-is (still intercepted). */
static void msrpm_clear_intercept(uint8_t *msrpm, uint32_t msr) {
    uint32_t byte_off;
    uint32_t bit_in_byte;
    if (msr < 0x2000u) {
        byte_off = msr / 4u;
        bit_in_byte = (msr % 4u) * 2u;
    } else if (msr >= 0xC0000000u && msr < 0xC0002000u) {
        uint32_t idx = msr - 0xC0000000u;
        byte_off = 0x800u + idx / 4u;
        bit_in_byte = (idx % 4u) * 2u;
    } else if (msr >= 0xC0010000u && msr < 0xC0012000u) {
        uint32_t idx = msr - 0xC0010000u;
        byte_off = 0x1000u + idx / 4u;
        bit_in_byte = (idx % 4u) * 2u;
    } else {
        return;
    }
    msrpm[byte_off] &= (uint8_t) ~(0x3u << bit_in_byte);
}

/* Fills the MSRPM to intercept everything (guest-isolation default),
 * then opens the VMSAVE/VMLOAD-managed passthrough set above. */
static void configure_guest_msrpm(uint8_t *msrpm) {
    unsigned i;
    for (i = 0; i < 8192u; i++) {
        msrpm[i] = 0xFFu;
    }
    for (i = 0; i < sizeof(g_msrpm_passthrough) / sizeof(g_msrpm_passthrough[0]); i++) {
        msrpm_clear_intercept(msrpm, g_msrpm_passthrough[i]);
    }
}

hype_vcpu_ctx_t *hype_svm_vcpu_create(uint64_t guest_rip, uint64_t guest_rsp, uint64_t ept_or_npt_root) {
    unsigned i;
    unsigned slot = svm_alloc_vcpu_slot();
    hype_vmcb_t *vmcb = &g_vmcb_pool[slot];
    struct hype_vcpu_ctx *ctx = &g_ctx_pool[slot];

    /* FW-1: this guest now sets HYPE_SVM_INTERCEPT_IOIO_PROT too
     * (hype_vmcb_build_realmode_guest()) -- a real firmware guest does
     * real port I/O, unlike every prior real-mode test guest. Same "the
     * bit only enables interception, the bitmap decides per-port"
     * reasoning hype_svm_vcpu_create_long_mode() already documents:
     * all-zero would silently let every guest IN/OUT reach real
     * hardware despite the intercept bit being set. */
    for (i = 0; i < sizeof(g_iopm); i++) {
        g_iopm[i] = 0xFFu;
    }

    /* CPUMSR-2: intercept every MSR (isolation default), then open the
     * VMSAVE/VMLOAD-managed passthrough set so a real guest (FW-1's
     * Linux) can use FS/GS base + syscall/sysenter MSRs natively. */
    configure_guest_msrpm(g_msrpm);

    hype_vmcb_build_realmode_guest(vmcb, guest_rip, guest_rsp, (uint64_t)(uintptr_t)g_iopm,
                                    (uint64_t)(uintptr_t)g_msrpm);
    /* AFTER the build: it zeroes the VMCB and hardcodes ASID 1, so assigning before
     * would be silently overwritten (#244). */
    svm_assign_asid(vmcb, slot);

    /* 0 means "no nested paging" (M2's original, still-supported
     * scope) -- a real NPT root is always a nonzero, page-aligned
     * physical address. See vmcb.h's HYPE_SVM_INT_CTL_AVIC_ENABLE
     * comment: AVIC additionally requires this to have been called. */
    if (ept_or_npt_root != 0) {
        hype_vmcb_enable_nested_paging(vmcb, ept_or_npt_root);
    }

    ctx->vmcb = vmcb;
    reset_gprs(ctx);
    return ctx;
}

int hype_svm_vcpu_handle_pm1_cnt_ioio(hype_vcpu_ctx_t *ctx, uint16_t port, uint16_t *value,
                                      int *slp_en) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_ioio_t io;
    hype_svm_decode_ioio_info1(real->vmcb->control.exitinfo1, &io);
    if (io.port != port) {
        return -1;
    }
    if (io.is_in) {
        /* SLP_EN (bit 13) is write-only -- always reads 0. */
        real->vmcb->save.rax =
            (real->vmcb->save.rax & ~0xFFFFULL) | ((uint64_t)(*value) & 0xDFFFu);
        real->vmcb->save.rip = real->vmcb->control.exitinfo2;
        return 1;
    }
    {
        uint16_t w = (uint16_t)(real->vmcb->save.rax & 0xFFFFu);
        *slp_en = (w & (1u << 13)) ? 1 : 0;
        *value = (uint16_t)(w & ~(uint16_t)(1u << 13)); /* store without SLP_EN */
    }
    real->vmcb->save.rip = real->vmcb->control.exitinfo2;
    return 0;
}

/*
 * #94: the 0xCF9 reset-control port (the FADT's reset register). A write with
 * bit 2 (RST_CPU) set is a platform-reset request; reads return 0. Returns 0
 * for a handled write (with *reset_requested set), 1 for a handled read, -1
 * when the exit is not this port's.
 */
int hype_svm_vcpu_handle_reset_ctl_ioio(hype_vcpu_ctx_t *ctx, uint16_t port, int *reset_requested) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_ioio_t io;
    hype_svm_decode_ioio_info1(real->vmcb->control.exitinfo1, &io);
    if (io.port != port) {
        return -1;
    }
    if (io.is_in) {
        real->vmcb->save.rax = (real->vmcb->save.rax & ~0xFFULL);
        real->vmcb->save.rip = real->vmcb->control.exitinfo2;
        return 1;
    }
    *reset_requested = ((real->vmcb->save.rax & 0x04u) != 0) ? 1 : 0;
    real->vmcb->save.rip = real->vmcb->control.exitinfo2;
    return 0;
}

/*
 * #436: PM1a EVENT block (status @base, enable @base+2). A non-hardware-reduced
 * ACPI platform has one, and hype's FADT now says so. No event sources are wired
 * to it, so status reads "nothing pending" and write-1-to-clear is a no-op; the
 * enable register round-trips what the guest wrote so a read-back matches.
 * Returns 0 when the access was this block's, -1 otherwise.
 */
int hype_svm_vcpu_handle_pm1_evt_ioio(hype_vcpu_ctx_t *ctx, uint16_t base, uint16_t *enable) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_ioio_t io;
    int is_enable;

    hype_svm_decode_ioio_info1(real->vmcb->control.exitinfo1, &io);
    if (io.port < base || io.port >= (uint16_t)(base + 4u)) {
        return -1;
    }
    is_enable = (io.port >= (uint16_t)(base + 2u));
    if (io.is_in) {
        uint16_t v = is_enable ? *enable : 0u;
        real->vmcb->save.rax = (real->vmcb->save.rax & ~0xFFFFULL) | (uint64_t)v;
    } else if (is_enable) {
        *enable = (uint16_t)(real->vmcb->save.rax & 0xFFFFu);
    }
    real->vmcb->save.rip = real->vmcb->control.exitinfo2;
    return 0;
}

void hype_svm_vcpu_reset_realmode(hype_vcpu_ctx_t *ctx, uint64_t guest_rip, uint64_t guest_rsp,
                                  uint64_t npt_root) {
    unsigned i;
    hype_vmcb_t *vmcb = ctx->vmcb; /* reuse this vCPU's already-allocated slot/VMCB */
    for (i = 0; i < sizeof(g_iopm); i++) {
        g_iopm[i] = 0xFFu;
    }
    configure_guest_msrpm(g_msrpm);
    hype_vmcb_build_realmode_guest(vmcb, guest_rip, guest_rsp, (uint64_t)(uintptr_t)g_iopm,
                                    (uint64_t)(uintptr_t)g_msrpm);
    /* AFTER the build: it zeroes the VMCB and hardcodes ASID 1, so assigning before
     * would be silently overwritten (#244). A reset reuses this vCPU's slot, and its
     * ASID is recycled with it -- hence the flush this re-arms. */
    svm_assign_asid(vmcb, svm_ctx_slot((const struct hype_vcpu_ctx *)ctx));
    if (npt_root != 0) {
        hype_vmcb_enable_nested_paging(vmcb, npt_root);
    }
    reset_gprs(ctx);
}

/*
 * #535: rebuild THIS vCPU as a 64-bit long-mode guest, in place -- the long-mode counterpart of
 * hype_svm_vcpu_reset_realmode above, for `boot = kernel`.
 *
 * A reset rather than hype_svm_vcpu_create_long_mode() because a configured VM's vCPU already
 * exists and already holds this VM's pool slot: creating would take a second slot for the same
 * vCPU, which is #237's failure (two cores, one VMCB) reintroduced by a different route.
 */
void hype_svm_vcpu_reset_longmode(hype_vcpu_ctx_t *ctx, uint64_t guest_rip, uint64_t guest_cr3,
                                  uint64_t guest_rsp, uint64_t npt_root) {
    unsigned i;
    hype_vmcb_t *vmcb;

    if (ctx == 0) {
        return;
    }
    vmcb = ctx->vmcb; /* reuse this vCPU's already-allocated slot/VMCB */
    for (i = 0; i < sizeof(g_iopm); i++) {
        g_iopm[i] = 0xFFu;
    }
    configure_guest_msrpm(g_msrpm);
    hype_vmcb_build_long_mode_guest(vmcb, guest_rip, guest_cr3, guest_rsp,
                                    (uint64_t)(uintptr_t)g_iopm, (uint64_t)(uintptr_t)g_msrpm);
    /* AFTER the build, which zeroes the VMCB and hardcodes ASID 1 -- same ordering trap as the
     * realmode reset (#244). */
    svm_assign_asid(vmcb, svm_ctx_slot((const struct hype_vcpu_ctx *)ctx));
    if (npt_root != 0) {
        hype_vmcb_enable_nested_paging(vmcb, npt_root);
    }
    reset_gprs(ctx);
}

hype_vcpu_ctx_t *hype_svm_vcpu_create_long_mode(uint64_t entry_rip, uint64_t guest_cr3, uint64_t rsp,
                                                 uint64_t npt_root) {
    unsigned i;
    unsigned slot = svm_alloc_vcpu_slot();
    hype_vmcb_t *vmcb = &g_vmcb_pool[slot];
    struct hype_vcpu_ctx *ctx = &g_ctx_pool[slot];

    /* This guest sets HYPE_SVM_INTERCEPT_IOIO_PROT (unlike the
     * real-mode guest, which never checks the IOPM at all) -- that
     * control bit only *enables* IOIO interception; whether any given
     * port actually traps is decided per-port by the IOPM bitmap
     * itself. All-zero (g_iopm's default, correct for the real-mode
     * guest) means "intercept nothing," which would let every guest
     * IN/OUT reach real hardware directly -- exactly the direct
     * guest-hardware-access this project's guest-isolation invariant
     * forbids (AGENTS.md), confirmed the hard way: without this fill,
     * the guest's port I/O silently reached QEMU's own real emulated
     * PIC/PIT instead of devices/pic.h and devices/pit.h. Filling
     * every byte with 0xFF marks every port as intercepted. */
    for (i = 0; i < sizeof(g_iopm); i++) {
        g_iopm[i] = 0xFFu;
    }

    /* CPUMSR-2: same reasoning as the IOPM fill just above, now for
     * HYPE_SVM_INTERCEPT_MSR_PROT -- intercept all, then open the
     * VMSAVE/VMLOAD-managed passthrough set (harmless for the long-mode
     * test guests, which never touch those MSRs). */
    configure_guest_msrpm(g_msrpm);

    hype_vmcb_build_long_mode_guest(vmcb, entry_rip, guest_cr3, rsp, (uint64_t)(uintptr_t)g_iopm,
                                     (uint64_t)(uintptr_t)g_msrpm);
    svm_assign_asid(vmcb, slot); /* #244 -- after the build, which resets ASID to 1 */

    if (npt_root != 0) {
        hype_vmcb_enable_nested_paging(vmcb, npt_root);
    }

    ctx->vmcb = vmcb;
    reset_gprs(ctx);
    return ctx;
}

void hype_svm_vcpu_set_rsi(hype_vcpu_ctx_t *ctx, uint64_t rsi) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    real->gprs[6] = rsi; /* RSI's index in gprs[] -- see the struct's own comment */
}

void hype_svm_vcpu_set_idt(hype_vcpu_ctx_t *ctx, uint64_t base, uint16_t limit) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    real->vmcb->save.idtr.base = base;
    real->vmcb->save.idtr.limit = limit;
}

void hype_svm_vcpu_set_gdt(hype_vcpu_ctx_t *ctx, uint64_t base, uint16_t limit) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    real->vmcb->save.gdtr.base = base;
    real->vmcb->save.gdtr.limit = limit;
}

/* M4-6d4: turn on SVM PAUSE-filtering for this guest -- intercept PAUSE, but
 * only fire EXITCODE_PAUSE after `count` PAUSEs occur within `threshold`
 * cycles of each other (a spin-loop detector). Lets the hypervisor reclaim
 * control from a guest busy-waiting on cpu_relax without trapping every
 * single PAUSE. Caller must first confirm CPU support
 * (hype_cpu_has_pause_filter) -- without it, INTERCEPT_PAUSE traps EVERY
 * pause. Exempt from unit testing, same as the other VMCB-reaching setters. */
void hype_svm_vcpu_enable_pause_filter(hype_vcpu_ctx_t *ctx, uint16_t count, uint16_t threshold) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    real->vmcb->control.intercept_misc1 |= HYPE_SVM_INTERCEPT_PAUSE;
    real->vmcb->control.pause_filter_count = count;
    real->vmcb->control.pause_filter_threshold = threshold;
    real->vmcb->control.vmcb_clean_bits = 0; /* control area changed */
}

/* RT-2b: intercept physical INTR for this guest so a host periodic-timer tick
 * arriving during VMRUN forces #VMEXIT(EXITCODE_INTR) instead of leaking into
 * the guest. Part of the FW-1 guest's baseline intercept set once hype's own
 * timer is live post-EBS -- the interrupt itself is then taken by the host
 * (hype_timer_isr) when the loop does STGI with host IF=1. Exempt from unit
 * testing, same as the other VMCB-reaching setters. */
void hype_svm_vcpu_enable_intr_intercept(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    real->vmcb->control.intercept_misc1 |= HYPE_SVM_INTERCEPT_INTR;
    /* #436: without V_INTR_MASKING the guest's IF masks the physical tick, so
     * an IF=0 guest spin is unpreemptible and starves the whole run loop.
     * With it, the tick exits regardless of guest IF (host IF=1 at VMRUN). */
    real->vmcb->control.vintr |= HYPE_SVM_VINTR_V_INTR_MASKING;
    real->vmcb->control.vmcb_clean_bits = 0; /* control area changed */
}

void hype_svm_vcpu_set_cs_ss_selectors(hype_vcpu_ctx_t *ctx, uint16_t cs_selector, uint16_t ss_selector) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    real->vmcb->save.cs.selector = cs_selector;
    real->vmcb->save.ss.selector = ss_selector;
}

/*
 * Maps an x86-64 register encoding (as hype_mmio_decode() reports it)
 * to where this backend actually stores that register's live value.
 * NULL for RSP (index 4): never a legal MMIO operand register, and
 * this backend never captures the guest's RSP value at all (VMRUN's
 * own host-state save/restore only concerns the *host's* RSP). RAX
 * (index 0) lives in vmcb->save.rax, the one GPR VMRUN itself manages
 * directly; every other index is g_ctx's own post-VMRUN capture
 * (vmrun_full() above). Pure pointer arithmetic over already-captured
 * state -- no CPU access itself -- but kept in this exempt file since
 * it only makes sense paired with the exempt VMCB/GPR state it reaches
 * into.
 */
static uint64_t *gpr_ptr(struct hype_vcpu_ctx *real, uint8_t reg) {
    if (reg == 4u) {
        return 0;
    }
    if (reg == 0u) {
        return &real->vmcb->save.rax;
    }
    return &real->gprs[reg];
}

int hype_svm_vcpu_handle_ioio(hype_vcpu_ctx_t *ctx, hype_pic_emu_t *pic, hype_pit_emu_t *pit) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_ioio_t io;
    int rc;

    hype_svm_decode_ioio_info1(real->vmcb->control.exitinfo1, &io);

    if (io.port == 0x20u || io.port == 0x21u || io.port == 0xA0u || io.port == 0xA1u) {
        if (io.is_in) {
            uint8_t value = 0;
            rc = hype_pic_emu_io_read(pic, io.port, &value);
            if (rc == 0) {
                real->vmcb->save.rax = (real->vmcb->save.rax & ~0xFFULL) | value;
            }
        } else {
            uint8_t pv = (uint8_t)(real->vmcb->save.rax & 0xFFu);
            /* M4-6d7 DIAG: log OCW1 (IMR) writes whose IRQ4/IRQ3 mask bits
             * CHANGE -- shows whether Linux ever wires the serial IRQ through
             * the 8259 instead of the IO-APIC in APIC mode. */
            if (io.port == 0x21u) {
                uint8_t old = pic->master.imr;
                if (((old ^ pv) & 0x1Au) != 0u) { /* IRQ1/3/4 mask-bit change */
                    static unsigned imr_log_n = 0;
                    if (imr_log_n < 32u) {
                        imr_log_n++;
                        hype_debug_print("fw-1 PICIMR 0x%x->0x%x rip=0x%llx\n", (unsigned)old,
                                         (unsigned)pv, (unsigned long long)real->vmcb->save.rip);
                    }
                }
            }
            /*
             * #455: ICW1 (command port, bit4 set) begins a full 8259 reinitialisation --
             * both the loader and, later, the kernel each do this as they take ownership
             * of the interrupt controller. hype's own request_interrupt() translates an
             * acknowledged IRQ line into a CPU vector and queues it in pending_irr the
             * instant the guest can't yet accept it (IF=0) -- eagerly, at acknowledge
             * time, not at delivery time. A PIC reinit resets the CHIP's own IRR/IMR
             * (hype_pic_emu_reset(), via chip_write_command() below) but never reached
             * pending_irr, so a vector translated under the OLD irq_offset (raised by,
             * say, the loader's own PS/2 handshake traffic while its IF was 0) survived
             * untouched across the reinit and got delivered late, into whatever the
             * vector now happens to mean under the NEW configuration -- observed as a
             * spurious IRQ1 (vector 0x21, queued during the loader's own 8042 self-test)
             * landing at the KERNEL's first `sti`, before it had registered a real
             * handler for anything, which FreeBSD (correctly, by its own lights) treated
             * as fatal. Capture the pre-reinit vector range on ICW1 and drop anything
             * still queued there -- exactly the discard a real reinit's IRR/IMR reset
             * already models one layer up, just extended to the vector this project
             * translates the IRQ into.
             */
            if ((io.port == 0x20u || io.port == 0xA0u) && (pv & 0x10u) != 0u) {
                uint8_t old_offset = (io.port == 0x20u) ? pic->master.irq_offset
                                                        : pic->slave.irq_offset;
                unsigned i;
                for (i = 0; i < 8u; i++) {
                    hype_svm_irr_clear(real->pending_irr, (uint8_t)(old_offset + i));
                    hype_svm_irr_clear(real->pending_pic, (uint8_t)(old_offset + i)); /* #512 */
                }
            }
            rc = hype_pic_emu_io_write(pic, io.port, pv);
        }
    } else if (io.port >= 0x40u && io.port <= 0x43u) {
        /* #436 CALTRACE: cdboot's TSC calibration reads a hype timing source and
         * concludes a wildly wrong frequency (its Stall() deadlines then never
         * arrive). Trace the first PIT accesses from LOW (Windows) RIPs to see
         * the exact calibration pattern and the values hype served. */
        if (real->vmcb->save.rip < 0x80000000ull) {
            static unsigned cal_n = 0;
#ifdef HYPE_QUIET
            cal_n = 48u;
#endif
            if (cal_n < 48u) {
                cal_n++;
                hype_debug_print("fw-1 #436 CALTRACE pit p=0x%x %s rax=0x%02x rip=0x%llx tsc=0x%llx\n",
                                 (unsigned)io.port, io.is_in ? "IN" : "OUT",
                                 (unsigned)(real->vmcb->save.rax & 0xFFu),
                                 (unsigned long long)real->vmcb->save.rip,
                                 (unsigned long long)real_rdtsc());
            }
        }
        if (io.is_in) {
            uint8_t value = 0;
            rc = hype_pit_emu_io_read(pit, io.port, &value);
            if (rc == 0) {
                real->vmcb->save.rax = (real->vmcb->save.rax & ~0xFFULL) | value;
            }
        } else {
            rc = hype_pit_emu_io_write(pit, io.port, (uint8_t)(real->vmcb->save.rax & 0xFFu));
        }
    } else if (io.port == 0x61u) {
        /* System Control Port B: PIT channel-2 gate (write) + OUT/refresh
         * clock (read). A guest's PIT-based TSC/delay calibration
         * (e.g. Linux pit_calibrate_tsc) sets the ch2 gate here then
         * polls bit 5 for OUT; without it the poll spins forever. */
        if (io.is_in) {
            real->vmcb->save.rax =
                (real->vmcb->save.rax & ~0xFFULL) | hype_pit_emu_port61_read(pit);
        } else {
            hype_pit_emu_port61_write(pit, (uint8_t)(real->vmcb->save.rax & 0xFFu));
        }
        /* #436 CALTRACE: same trace for port 0x61 (ch2 gate/OUT + refresh toggle). */
        if (real->vmcb->save.rip < 0x80000000ull) {
            static unsigned cal61_n = 0;
#ifdef HYPE_QUIET
            cal61_n = 48u;
#endif
            if (cal61_n < 48u) {
                cal61_n++;
                hype_debug_print("fw-1 #436 CALTRACE p61 %s rax=0x%02x rip=0x%llx tsc=0x%llx\n",
                                 io.is_in ? "IN" : "OUT",
                                 (unsigned)(real->vmcb->save.rax & 0xFFu),
                                 (unsigned long long)real->vmcb->save.rip,
                                 (unsigned long long)real_rdtsc());
            }
        }
        rc = 0;
    } else {
        return -1;
    }

    if (rc != 0) {
        return -1;
    }

    /* EXITINFO2 gives the resume RIP directly -- the instruction after
     * the IN/OUT, same "next-RIP-for-free" convenience HLT provides. */
    real->vmcb->save.rip = real->vmcb->control.exitinfo2;
    return 0;
}

/* Runs the real `cpuid` instruction for (eax, ecx). Exempt from unit
 * testing per AGENTS.md -- same reasoning as cpu_features_hw.c's own
 * cpuid() helper (which this deliberately mirrors): the actual
 * decision logic (hype_cpuid_emulate()) is what's tested; this is just
 * the raw leaf read. */
static inline void real_cpuid(uint32_t eax, uint32_t ecx, hype_cpuid_result_t *out) {
    __asm__ volatile("cpuid"
                      : "=a"(out->eax), "=b"(out->ebx), "=c"(out->ecx), "=d"(out->edx)
                      : "a"(eax), "c"(ecx));
}

/*
 * #92 diag: WHICH CPUID leaves / MSR indices a spinning guest is hammering, plus the RIP
 * doing it. An 8-entry key->count MRU per kind, dumped by the 30s diagnostic -- the EXHIST
 * totals say "cpuid 400/s" but not which leaf, and a Windows-boot wedge is indistinguishable
 * from a calibration loop without the number. Counters, not a trace (#356's lesson).
 */
#define HYPE_SPIN_MRU 8u
static uint32_t g_spin_cpuid_key[HYPE_SPIN_MRU];
static uint64_t g_spin_cpuid_cnt[HYPE_SPIN_MRU];
static uint32_t g_spin_msr_key[HYPE_SPIN_MRU];
static uint64_t g_spin_msr_cnt[HYPE_SPIN_MRU];
static uint64_t g_spin_cpuid_rip;
static uint64_t g_spin_msr_rip;

static void spin_mru_bump(uint32_t *keys, uint64_t *cnts, uint32_t key) {
    unsigned i, min_i = 0;
    for (i = 0; i < HYPE_SPIN_MRU; i++) {
        if (cnts[i] != 0u && keys[i] == key) {
            cnts[i]++;
            return;
        }
    }
    for (i = 0; i < HYPE_SPIN_MRU; i++) {
        if (cnts[i] == 0u) {
            keys[i] = key;
            cnts[i] = 1u;
            return;
        }
    }
    for (i = 1; i < HYPE_SPIN_MRU; i++) {
        if (cnts[i] < cnts[min_i]) min_i = i;
    }
    keys[min_i] = key;
    cnts[min_i] = 1u;
}

void hype_svm_vcpu_get_spin_diag(uint32_t *cpuid_keys, uint64_t *cpuid_cnts, uint32_t *msr_keys,
                                 uint64_t *msr_cnts, unsigned n, uint64_t *cpuid_rip,
                                 uint64_t *msr_rip) {
    unsigned i;
    for (i = 0; i < n && i < HYPE_SPIN_MRU; i++) {
        cpuid_keys[i] = g_spin_cpuid_key[i];
        cpuid_cnts[i] = g_spin_cpuid_cnt[i];
        msr_keys[i] = g_spin_msr_key[i];
        msr_cnts[i] = g_spin_msr_cnt[i];
    }
    if (cpuid_rip != 0) *cpuid_rip = g_spin_cpuid_rip;
    if (msr_rip != 0) *msr_rip = g_spin_msr_rip;
}

void hype_svm_vcpu_handle_cpuid(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    uint32_t eax_in = (uint32_t)real->vmcb->save.rax;
    uint32_t ecx_in = (uint32_t)real->gprs[1]; /* RCX */
    hype_cpuid_result_t host_real;
    hype_cpuid_result_t out;

    spin_mru_bump(g_spin_cpuid_key, g_spin_cpuid_cnt, eax_in); /* #92 diag */
    g_spin_cpuid_rip = real->vmcb->save.rip;

    real_cpuid(eax_in, ecx_in, &host_real);
    /* SMP-2 topology + the live guest CR4: CPUID.1:ECX[27] mirrors CR4.OSXSAVE, so it has to
     * be read now rather than cached at vCPU creation. */
    hype_cpuid_emulate_topo(eax_in, ecx_in, real->hv_enabled, &real->cpuid_topo,
                            real->vmcb->save.cr4, &host_real, &out);

    /* CPUID zero-extends all four registers to their full 64-bit width
     * in 64-bit mode -- assigning a uint32_t into a uint64_t field
     * already does that zero-extension. */
    real->vmcb->save.rax = out.eax;
    real->gprs[3] = out.ebx; /* RBX */
    real->gprs[1] = out.ecx; /* RCX */
    real->gprs[2] = out.edx; /* RDX */

    real->vmcb->save.rip += 2; /* CPUID is always exactly 2 bytes (0F A2) */
}

void hype_svm_vcpu_handle_rdtsc(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    uint64_t tsc = real_rdtsc() + real->vmcb->control.tsc_offset;

    real->vmcb->save.rax = (uint64_t)(uint32_t)tsc;
    real->gprs[2] = (uint64_t)(uint32_t)(tsc >> 32);
    real->vmcb->save.rip += 2; /* RDTSC is exactly 0F 31. */
}

/* Runs the real `rdtsc` instruction. Exempt from unit testing, same
 * reasoning as real_cpuid() above. */
static inline uint64_t real_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

void hype_svm_vcpu_get_last_npf(hype_vcpu_ctx_t *ctx, hype_svm_npf_t *out) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_decode_npf_info(real->vmcb->control.exitinfo1, real->vmcb->control.exitinfo2, out);
}

/* PERF-1: decode an IOIO exit's port/direction/size WITHOUT consuming it (no
 * RIP advance, no RAX write) -- lets the FW-1 loop record every I/O exit's port
 * into the per-port histogram at the top of its IOIO branch, before the normal
 * handler cascade runs and advances RIP. Read-only on the VMCB. */
void hype_svm_vcpu_peek_ioio(hype_vcpu_ctx_t *ctx, hype_svm_ioio_t *out) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_decode_ioio_info1(real->vmcb->control.exitinfo1, out);
}

void hype_svm_vcpu_handle_unknown_ioio(hype_vcpu_ctx_t *ctx, hype_svm_ioio_t *out) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;

    hype_svm_decode_ioio_info1(real->vmcb->control.exitinfo1, out);

    if (out->is_in) {
        uint64_t mask =
            (out->size_bytes == 1u) ? 0xFFULL : (out->size_bytes == 2u) ? 0xFFFFULL : 0xFFFFFFFFULL;
        real->vmcb->save.rax = (real->vmcb->save.rax & ~mask) | mask;
    }

    /* EXITINFO2 gives the resume RIP directly, same "next-RIP-for-free"
     * convenience hype_svm_vcpu_handle_ioio() itself already relies on. */
    real->vmcb->save.rip = real->vmcb->control.exitinfo2;
}

int hype_svm_vcpu_handle_pci_cf8_ioio(hype_vcpu_ctx_t *ctx, hype_pci_t *pci) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_ioio_t io;

    hype_svm_decode_ioio_info1(real->vmcb->control.exitinfo1, &io);

    /*
     * #518: CONFIG_ADDRESS spans 0xCF8-0xCFB, and its upper bytes are addressed individually --
     * Linux's pci_check_type1() writes a byte to 0xCFB. 0xCF9 is excluded: within this span it is
     * the chipset reset register (#94), which its own handler owns, exactly as on real hardware.
     */
    if (io.port >= HYPE_PCI_CF8_PORT && io.port <= HYPE_PCI_CF8_PORT + 3u &&
        io.port != HYPE_ACPI_RESET_PORT) {
        unsigned int byte_offset = (unsigned int)(io.port - HYPE_PCI_CF8_PORT);
        if (io.is_in) {
            uint32_t value = hype_pci_cf8_read_bytes(pci, byte_offset, io.size_bytes);
            real->vmcb->save.rax =
                hype_mmio_merge_read_value(real->vmcb->save.rax, value, io.size_bytes, io.size_bytes == 4);
        } else {
            hype_pci_cf8_write_bytes(pci, byte_offset, io.size_bytes,
                                     hype_mmio_extract_write_value(real->vmcb->save.rax,
                                                                   io.size_bytes));
        }
    } else if (io.port >= HYPE_PCI_CFC_PORT && io.port <= HYPE_PCI_CFC_PORT + 3) {
        unsigned int byte_offset = io.port - HYPE_PCI_CFC_PORT;

        if (io.is_in) {
            uint32_t value;
            hype_pci_cf8_config_read(pci, byte_offset, io.size_bytes, &value);
            real->vmcb->save.rax =
                hype_mmio_merge_read_value(real->vmcb->save.rax, value, io.size_bytes, io.size_bytes == 4);
        } else {
            hype_pci_cf8_config_write(pci, byte_offset, io.size_bytes,
                                       hype_mmio_extract_write_value(real->vmcb->save.rax, io.size_bytes));
        }
    } else {
        return -1;
    }

    /* EXITINFO2 gives the resume RIP directly, same "next-RIP-for-free"
     * convenience hype_svm_vcpu_handle_ioio() itself already relies on. */
    real->vmcb->save.rip = real->vmcb->control.exitinfo2;
    return 0;
}

int hype_svm_vcpu_handle_cmos_ioio(hype_vcpu_ctx_t *ctx, hype_cmos_t *cmos) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_ioio_t io;

    hype_svm_decode_ioio_info1(real->vmcb->control.exitinfo1, &io);

    if (io.port == 0x70u) {
        if (io.is_in) {
            /* Real hardware supports reading the index register back;
             * this project has no callers that do, but there is no
             * reason to fail an IN here rather than answer it. */
            real->vmcb->save.rax = (real->vmcb->save.rax & ~0xFFULL) | cmos->index;
        } else {
            hype_cmos_index_write(cmos, (uint8_t)(real->vmcb->save.rax & 0xFFu));
        }
    } else if (io.port == 0x71u) {
        if (io.is_in) {
            uint8_t value = hype_cmos_data_read(cmos);
            real->vmcb->save.rax = (real->vmcb->save.rax & ~0xFFULL) | value;
        } else {
            hype_cmos_data_write(cmos, (uint8_t)(real->vmcb->save.rax & 0xFFu));
        }
    } else {
        return -1;
    }

    /* EXITINFO2 gives the resume RIP directly, same "next-RIP-for-free"
     * convenience hype_svm_vcpu_handle_ioio() itself already relies on. */
    real->vmcb->save.rip = real->vmcb->control.exitinfo2;
    return 0;
}

int hype_svm_vcpu_handle_ps2_kbd_ioio(hype_vcpu_ctx_t *ctx, hype_ps2_kbd_t *kbd) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_ioio_t io;
    int rc;

    hype_svm_decode_ioio_info1(real->vmcb->control.exitinfo1, &io);

    if (io.is_in) {
        uint8_t value = 0;
        rc = hype_ps2_kbd_io_read(kbd, io.port, &value);
        if (rc == 0) {
            real->vmcb->save.rax = (real->vmcb->save.rax & ~0xFFULL) | value;
        }
    } else {
        rc = hype_ps2_kbd_io_write(kbd, io.port, (uint8_t)(real->vmcb->save.rax & 0xFFu));
    }

    if (rc != 0) {
        return -1;
    }

    /* EXITINFO2 gives the resume RIP directly, same "next-RIP-for-free"
     * convenience hype_svm_vcpu_handle_ioio() itself already relies on. */
    real->vmcb->save.rip = real->vmcb->control.exitinfo2;
    return 0;
}

/* FW-1g: when on, every guest 0x60/0x64 access is logged (port, dir,
 * value, guest rip). FW-1 turns it on right after injecting a keystroke
 * so we can see whether OVMF's WaitForKey poll reads the status (OBF)
 * and consumes the scancode -- without the init traffic drowning it. */
static int g_ps2_trace = 0;
/* #436 kbd-poll breadcrumbs (see the PS/2 ioio handler). */
volatile uint64_t g_436_last_p64_tsc; volatile uint64_t g_436_last_p64_rip;
volatile uint8_t g_436_last_p64_val;
volatile uint64_t g_436_last_p60_tsc; volatile uint8_t g_436_last_p60_val;

void hype_svm_set_ps2_trace(int enabled) {
    g_ps2_trace = enabled ? 1 : 0;
}

/* FW-1h diagnostic: trace every AHCI command-slot dispatch (the CDB
 * opcode, whether it is an ATAPI PACKET, and the resulting status), so
 * we can see exactly what OVMF's AtaAtapiPassThru/ScsiDisk stack asks
 * the emulated CD-ROM for during boot-device discovery. Off by default;
 * FW-1's guest turns it on right before launch. */
/*
 * #318: the per-MMIO ABAR trace is separately gated. It is ~20x the volume of the CDB trace
 * (every register poll, and a polling driver does thousands per command), and each line is
 * GOP-rendered -- which is what turned a 33-second kernel load into 20+ minutes and made the
 * instrument unusable for reaching the point we actually need to observe. The CDB trace alone
 * answers "which command is the guest stuck on"; opt into the register firehose only when the
 * question is about register semantics.
 */
#ifndef HYPE_318_TRACE_MMIO
#define HYPE_318_TRACE_MMIO 0
#endif
static int g_ahci_trace = 0;

/* #315: how often an IDT-delivery event was re-staged or refused. Counters rather than a
 * per-event log for the re-stage case: it is on the VMRUN path, and a printf there at any
 * frequency is its own problem (PERF-2). */
static unsigned long g_evtreplay_restaged;
static unsigned long g_evtreplay_refused;

void hype_svm_get_evtreplay_counts(unsigned long *restaged, unsigned long *refused) {
    if (restaged) *restaged = g_evtreplay_restaged;
    if (refused) *refused = g_evtreplay_refused;
}

void hype_svm_set_ahci_trace(int enabled) {
    g_ahci_trace = enabled ? 1 : 0;
}

/* M4-6 diagnostic: when on, an MSR the allow-list doesn't recognize is
 * logged and handled permissively (RDMSR -> 0, WRMSR -> ignored) instead
 * of being fatal, so a single real-guest (Linux) boot reveals the full
 * set of MSRs the guest touches -- far cheaper than one fatal-and-fix
 * cycle per MSR. Off by default; the committed handler stays fail-closed
 * (returns -1 -> the caller's fatal) for guest isolation. */
static int g_msr_trace = 0;

void hype_svm_set_msr_trace(int enabled) {
    g_msr_trace = enabled ? 1 : 0;
}

int hype_svm_vcpu_handle_ps2_ioio(hype_vcpu_ctx_t *ctx, hype_ps2_kbd_t *kbd, hype_ps2_mouse_t *mouse,
                                   int *out_kbd_wait) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_ioio_t io;
    uint8_t traced_value = 0;

    if (out_kbd_wait != 0) {
        *out_kbd_wait = 0;
    }

    hype_svm_decode_ioio_info1(real->vmcb->control.exitinfo1, &io);

    if (io.port == HYPE_PS2_PORT_DATA) {
        if (io.is_in) {
            uint8_t value;
            /* The model keeps upstream keyboard and auxiliary FIFOs
             * separately, but the i8042 exposes one output buffer. Prefer a
             * waiting keyboard byte so an old auxiliary reply cannot make the
             * guest observe AUX_DATA forever and starve interactive input. */
            if (hype_ps2_kbd_has_pending_byte(kbd)) {
                hype_ps2_kbd_io_read(kbd, HYPE_PS2_PORT_DATA, &value);
            } else {
                value = hype_ps2_mouse_read_byte(mouse);
            }
            real->vmcb->save.rax = (real->vmcb->save.rax & ~0xFFULL) | value;
            traced_value = value;
        } else {
            uint8_t value = (uint8_t)(real->vmcb->save.rax & 0xFFu);
            if (hype_ps2_kbd_take_aux_data_write(kbd)) {
                hype_ps2_mouse_write_command(mouse, value);
            } else {
                hype_ps2_kbd_io_write(kbd, HYPE_PS2_PORT_DATA, value);
            }
            traced_value = value;
        }
    } else if (io.port == HYPE_PS2_PORT_STATUS_COMMAND) {
        if (io.is_in) {
            uint8_t status;
            hype_ps2_kbd_io_read(kbd, HYPE_PS2_PORT_STATUS_COMMAND, &status);
            if (!hype_ps2_kbd_has_pending_byte(kbd) &&
                hype_ps2_mouse_has_pending_byte(mouse)) {
                status |= HYPE_PS2_STATUS_OUTPUT_FULL | HYPE_PS2_STATUS_AUX_DATA;
            }
            /* A status read with the output buffer empty is the guest
             * checking "is a key/byte available?" and finding none -- the
             * signal that it is polling, waiting for input (FW-1g). */
            if (out_kbd_wait != 0 && (status & HYPE_PS2_STATUS_OUTPUT_FULL) == 0) {
                *out_kbd_wait = 1;
            }
            real->vmcb->save.rax = (real->vmcb->save.rax & ~0xFFULL) | status;
            traced_value = status;
        } else {
            traced_value = (uint8_t)(real->vmcb->save.rax & 0xFFu);
            hype_ps2_kbd_io_write(kbd, HYPE_PS2_PORT_STATUS_COMMAND, traced_value);
        }
    } else {
        return -1;
    }

    {
        /* #436: always trace the first N accesses -- covers the whole init
         * dialogue (which is where OVMF's driver gives up) without the
         * interactive-poll flood the unconditional trace produced. */
        static unsigned ps2_trace_n = 0;
        /* status-read spam excluded: the 10ms poll fills any cap instantly. */
        int interesting = !(io.is_in && io.port == 0x64u);
#ifdef HYPE_QUIET
        interesting = 0;
#endif
        if (g_ps2_trace || (interesting && ps2_trace_n < 200u)) {
            if (interesting) ps2_trace_n++;
            hype_debug_print("fw-1 ps2| %s 0x%x %s=0x%x rip=0x%llx\n", io.is_in ? "IN " : "OUT",
                              (unsigned int)io.port, io.is_in ? "->" : "<-", (unsigned int)traced_value,
                              (unsigned long long)real->vmcb->save.rip);
        }
    }
    /* #436: cheap always-on breadcrumbs -- WHEN did the guest last poll the
     * kbd status/data ports, and what did hype answer? Distinguishes "guest
     * stopped polling" (OVMF-side, timer/event death) from "guest polls but
     * hype reports no data" (hype status-port bug) once GUESTKBD freezes. */
    if (io.is_in) {
        if (io.port == 0x64u) {
            g_436_last_p64_tsc = real_rdtsc();
            g_436_last_p64_rip = real->vmcb->save.rip;
            g_436_last_p64_val = (uint8_t)traced_value;
        } else if (io.port == 0x60u) {
            g_436_last_p60_tsc = real_rdtsc();
            g_436_last_p60_val = (uint8_t)traced_value;
        }
    }

    /* EXITINFO2 gives the resume RIP directly, same "next-RIP-for-free"
     * convenience hype_svm_vcpu_handle_ioio() itself already relies on. */
    real->vmcb->save.rip = real->vmcb->control.exitinfo2;
    return 0;
}

/* HYPE_FW_1_ACPI_PM_TIMER_PORT/_MASK now live in vmcb.h -- shared with VMX (#236). */

/* M4-6b2: host TSC frequency, stashed at guest start (hype_svm_vcpu_set_pvclock)
 * so the ACPI PM timer can scale the raw TSC down to the architectural
 * 3.579545 MHz PM-timer rate the guest firmware expects. */
static uint64_t g_acpi_pm_tsc_hz = 0;

int hype_svm_vcpu_handle_acpi_pm_timer_ioio(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_ioio_t io;

    hype_svm_decode_ioio_info1(real->vmcb->control.exitinfo1, &io);

    if (io.port != HYPE_FW_1_ACPI_PM_TIMER_PORT) {
        return -1;
    }

    if (io.is_in) {
        /* M4-6b2: scale the host TSC to the ACPI PM timer's architectural
         * 3.579545 MHz rate (was: raw ~GHz TSC, ~950x too fast -- which
         * mis-scaled every guest-firmware delay/timeout that reads this port). */
        uint64_t raw = real_rdtsc();
        uint32_t value = hype_acpi_pm_timer_scale(raw, g_acpi_pm_tsc_hz);
        /* M4-6d6 DIAG (GRUB-hang priority-0): PM-timer LIVENESS trace. GRUB's
         * early-init calibration spins reading this port; log the first reads'
         * (guest_rip, raw host TSC, returned 24-bit value, modular delta from
         * the previous value) so we can tell apart: (a) value not advancing =
         * emulation/order fault; (b) advancing at the wrong ratio = scale
         * fault; (c) advancing correctly while GRUB still loops = it waits on
         * something else (symbolize the RIP). Plus a ONE-SHOT host-side
         * controlled-interval check: read the scaler, busy-wait exactly
         * tsc_hz/1000 host TSC (=1ms), read again -- a correct 3.579545 MHz
         * timer must advance ~3579 ticks in that window. */
        static unsigned pm_trace_n = 0;
        static uint32_t pm_prev = 0;
        static int pm_selftest_done = 0;
        if (!pm_selftest_done && g_acpi_pm_tsc_hz != 0) {
            uint64_t t0 = real_rdtsc();
            uint32_t v0 = hype_acpi_pm_timer_scale(t0, g_acpi_pm_tsc_hz);
            uint64_t target = t0 + g_acpi_pm_tsc_hz / 1000ULL; /* 1ms of host TSC */
            uint32_t v1;
            uint64_t t1;
            while ((t1 = real_rdtsc()) < target) { /* busy-wait ~1ms */ }
            v1 = hype_acpi_pm_timer_scale(t1, g_acpi_pm_tsc_hz);
            pm_selftest_done = 1;
            hype_debug_print("fw-1 PMLIVE selftest: tsc_hz=%llu div=%llu | over 1ms (tsc +%llu) PM advanced "
                             "%u ticks (expect ~3579) v0=0x%x v1=0x%x\n",
                             (unsigned long long)g_acpi_pm_tsc_hz,
                             (unsigned long long)(g_acpi_pm_tsc_hz / 3579545ULL),
                             (unsigned long long)(t1 - t0),
                             (unsigned)((v1 - v0) & HYPE_FW_1_ACPI_PM_TIMER_MASK), v0, v1);
        }
        if (pm_trace_n < 32u) {
            hype_debug_print("fw-1 PMLIVE#%02u: rip=0x%llx raw_tsc=0x%llx pm=0x%06x d=%u\n",
                             pm_trace_n, (unsigned long long)real->vmcb->save.rip,
                             (unsigned long long)raw, (unsigned)value,
                             (unsigned)((value - pm_prev) & HYPE_FW_1_ACPI_PM_TIMER_MASK));
            pm_prev = value;
            pm_trace_n++;
        }
        real->vmcb->save.rax = (real->vmcb->save.rax & ~0xFFFFFFFFULL) | value;
    }
    /* A write to the PM Timer's own status/value port is not a real
     * hardware operation this register supports -- silently ignored,
     * matching every other "nothing meaningful to do" IOIO write
     * already established here (e.g. hype_svm_vcpu_handle_unknown_ioio()). */

    /* EXITINFO2 gives the resume RIP directly, same "next-RIP-for-free"
     * convenience hype_svm_vcpu_handle_ioio() itself already relies on. */
    real->vmcb->save.rip = real->vmcb->control.exitinfo2;
    return 0;
}

/* M4-6d2 DIAG: interrupt-injection path counters (INT-1/INT-2). Cumulative
 * across all vCPUs (a diagnostic aggregate); the pending-IRQ slot itself is
 * per-vCPU (struct hype_vcpu_ctx.pending_irq_*). */
/*
 * #311: did an injection of the AHCI vector actually get TAKEN? Staging EVENTINJ proves hype asked;
 * it does not prove the guest vectored through its IDT. So the vector is flagged when staged and the
 * guest's rip is reported at the NEXT exit: if the interrupt was taken, that rip is inside FreeBSD's
 * interrupt stub, not back at the instruction the guest was running.
 */

/*
 * #563: these four counters, plus the collision count below, are PER vCPU now -- see
 * int_eventinj/int_defer/int_window/int_overwrite/int_collision on the context struct. As
 * file-globals they summed every vCPU of every VM, so a lost injection could not be attributed to
 * the guest that lost it.
 *
 * M4-6b2, on the collision count: it records a request that found an event already staged for the
 * next VM entry and QUEUED the new vector in the IRR instead of clobbering the staged one. Under
 * the old code that was an INVISIBLE lost interrupt -- the direct path overwrote unconditionally
 * and counted nothing -- and it was the actual cause of the one-shot-clockevent death. Counted,
 * and never lost.
 */

/*
 * #343: ATAPI transfer accounting. A SHORT transfer -- the PRDT list exhausted with bytes still
 * owed -- is reported to the guest as success, so nothing else in the system can notice it. Any
 * non-zero short count means a guest was handed a partly-filled buffer and told the read completed.
 *
 * Counters, not a trace: the ISO stream trace is capped at 24 records and the guest's kernel load
 * happens long after those, which is exactly why the first pass at this question had no evidence
 * either way. See #356 for the same lesson at greater cost.
 */
static unsigned long long g_atapi_xfers = 0;
static unsigned long long g_atapi_short_xfers = 0;
static unsigned long long g_atapi_req_bytes = 0;
static unsigned long long g_atapi_done_bytes = 0;
static unsigned long long g_atapi_owed_bytes = 0;
#if HYPE_343_VERIFY_READS
static unsigned long long g_343_verified = 0;
static unsigned long long g_343_mismatch = 0;
#endif

void hype_svm_vcpu_get_atapi_diag(unsigned long long *xfers, unsigned long long *short_xfers,
                                  unsigned long long *req_bytes, unsigned long long *done_bytes,
                                  unsigned long long *owed_bytes) {
    if (xfers != 0) { *xfers = g_atapi_xfers; }
    if (short_xfers != 0) { *short_xfers = g_atapi_short_xfers; }
    if (req_bytes != 0) { *req_bytes = g_atapi_req_bytes; }
    if (done_bytes != 0) { *done_bytes = g_atapi_done_bytes; }
    if (owed_bytes != 0) { *owed_bytes = g_atapi_owed_bytes; }
}

#if HYPE_343_VERIFY_READS
void hype_svm_vcpu_get_read_verify(unsigned long long *checked, unsigned long long *mismatched) {
    if (checked != 0) { *checked = g_343_verified; }
    if (mismatched != 0) { *mismatched = g_343_mismatch; }
}
#endif

void hype_svm_vcpu_get_int_diag(hype_vcpu_ctx_t *ctx, unsigned long long *eventinj,
                                 unsigned long long *defer, unsigned long long *window,
                                 unsigned long long *overwrite) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    if (real == 0) {
        *eventinj = 0; *defer = 0; *window = 0; *overwrite = 0;
        return;
    }
    *eventinj = real->int_eventinj;
    *defer = real->int_defer;
    *window = real->int_window;
    *overwrite = real->int_overwrite;
}

unsigned long long hype_svm_vcpu_get_eventinj_collisions(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    return (real != 0) ? real->int_collision : 0ull;
}

/* Arm the VINTR interrupt-window intercept iff vectors are still queued in the
 * IRR; disarm it once the queue drains. Keeps the "wake me when the guest can
 * take an interrupt" request exactly as long as there is something to deliver. */
static void hype_svm_sync_vintr(struct hype_vcpu_ctx *real) {
    if (hype_svm_irr_any(real->pending_irr)) {
        int v = hype_svm_irr_highest(real->pending_irr);
        real->vmcb->control.vintr = hype_svm_arm_vintr_request(real->vmcb->control.vintr,
                                                                 (uint8_t)v);
        real->vmcb->control.intercept_misc1 |= HYPE_SVM_INTERCEPT_VINTR;
    } else {
        real->vmcb->control.vintr = hype_svm_disarm_vintr_request(real->vmcb->control.vintr);
        real->vmcb->control.intercept_misc1 &= ~HYPE_SVM_INTERCEPT_VINTR;
    }
}

void hype_svm_vcpu_get_intr_state(hype_vcpu_ctx_t *ctx, hype_svm_intr_state_t *out) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    out->rflags = real->vmcb->save.rflags;
    out->interrupt_shadow = real->vmcb->control.interrupt_shadow;
    out->eventinj = real->vmcb->control.eventinj;
    out->vintr = real->vmcb->control.vintr;
    out->can_accept =
        hype_svm_can_accept_interrupt(real->vmcb->save.rflags, real->vmcb->control.interrupt_shadow);
    out->pending_valid = hype_svm_irr_any(real->pending_irr);
    out->pending_count = hype_svm_irr_count(real->pending_irr); /* #356 */
    {
        int hv = hype_svm_irr_highest(real->pending_irr);
        out->pending_vector = (uint8_t)(hv < 0 ? 0 : hv);
    }
}

static unsigned int g_int_trace_n = 0; /* #311: bounds the injection trace above */
static unsigned int g_int_trace_timer_n = 0;
static uint8_t g_int_trace_timer_vec = 0xFFu;

/*
 * #318: per-vector interrupt accounting. The bounded INJ# text trace above cannot answer "was
 * this vector ever delivered": twice now its budget was spent by the periodic timer before the
 * vector under investigation first fired, and both times the missing lines read as absence of
 * the event rather than absence of the trace. Counters cannot be crowded out.
 */
void hype_svm_vcpu_get_vec_counts(hype_vcpu_ctx_t *ctx, uint8_t vector, uint32_t *out_req,
                                  uint32_t *out_inj) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    if (out_req != 0) {
        *out_req = (real != 0) ? real->int_req_by_vec[vector] : 0u;
    }
    if (out_inj != 0) {
        *out_inj = (real != 0) ? real->int_inj_by_vec[vector] : 0u;
    }
}

/* #456: record that `vector` has just been staged into EVENTINJ, so the caller can mark the
 * guest's emulated LAPIC ISR for exactly the vectors hype committed. */
static void svm_note_injected(struct hype_vcpu_ctx *real, uint8_t vector) {
    real->inj_notify[vector >> 5] |= (uint32_t)1u << (vector & 31u);
}

int hype_svm_vcpu_take_injected_vector(hype_vcpu_ctx_t *ctx, uint8_t *out_vector) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    unsigned word;

    for (word = 0; word < 8u; word++) {
        uint32_t bits = real->inj_notify[word];
        unsigned bit;
        if (bits == 0u) {
            continue;
        }
        for (bit = 0; bit < 32u; bit++) {
            if ((bits & ((uint32_t)1u << bit)) != 0u) {
                real->inj_notify[word] &= ~((uint32_t)1u << bit);
                *out_vector = (uint8_t)(word * 32u + bit);
                return 1;
            }
        }
    }
    return 0;
}

void hype_svm_vcpu_request_interrupt(hype_vcpu_ctx_t *ctx, uint8_t vector) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    real->int_req_by_vec[vector]++;
    int eventinj_busy = (real->vmcb->control.eventinj & HYPE_SVM_EVENTINJ_V) != 0;

    /* Fast path: the guest can take an interrupt AND nothing is already staged
     * for the next VMRUN -> inject directly. The eventinj_busy guard is the fix
     * for the lost-interrupt bug: without it, a second request in the same run-
     * loop iteration (timer, then AHCI/serial/PIT) unconditionally overwrote the
     * first vector's EVENTINJ, silently dropping it -- fatal to a self-re-arming
     * one-shot clockevent, which then never gets its next tick. */
    /* #311: a bounded trace of every vector hype hands the guest, and by which route. The
     * open question is whether an AHCI completion's vector reaches the guest's IDT at all --
     * "injected but never taken" and "taken but the EOI was missed" are different bugs with
     * different fixes, and from outside they look identical: a command that completed in hype
     * and timed out in the guest. Bounded so steady interrupt traffic cannot flood the log. */
    /* The periodic timer vector fires constantly and would consume the whole budget before
     * the storage probe even starts, so it gets a small quota of its own and everything else
     * -- which is what this trace exists for -- keeps the rest. */
    if (vector == g_int_trace_timer_vec) {
        if (g_int_trace_timer_n < 4u) {
            g_int_trace_timer_n++;
        } else {
            goto trace_done;
        }
    } else if (g_int_trace_timer_vec == 0xFFu) {
        g_int_trace_timer_vec = vector; /* first vector seen is the timer's, by construction */
    }
    if (g_int_trace_n < 40u) {
        g_int_trace_n++;
        hype_debug_print("fw-1 INJ#%02u vec=0x%02x %s rflags_if=%d shadow=%d einj_busy=%d\n",
                         (unsigned int)g_int_trace_n, (unsigned int)vector,
                         (!eventinj_busy && hype_svm_can_accept_interrupt(
                                                real->vmcb->save.rflags,
                                                real->vmcb->control.interrupt_shadow) &&
                          hype_svm_vintr_priority_allows(real->vmcb->control.vintr, vector))
                             ? "direct"
                             : "deferred",
                         (int)((real->vmcb->save.rflags >> 9) & 1u),
                         (int)(real->vmcb->control.interrupt_shadow & 1u), eventinj_busy);
    }
trace_done:

    if (!eventinj_busy &&
        hype_svm_can_accept_interrupt(real->vmcb->save.rflags, real->vmcb->control.interrupt_shadow) &&
        hype_svm_vintr_priority_allows(real->vmcb->control.vintr, vector)) {
        real->vmcb->control.eventinj = hype_svm_encode_eventinj_intr(vector);
        real->int_inj_by_vec[vector]++;
        svm_note_injected(real, vector); /* #456 */
        real->int_eventinj++;
        return;
    }

    /* Otherwise queue the vector in the IRR (never overwrite a staged event or
     * a differently-numbered pending vector) and arm the interrupt window so it
     * drains as soon as the guest can accept it. Requesting an already-pending
     * vector coalesces (correct IRR semantics: one delivery per set bit). */
    if (eventinj_busy) {
        real->int_collision++;
    }
    if ((real->pending_irr[vector >> 5] & ((uint32_t)1u << (vector & 31u))) != 0) {
        real->int_overwrite++; /* vector already pending -> coalesced (not lost) */
    }
    hype_svm_irr_set(real->pending_irr, vector);
    hype_svm_sync_vintr(real);
    real->int_defer++;
}

/* GLADDER-6c DIAG: reinject a guest exception that hype intercepted purely to
 * observe it -- staged in EVENTINJ so the guest takes it through its own IDT on
 * the next VMRUN, exactly as if hype had never intercepted the vector. Type =
 * EXCEPTION(3); EV + error code for faults that push one (#GP=13/#PF=14/#DF=8);
 * #UD=6 pushes none. Does not touch the pending-IRR interrupt path. */
void hype_svm_vcpu_reinject_exception(hype_vcpu_ctx_t *ctx, uint8_t vector,
                                      int has_error_code, uint32_t error_code) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    uint64_t einj = HYPE_SVM_EVENTINJ_V |
                    (HYPE_SVM_EVENTINJ_TYPE_EXCEPTION << HYPE_SVM_EVENTINJ_TYPE_SHIFT) |
                    ((uint64_t)vector & HYPE_SVM_EVENTINJ_VECTOR_MASK);
    if (has_error_code) {
        einj |= HYPE_SVM_EVENTINJ_EV | ((uint64_t)error_code << 32);
    }
    real->vmcb->control.eventinj = einj;
}

/*
 * #484: inject an NMI (EVENTINJ type 2, vector 2).
 *
 * The guest's ICR supports a delivery mode hype never implemented. Linux uses an NMI IPI to
 * make a CPU that is not responding print a backtrace -- which is exactly what an RCU stall
 * report does -- and also for its NMI watchdog. hype's IPI router had cases for INIT, STARTUP,
 * FIXED and lowest-priority, and silently dropped NMI, so "Sending NMI from CPU 0 to CPUs 1:"
 * was followed by nothing at all and the guest could neither diagnose nor recover.
 *
 * Must be called on the target vCPU's OWN core: this writes its VMCB, and hardware rewrites
 * the control area on exit, so a cross-core write can be lost (the #190 IPI bug).
 */
void hype_svm_vcpu_inject_nmi(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    real->vmcb->control.eventinj = HYPE_SVM_EVENTINJ_V |
                                   (HYPE_SVM_EVENTINJ_TYPE_NMI << HYPE_SVM_EVENTINJ_TYPE_SHIFT) |
                                   2ULL;
}

void hype_svm_vcpu_cancel_pending_vector(hype_vcpu_ctx_t *ctx, uint8_t vector) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_irr_clear(real->pending_irr, vector);
    hype_svm_irr_clear(real->pending_pic, vector); /* #512 */
}

/* #512: mark a just-queued vector as PIC-sourced, so the #455 pruner may cancel it if its
 * line masks. Only a vector still PENDING gets the mark -- a direct EVENTINJ needs none. */
void hype_svm_vcpu_note_pic_pending(hype_vcpu_ctx_t *ctx, uint8_t vector) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    if ((real->pending_irr[vector >> 5] & ((uint32_t)1u << (vector & 31u))) != 0u) {
        hype_svm_irr_set(real->pending_pic, vector);
    }
}

/* #512: the #455 prune, restricted to vectors the PIC-acknowledge path queued. An IO-APIC or
 * MSI vector that merely shares the masked PIC's vector range is not touched. */
void hype_svm_vcpu_cancel_pic_pending(hype_vcpu_ctx_t *ctx, uint8_t vector) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    if ((real->pending_pic[vector >> 5] & ((uint32_t)1u << (vector & 31u))) != 0u) {
        hype_svm_irr_clear(real->pending_irr, vector);
        hype_svm_irr_clear(real->pending_pic, vector);
        hype_svm_sync_vintr(real);
    }
}

void hype_svm_vcpu_handle_vintr_window(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;

    /* The window fired -> the guest can accept an interrupt now. Stage the
     * highest-priority queued vector into EVENTINJ (if EVENTINJ isn't already
     * occupied), then re-sync the window: keep it armed while more remain,
     * disarm once the IRR is empty. */
    if (hype_svm_irr_any(real->pending_irr) &&
        (real->vmcb->control.eventinj & HYPE_SVM_EVENTINJ_V) == 0) {
        int v = hype_svm_irr_highest(real->pending_irr);
        hype_svm_irr_clear(real->pending_irr, (uint8_t)v);
        hype_svm_irr_clear(real->pending_pic, (uint8_t)v); /* #512 */
        real->vmcb->control.eventinj = hype_svm_encode_eventinj_intr((uint8_t)v);
        real->int_inj_by_vec[(uint8_t)v]++;
        svm_note_injected(real, (uint8_t)v); /* #456 */
        real->int_window++;
    }
    hype_svm_sync_vintr(real);
}

/*
 * #553: THE RULE, measured and cited, because this has been got wrong twice.
 *
 * Intel SDM Vol. 2A, "HLT-Halt" (p. 3-439, research/325462-092-sdm-vol-1-2abcd-3abcd-4.pdf):
 *
 *   "If an interrupt (including NMI) is used to resume execution after a HLT instruction, the
 *    saved instruction pointer (CS:EIP) points to the instruction FOLLOWING the HLT instruction."
 *
 * So retiring the HLT before injecting is not an optimisation or a Linux quirk -- it is the
 * architectural requirement. An interrupt delivered while RIP still points AT the hlt makes the
 * guest's iretq re-execute it, so the guest re-halts and its idle loop makes NO forward progress
 * per tick.
 *
 * MEASURED, on tests/micro/intdeliver: 1154 IRQ0 deliveries at exactly the right 100 Hz rate, and
 *
 *     [#553] frame RIP at_hlt=1153 past_hlt=1 other=0
 *
 * -- 1153 of them resumed AT the hlt. The guest's own `while (ticks < N) { hlt; resumes++; }` loop
 * advanced once in 1154 wakeups. Any guest counting one wake per interrupt (a jiffies-driven
 * timeout, a bounded retry spinner) therefore advances ~1000x slower than the wall clock says,
 * which is the shape of several past guest stalls here.
 *
 * This function is correct. The defect is that the dispatch loop's OTHER injection paths reach a
 * halted guest without coming through here -- see #580. Anything that injects into a guest that is
 * sitting at a hlt must retire it first.
 */
void hype_svm_vcpu_wake_hlt(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    /* Model an interrupt waking a halted CPU: the HLT retires (so the
     * guest resumes AFTER it -- e.g. its idle loop's need_resched check --
     * not by re-executing HLT), and the STI interrupt-shadow that covered
     * the HLT is consumed. HLT is a fixed 1-byte opcode (0xF4), so past-it
     * is RIP+1 (no reliance on nRIP, which QEMU's nested SVM may not
     * populate). The caller injects the waking interrupt right after; the
     * guest takes it with a return address after the HLT. */
    real->vmcb->save.rip += 1;
    real->vmcb->control.interrupt_shadow &= ~1ULL;
}

int hype_svm_vcpu_deliver_pending_if_ready(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;

    /* M4-6d2: belt-and-suspenders to the VINTR window (INT-2). A deferred
     * interrupt is only delivered when its VINTR intercept fires; if that
     * doesn't happen promptly (observed: a deferred PIT IRQ0 stranded
     * while the guest was halted/ready, wedging the timer -> jiffies froze
     * -> libata's post-COMRESET msleep hung), poll each vCPU loop
     * iteration and inject directly the moment the guest can accept -- and
     * only then move it off the pending slot / disarm the window. Also
     * guards against EVENTINJ clobbering an event already staged this
     * entry. Returns 1 if it injected. */
    if (!hype_svm_irr_any(real->pending_irr)) {
        return 0;
    }
    if ((real->vmcb->control.eventinj & HYPE_SVM_EVENTINJ_V) != 0) {
        return 0; /* an event is already staged for the next VMRUN */
    }
    if (!hype_svm_can_accept_interrupt(real->vmcb->save.rflags, real->vmcb->control.interrupt_shadow)) {
        return 0;
    }
    {
        int v = hype_svm_irr_highest(real->pending_irr);
        if (!hype_svm_vintr_priority_allows(real->vmcb->control.vintr, (uint8_t)v)) {
            return 0;
        }
        hype_svm_irr_clear(real->pending_irr, (uint8_t)v);
        hype_svm_irr_clear(real->pending_pic, (uint8_t)v); /* #512 */
        real->vmcb->control.eventinj = hype_svm_encode_eventinj_intr((uint8_t)v);
        real->int_inj_by_vec[(uint8_t)v]++;
        svm_note_injected(real, (uint8_t)v); /* #456 */
    }
    /* Keep the window armed if more vectors remain; disarm once drained. */
    hype_svm_sync_vintr(real);
    real->int_eventinj++;
    return 1;
}

void hype_svm_vcpu_deliver_pic_irq(hype_vcpu_ctx_t *ctx, hype_pic_emu_chip_t *chip, uint8_t irq) {
    uint8_t vector;

    hype_pic_emu_raise_irq(chip, irq);
    if (hype_pic_emu_acknowledge_highest_priority(chip, &vector)) {
        hype_svm_vcpu_request_interrupt(ctx, vector);
    }
}

void hype_svm_vcpu_get_debug_state(hype_vcpu_ctx_t *ctx, hype_svm_debug_state_t *out) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    out->cs_selector = real->vmcb->save.cs.selector;
    out->cs_base = real->vmcb->save.cs.base;
    out->cr0 = real->vmcb->save.cr0;
    out->cr2 = real->vmcb->save.cr2;
    out->cr3 = real->vmcb->save.cr3;
    out->rip = real->vmcb->save.rip;
    out->rflags = real->vmcb->save.rflags;
    out->rsp = real->vmcb->save.rsp;
    out->exitinfo2 = real->vmcb->control.exitinfo2;
    out->exitintinfo = real->vmcb->control.exitintinfo;
    out->nrip = real->vmcb->control.nrip;
    out->cr4 = real->vmcb->save.cr4;
    out->g_pat = real->vmcb->save.g_pat;
    out->idtr_base = real->vmcb->save.idtr.base;
    out->idtr_limit = real->vmcb->save.idtr.limit;
}

/* PERF-1 memory-type probe: guest MTRR MSR access counters (aggregate across
 * VMs -- diagnostic). Reads currently return 0 (stubbed), writes are ignored;
 * these count how often the guest touches MTRRs and what it tried to write, to
 * tell whether the guest is left thinking its RAM is uncacheable. */
static volatile unsigned long long g_mtrr_reads = 0;
static volatile unsigned long long g_mtrr_writes = 0;
static volatile uint64_t g_mtrr_last_deftype_wr = 0;
static volatile uint64_t g_mtrr_last_var_wr = 0;

void hype_svm_vcpu_get_mtrr_diag(unsigned long long *reads, unsigned long long *writes,
                                 uint64_t *last_deftype_wr, uint64_t *last_var_wr) {
    *reads = g_mtrr_reads;
    *writes = g_mtrr_writes;
    *last_deftype_wr = g_mtrr_last_deftype_wr;
    *last_var_wr = g_mtrr_last_var_wr;
}

/* True for the MTRR MSR set: MTRRcap 0xFE, MTRRdefType 0x2FF, 8 variable
 * base/mask pairs 0x200-0x20F, and the fixed MTRRs (0x250, 0x258/0x259,
 * 0x268-0x26F). IA32_PAT (0x277) is handled separately, not counted here. */
static int msr_is_mtrr(uint32_t n) {
    return n == 0xFEu || n == 0x2FFu || (n >= 0x200u && n <= 0x20Fu) || n == 0x250u ||
           n == 0x258u || n == 0x259u || (n >= 0x268u && n <= 0x26Fu);
}

void hype_svm_vcpu_set_rip(hype_vcpu_ctx_t *ctx, uint64_t rip) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    real->vmcb->save.rip = rip;
}

/*
 * #436: re-apply the breakpoint after the guest writes DR7. A booting kernel
 * initialises DR7 to the architectural 0x400 as a matter of course, which
 * silently disables any breakpoint set behind its back -- measured: the
 * breakpoint armed correctly and never fired. Intercepting the write lets the
 * observation survive the guest's own initialisation. The guest's intended
 * value is deliberately NOT honoured; this is a diagnostic path, and nothing in
 * the boot being observed uses the debug registers for its own purposes.
 */
int hype_svm_vcpu_handle_dr_write(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;

    if (real->vmcb->control.nrip == 0) {
        return -1; /* no decode assist: cannot skip the instruction safely */
    }
    real->vmcb->save.dr7 = 0x403u;
    real->vmcb->save.rip = real->vmcb->control.nrip;
    real->vmcb->control.vmcb_clean_bits = 0;
    return 0;
}

void hype_svm_vcpu_arm_exec_breakpoint(hype_vcpu_ctx_t *ctx, uint64_t gva) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;

    if (gva == 0) {
        real->vmcb->save.dr7 = 0x400u; /* the architectural reset value */
        real->vmcb->control.intercept_dr = 0;
        real->vmcb->control.vmcb_clean_bits = 0;
        return;
    }
    /* Intercept writes to DR7 (bit 16+7) so the guest cannot disarm this. */
    real->vmcb->control.intercept_dr |= (1u << (16 + 7));
    /*
     * DR0 holds the linear address. DR7: L0|G0 enable it, and its type/length
     * field (bits 19:16) stays zero, which is "execute, one byte" -- the only
     * combination architecturally valid for an instruction breakpoint. Bit 10
     * is reserved-one. The guest's own DR0 is not preserved: nothing in a
     * Windows boot uses it, and this is a diagnostic build.
     */
    __asm__ volatile("mov %0, %%dr0" : : "r"(gva));
    real->vmcb->save.dr7 = 0x403u;
    real->vmcb->control.vmcb_clean_bits = 0;
}

void hype_svm_vcpu_set_exception_intercepts(hype_vcpu_ctx_t *ctx, uint32_t mask) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    real->vmcb->control.intercept_exceptions = mask;
}

const uint8_t *hype_svm_vcpu_guest_insn_bytes(hype_vcpu_ctx_t *ctx, uint8_t *out_num) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    if (out_num != 0) {
        *out_num = real->vmcb->control.num_bytes_fetched;
    }
    return real->vmcb->control.guest_instruction_bytes;
}

uint64_t hype_svm_vcpu_get_gpr(hype_vcpu_ctx_t *ctx, unsigned idx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    if (real == 0 || idx >= 16u) {
        return 0;
    }
    return real->gprs[idx];
}
uint64_t hype_svm_vcpu_get_cr3(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    return real->vmcb->save.cr3;
}

/* PVCLOCK (kvmclock) TSC->ns scale: global, because all cores share one TSC
 * rate. The per-VM guest-memory map and per-guest MSR values live in the vCPU
 * ctx (see struct hype_vcpu_ctx) -- they are NOT shared. */
static uint32_t g_pvclock_mul = 0;
static int8_t g_pvclock_shift = 0;
/* Times hype filled a pvclock time-info page -- nonzero proves the guest
 * detected KVM and enabled kvmclock, and hype backed it. Read by the diag. */
volatile uint32_t g_hype_pvclock_arm_count = 0;

void hype_svm_vcpu_set_pvclock(hype_vcpu_ctx_t *ctx, const hype_gpa_map_t *map, uint64_t tsc_hz) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    real->pvclock_map = map;
    hype_pvclock_calc_scale(tsc_hz, &g_pvclock_mul, &g_pvclock_shift);
    g_acpi_pm_tsc_hz = tsc_hz; /* M4-6b2: also drive the ACPI PM timer's rate */
}

/* Guest wrote MSR_KVM_SYSTEM_TIME: fill its per-vCPU time-info page so it can
 * read time as system_time + scale(rdtsc - tsc_timestamp). We publish
 * system_time = scale(now) with tsc_timestamp = now, so guest time == scale of
 * the raw (passthrough) TSC -- monotonic, TSC_STABLE, no guest calibration. */
static void hype_svm_pvclock_arm_system_time(struct hype_vcpu_ctx *real, uint64_t msr_value) {
    /* #667: the GPA-translate/write sequence itself now lives in hype_pvclock_arm_system_time()
     * (devices/pvclock.c), shared verbatim with the VMX backend, so its VALID-3 rejection path is
     * independently unit-testable without a full vcpu context. */
    (void)hype_pvclock_arm_system_time(msr_value, real->pvclock_map, real_rdtsc(), g_pvclock_mul,
                                       g_pvclock_shift);
}

/* Guest wrote MSR_KVM_WALL_CLOCK: fill the boot-wall-time page. hype has no
 * RTC (CMOS returns 0), so publish epoch 0 -- the guest's monotonic clock
 * (above) is correct; only wall-clock date is unknown, same as today. */
static void hype_svm_pvclock_arm_wall_clock(struct hype_vcpu_ctx *real, uint64_t msr_value) {
    (void)hype_pvclock_arm_wall_clock(msr_value, real->pvclock_map);
}

/* #275: IA32_TSC_AUX, read by RDTSCP into ECX. Not covered by VMSAVE/VMLOAD. */
#define HYPE_SVM_MSR_TSC_AUX 0xC0000103u

static inline uint64_t svm_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void svm_wrmsr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" ::"a"((uint32_t)value), "d"((uint32_t)(value >> 32)), "c"(msr));
}


/*
 * #244: the ASID this vCPU actually runs under, read straight out of the VMCB
 * field VMRUN consumes rather than recomputed from the slot -- so it reports
 * what the hardware will use, not what was intended. 0 would mean the guest
 * shares the host's TLB tag. See hype_vmm_ops_t.vcpu_tlb_tag for why this is a
 * vtable entry rather than a log line.
 */
uint32_t hype_svm_vcpu_tlb_tag(hype_vcpu_ctx_t *ctx) {
    const struct hype_vcpu_ctx *real = (const struct hype_vcpu_ctx *)ctx;
    if (real == 0 || real->vmcb == 0) {
        return 0u;
    }
    return (uint32_t)(real->vmcb->control.guest_asid_tlb_ctl & 0xFFFFFFFFull);
}

/* RDMSR result convention on SVM: low half in the VMCB's own RAX, high half in the
 * shadow RDX. Factored out because the Hyper-V MSRs below all return one value. */
static void svm_msr_return(struct hype_vcpu_ctx *real, uint64_t value) {
    real->vmcb->save.rax = (uint64_t)(uint32_t)value;
    real->gprs[2] = (uint64_t)(uint32_t)(value >> 32);
}

int hype_svm_vcpu_handle_msr(hype_vcpu_ctx_t *ctx, hype_guest_lapic_t *lapic) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    int is_write = (real->vmcb->control.exitinfo1 & 0x1ULL) != 0;
    uint32_t msr_number = (uint32_t)real->gprs[1]; /* RCX */
    hype_msr_action_t action;
#if !defined(HYPE_ENABLE_X2APIC) || !HYPE_ENABLE_X2APIC
    (void)lapic; /* #601: only consulted when the feature is compiled in -- see below */
#endif

    spin_mru_bump(g_spin_msr_key, g_spin_msr_cnt, msr_number); /* #92 diag */
    g_spin_msr_rip = real->vmcb->save.rip;

    /*
     * #275: IA32_TSC_AUX. Guest writes used to fall through to the absorb path, so a
     * guest RDTSCP read the HOST's value. Worse on AMD than it was on Intel: VMX gated
     * RDTSCP behind ENABLE_RDTSCP so the exposure was new, whereas on SVM RDTSCP has
     * always simply executed.
     *
     * Serviced ahead of the action switch, and deliberately NOT added to msr_emulate's
     * action list, for the same reason #251 gave for the VMX MSR-area MSRs: keeping
     * one source of truth. Letting it fall to absorb would discard a guest write that
     * the next entry then contradicts.
     */
    if (msr_number == HYPE_SVM_MSR_TSC_AUX) {
        if (is_write) {
            real->tsc_aux =
                ((uint64_t)(uint32_t)real->gprs[2] << 32) | (uint64_t)(uint32_t)real->vmcb->save.rax;
            if (!real->tsc_aux_valid) {
                /* Say it once: this is the moment the swap below becomes active, and
                 * without it "the fix is wired in" is unfalsifiable from a log. */
                hype_debug_print("svm: guest wrote TSC_AUX=0x%llx -- per-guest RDTSCP value now "
                                 "swapped around VMRUN (#275)\n",
                                 (unsigned long long)(((uint64_t)(uint32_t)real->gprs[2] << 32) |
                                                      (uint64_t)(uint32_t)real->vmcb->save.rax));
            }
            real->tsc_aux_valid = 1;
        } else {
            real->vmcb->save.rax = (uint64_t)(uint32_t)real->tsc_aux;
            real->gprs[2] = (uint64_t)(uint32_t)(real->tsc_aux >> 32);
        }
        /*
         * WRMSR/RDMSR are 2 bytes. Advance by 2, exactly as every other MSR path
         * in this function does.
         *
         * This used to read `save.rip = control.exitinfo2`, with a comment
         * claiming SVM hands the next RIP there and that the neighbouring paths
         * already used it. Both halves were wrong. For an MSR intercept SVM
         * defines EXITINFO1 as the read/write flag and leaves EXITINFO2
         * RESERVED -- the next-sequential-RIP lives in the VMCB's separate nRIP
         * field, not there -- and the other paths a few lines below plainly do
         * `save.rip += 2`.
         *
         * EXITINFO2 read as 0, so the guest resumed executing at address 0. It
         * cost a guest boot: Linux writes this MSR from setup_getcpu() inside
         * cpu_init_exception_handling(), so the first guest to touch TSC_AUX
         * died in trap_init() with `RIP: 0010:0x0`, a NULL-pointer Oops, and
         * "Attempted to kill the idle task!" -- with RCX still holding
         * 0xC0000103 from the WRMSR, which is what identified it.
         *
         * It hid because #275 shipped against a guest that never wrote TSC_AUX,
         * so this line never executed and the feature was correctly reported as
         * dormant. Dormant code is untested code: the first guest that woke it
         * did not survive.
         */
        real->vmcb->save.rip += 2;
        return 0;
    }

    /* PERF-1 memory-type probe: count guest MTRR MSR traffic (does not change
     * handling -- these still fall through to the stub path below). */
    if (msr_is_mtrr(msr_number) || msr_number == 0xFEu) {
        /*
         * #436: round-trip the MTRR MSRs (store writes, return them on reads) so OVMF MtrrLib's
         * write-then-verify converges instead of looping. MTRRcap (0xFE) is read-only and returns
         * a fixed, self-consistent capability: 8 variable MTRRs + fixed-MTRR + WC supported.
         */
        uint64_t rval = 0;
        int fixi = -1;
        if (msr_number == 0x250u) fixi = 0;
        else if (msr_number == 0x258u) fixi = 1;
        else if (msr_number == 0x259u) fixi = 2;
        else if (msr_number >= 0x268u && msr_number <= 0x26Fu) fixi = 3 + (int)(msr_number - 0x268u);
        if (is_write) {
            uint64_t wval =
                ((uint64_t)(uint32_t)real->gprs[2] << 32) | (uint64_t)(uint32_t)real->vmcb->save.rax;
            g_mtrr_writes++;
            if (msr_number == 0xFEu) {
                /* MTRRcap is read-only; a write is #GP on real hardware. Ignore it. */
            } else if (msr_number == 0x2FFu) {
                real->mtrr_deftype = wval;
                g_mtrr_last_deftype_wr = wval;
            } else if (msr_number >= 0x200u && msr_number <= 0x20Fu) {
                real->mtrr_var[msr_number - 0x200u] = wval;
                g_mtrr_last_var_wr = wval;
            } else if (fixi >= 0) {
                real->mtrr_fix[fixi] = wval;
            }
            real->vmcb->save.rip += 2;
            return 0;
        }
        g_mtrr_reads++;
        if (msr_number == 0xFEu) {
            /* VCNT=8 [7:0], FIX=1 [8], WC=1 [10]. Matches the 8 mtrr_var[] pairs above. */
            rval = 0x0508u;
        } else if (msr_number == 0x2FFu) {
            rval = real->mtrr_deftype;
        } else if (msr_number >= 0x200u && msr_number <= 0x20Fu) {
            rval = real->mtrr_var[msr_number - 0x200u];
        } else if (fixi >= 0) {
            rval = real->mtrr_fix[fixi];
        }
        real->vmcb->save.rax = (uint64_t)(uint32_t)rval;
        real->gprs[2] = (uint64_t)(uint32_t)(rval >> 32);
        real->vmcb->save.rip += 2;
        return 0;
    }

    /* PVCLOCK (kvmclock): the guest arms the paravirt clock by writing the
     * guest-physical address of its time-info page (| enable) to
     * MSR_KVM_SYSTEM_TIME, and the wall-clock page to MSR_KVM_WALL_CLOCK.
     * hype fills those pages from the host TSC (see helpers above), so the
     * guest never runs its own (failing, on AMD) TSC calibration. */
    if (msr_number == HYPE_MSR_KVM_SYSTEM_TIME_NEW || msr_number == HYPE_MSR_KVM_SYSTEM_TIME_OLD) {
        if (is_write) {
            real->pvclock_system_msr =
                ((uint64_t)(uint32_t)real->gprs[2] << 32) | (uint64_t)(uint32_t)real->vmcb->save.rax;
            hype_svm_pvclock_arm_system_time(real, real->pvclock_system_msr);
        } else {
            real->vmcb->save.rax = (uint64_t)(uint32_t)real->pvclock_system_msr;
            real->gprs[2] = (uint64_t)(uint32_t)(real->pvclock_system_msr >> 32);
        }
        real->vmcb->save.rip += 2;
        return 0;
    }
    if (msr_number == HYPE_MSR_KVM_WALL_CLOCK_NEW || msr_number == HYPE_MSR_KVM_WALL_CLOCK_OLD) {
        if (is_write) {
            real->pvclock_wall_msr =
                ((uint64_t)(uint32_t)real->gprs[2] << 32) | (uint64_t)(uint32_t)real->vmcb->save.rax;
            hype_svm_pvclock_arm_wall_clock(real, real->pvclock_wall_msr);
        } else {
            real->vmcb->save.rax = (uint64_t)(uint32_t)real->pvclock_wall_msr;
            real->gprs[2] = (uint64_t)(uint32_t)(real->pvclock_wall_msr >> 32);
        }
        real->vmcb->save.rip += 2;
        return 0;
    }

    /* IA32_PAT (0x277): emulated into the VMCB's own g_pat, which VMRUN
     * loads for the guest under nested paging -- per-guest and isolated.
     * Must stay intercepted (not passed through like the VMSAVE-managed
     * MSRs): PAT is not context-switched by VMSAVE/VMLOAD, so a native
     * guest write would corrupt the host's page-attribute table. */
    if (msr_number == 0x277u) {
        if (is_write) {
            real->vmcb->save.g_pat =
                ((uint64_t)(uint32_t)real->gprs[2] << 32) | (uint64_t)(uint32_t)real->vmcb->save.rax;
        } else {
            real->vmcb->save.rax = (uint64_t)(uint32_t)real->vmcb->save.g_pat;
            real->gprs[2] = (uint64_t)(uint32_t)(real->vmcb->save.g_pat >> 32);
        }
        real->vmcb->save.rip += 2;
        return 0;
    }

#if defined(HYPE_ENABLE_X2APIC) && HYPE_ENABLE_X2APIC
    /*
     * #601: IA32_APIC_BASE mode transitions and the x2APIC MSR range
     * (0x800-0x8FF), gated on this build flag so the default build's MSR
     * dispatch is untouched -- APIC_BASE writes still fall through to the
     * REJECT/absorb action below and 0x800-0x8FF is still unrecognized,
     * exactly as before #601.
     *
     * Both are handled here, ahead of hype_msr_decide_ex(), for the same
     * reason the MTRR/g_pat/TSC_AUX special cases above are: they need this
     * vCPU's own state (the guest LAPIC's current mode), which is not the
     * static, context-free allow-list hype_msr_decide_ex() answers from.
     */
    if (msr_number == HYPE_MSR_NUMBER_APIC_BASE && is_write) {
        uint64_t requested =
            ((uint64_t)(uint32_t)real->gprs[2] << 32) | (uint64_t)(uint32_t)real->vmcb->save.rax;
        int want_en = (requested & (1ULL << 11)) != 0;
        int want_extd = (requested & (1ULL << 10)) != 0;
        int next_mode;
        if (hype_apic_base_mode_transition((int)lapic->apic_mode, want_en, want_extd, &next_mode) !=
            0) {
            return 1; /* illegal transition (SDM state machine) -- caller injects #GP(0) */
        }
        hype_guest_lapic_set_apic_mode(lapic, (uint32_t)next_mode);
        real->vmcb->save.rip += 2;
        return 0;
    }
    if (hype_msr_is_x2apic_range(msr_number)) {
        /* #601: HYPE_GUEST_LAPIC_MODE_X2APIC and HYPE_APIC_MODE_X2APIC are the
         * same numeric value by construction (see msr_emulate.h) -- the LAPIC
         * model is asked directly rather than duplicating the mode here. */
        if (is_write) {
            uint64_t value =
                ((uint64_t)(uint32_t)real->gprs[2] << 32) | (uint64_t)(uint32_t)real->vmcb->save.rax;
            if (hype_guest_lapic_x2apic_write(lapic, msr_number, value) != 0) {
                return 1; /* not in x2APIC mode, or an illegal register/value -- #GP(0) */
            }
        } else {
            uint64_t value;
            if (hype_guest_lapic_x2apic_read(lapic, msr_number, &value) != 0) {
                return 1;
            }
            svm_msr_return(real, value);
        }
        real->vmcb->save.rip += 2;
        return 0;
    }
#endif

    action = hype_msr_decide_ex(msr_number, is_write, real->hv_enabled);

    switch (action) {
    /*
     * M7-1 (#91): Hyper-V synthetic MSRs. Only reachable when the Hyper-V CPUID
     * leaves are enabled -- hype_msr_decide_ex() gates them on that, so a Linux guest
     * still gets the fail-closed absorb here.
     */
    case HYPE_MSR_ACTION_READWRITE_HV_GUEST_OS_ID:
        if (is_write) {
            real->hv_guest_os_id = (((uint64_t)(uint32_t)real->gprs[2] << 32) |
                                    (uint64_t)(uint32_t)real->vmcb->save.rax);
            if (real->hv_guest_os_id == 0u) {
                real->hv_hypercall = hype_hv_hypercall_disable(real->hv_hypercall);
            }
        } else {
            svm_msr_return(real, real->hv_guest_os_id);
        }
        break;
    case HYPE_MSR_ACTION_READWRITE_HV_HYPERCALL:
        if (is_write) {
            uint64_t requested = (((uint64_t)(uint32_t)real->gprs[2] << 32) |
                                  (uint64_t)(uint32_t)real->vmcb->save.rax);
            uint64_t effective;
            if (hype_hv_hypercall_page_write(real->hv_hypercall, requested,
                                             real->hv_guest_os_id, real->pvclock_map,
                                             HYPE_HV_HYPERCALL_VENDOR_SVM, &effective) != 0) {
                /* The caller injects #GP(0) at this WRMSR. */
                return 1;
            }
            real->hv_hypercall = effective;
        } else {
            svm_msr_return(real, real->hv_hypercall);
        }
        break;
    case HYPE_MSR_ACTION_READ_HV_VP_INDEX:
        /*
         * Always 0. hype gives each guest exactly one vCPU, so this guest IS
         * VP 0 -- the pool slot index (1 for VM1) would be a lie: it is hype's
         * index across partitions, and a one-VP partition reporting VP 1 makes
         * Windows address a processor that does not exist. When guest SMP lands
         * this becomes the vCPU-within-VM index.
         */
        svm_msr_return(real, 0ULL);
        break;
    case HYPE_MSR_ACTION_READWRITE_HV_REFERENCE_TSC:
        if (is_write) {
            uint64_t requested = (((uint64_t)(uint32_t)real->gprs[2] << 32) |
                                  (uint64_t)(uint32_t)real->vmcb->save.rax);
            uint64_t effective;
            if (hype_hv_reference_tsc_write(requested, real->pvclock_map, g_acpi_pm_tsc_hz,
                                            &effective) != 0) {
                return 1; /* enabled page outside the map: caller injects #GP(0) */
            }
            real->hv_ref_tsc = effective;
        } else {
            svm_msr_return(real, real->hv_ref_tsc);
        }
        break;
    case HYPE_MSR_ACTION_READ_HV_TSC_FREQUENCY:
        /* #436: the guest's TSC runs at the raw host rate (no intercept, tsc_offset=0),
         * so report the calibrated host TSC frequency. Windows bootlib uses this
         * instead of self-calibrating, whose result under hype was wildly wrong. */
        svm_msr_return(real, g_acpi_pm_tsc_hz);
        break;
    case HYPE_MSR_ACTION_READ_HV_APIC_FREQUENCY:
        /* Report the APIC bus frequency OVMF's LocalApicTimerDxe assumes for this
         * platform (PcdFSBClock 1 GHz) -- consistent with the firmware's own view. */
        svm_msr_return(real, 1000000000ull);
        break;
    case HYPE_MSR_ACTION_READ_HV_TIME_REF_COUNT: {
        /*
         * 100ns ticks. Measured from the raw host TSC rather than a per-partition
         * epoch: a guest only ever uses DIFFERENCES of this counter, and using the
         * raw TSC keeps it monotonic across the guest's own reset without needing a
         * base to maintain.
         */
        svm_msr_return(real, hype_msr_hv_ref_count_from_tsc(real_rdtsc(), g_acpi_pm_tsc_hz / 1000u));
        break;
    }

    case HYPE_MSR_ACTION_READ_APIC_BASE: {
#if defined(HYPE_ENABLE_X2APIC) && HYPE_ENABLE_X2APIC
        /* #601: report the LAPIC's ACTUAL current mode instead of hardcoding
         * "always xAPIC-enabled" -- the WRMSR path above is what can now move it. */
        uint64_t value =
            hype_msr_apic_base_value_mode(real->cpuid_topo.apic_id == 0u, (int)lapic->apic_mode);
#else
        uint64_t value = hype_msr_apic_base_value(real->cpuid_topo.apic_id == 0u);
#endif
        real->vmcb->save.rax = (uint64_t)(uint32_t)value;
        real->gprs[2] = (uint64_t)(uint32_t)(value >> 32); /* RDX */
        break;
    }
    case HYPE_MSR_ACTION_READWRITE_EFER:
        if (is_write) {
            uint64_t requested =
                ((uint64_t)(uint32_t)real->gprs[2] << 32) | (uint64_t)(uint32_t)real->vmcb->save.rax;
            uint64_t value = 0;
            /*
             * #316: never store the guest's value verbatim. VMRUN refuses a VMCB whose guest
             * EFER has SVME clear, so a guest that rebuilds EFER from zero -- OpenBSD's kernel
             * does exactly that -- used to take hype down with it on the next entry. An illegal
             * write now becomes the guest's own #GP(0), which is what real hardware does, and
             * keeps a faulted guest contained to itself (AGENTS.md).
             */
            if (hype_svm_guest_efer_write(real->vmcb->save.efer, requested, real->vmcb->save.cr0,
                                          real->vmcb->save.cr4, &value) != 0) {
                hype_svm_vcpu_reinject_exception(ctx, 13u, 1, 0u); /* #GP(0) */
                return 0;                                         /* faulted: do NOT skip the WRMSR */
            }
            real->vmcb->save.efer = value;
        } else {
            uint64_t value = hype_svm_guest_efer_read(real->vmcb->save.efer);
            real->vmcb->save.rax = (uint64_t)(uint32_t)value;
            real->gprs[2] = (uint64_t)(uint32_t)(value >> 32);
        }
        break;
    case HYPE_MSR_ACTION_READ_TSC: {
        uint64_t tsc = real_rdtsc() + real->vmcb->control.tsc_offset;
        real->vmcb->save.rax = (uint64_t)(uint32_t)tsc;
        real->gprs[2] = (uint64_t)(uint32_t)(tsc >> 32);
        break;
    }
    case HYPE_MSR_ACTION_REJECT:
    default:
        if (g_msr_trace) {
            /* M4-6d3: Linux writes SPEC_CTRL (0x48) and PRED_CMD (0x49) on
             * nearly every kernel entry/exit for Spectre mitigation. The
             * per-write GOP-rendered trace floods the console and slows
             * the boot to a crawl, so stub those SILENTLY. Every other
             * unknown MSR is traced ONCE (latched by number) for
             * discovery -- enough to see what a real boot touches without
             * a repeating flood. The stub behaviour (WRMSR ignored, RDMSR
             * returns 0) is unchanged. */
            int silent = (msr_number == 0x48u || msr_number == 0x49u);
            if (!silent) {
                static uint32_t seen_msrs[128];
                static unsigned seen_count = 0;
                unsigned k;
                int already = 0;
                for (k = 0; k < seen_count; k++) {
                    if (seen_msrs[k] == msr_number) {
                        already = 1;
                        break;
                    }
                }
                if (!already) {
                    uint64_t wval = ((uint64_t)(uint32_t)real->gprs[2] << 32) |
                                    (uint64_t)(uint32_t)real->vmcb->save.rax;
                    hype_debug_print("msr-trace: unhandled %s msr=0x%x val=0x%llx rip=0x%llx\n",
                                      is_write ? "WRMSR" : "RDMSR", (unsigned int)msr_number,
                                      (unsigned long long)(is_write ? wval : 0ULL),
                                      (unsigned long long)real->vmcb->save.rip);
                    if (seen_count < 128u) {
                        seen_msrs[seen_count++] = msr_number;
                    }
                }
            }
        }
        /*
         * Absorb the unmodelled MSR: WRMSR ignored, RDMSR returns 0.
         *
         * This used to sit INSIDE the `if (g_msr_trace)` above, which coupled
         * "tolerate an unknown MSR" to "trace MSR accesses" -- so turning
         * tracing off silently made every unmodelled MSR fatal. That is not a
         * theoretical hazard: it killed a working guest the moment the trace
         * call was treated as pure diagnostics and gated, and it presented as
         * `unhandled guest MSR access msr=0x8b` (IA32_BIOS_SIGN_ID, read by
         * Linux's microcode init) at a kernel RIP, with nothing pointing at the
         * trace flag.
         *
         * Absorbing is BEHAVIOUR and belongs here unconditionally -- the same
         * absorb-rather-than-die posture GLADDER-1 established for unmodelled
         * MMIO. Tracing is diagnostics and stays gated above. A guest that
         * reads an MSR hype does not model gets 0, which is what it would get
         * from a CPU lacking the feature, rather than taking down the host.
         */
        if (!is_write) {
            real->vmcb->save.rax = 0;
            real->gprs[2] = 0;
        }
        real->vmcb->save.rip += 2;
        return 0;
    }

    real->vmcb->save.rip += 2; /* RDMSR/WRMSR are always exactly 2 bytes (0F 32 / 0F 30) */
    return 0;
}

int hype_svm_vcpu_handle_hypercall(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;

    if (real == 0 || !real->hv_enabled ||
        (real->hv_hypercall & HYPE_HV_HYPERCALL_ENABLE) == 0u) {
        return -1;
    }
    real->vmcb->save.rax = hype_hv_hypercall_dispatch(real->gprs[1]);
    if (real->vmcb->control.nrip > real->vmcb->save.rip) {
        real->vmcb->save.rip = real->vmcb->control.nrip;
    } else {
        real->vmcb->save.rip += 3u; /* VMMCALL: 0F 01 D9 */
    }
    return 0;
}

/* Max bytes hype_mmio_decode() could ever need for the narrow
 * instruction forms it supports (prefix + REX + two-byte opcode +
 * ModRM + SIB + disp32 -- the longest case, MOVZX with a SIB-plus-
 * disp32 memory operand) -- comfortably under x86's own 15-byte
 * legal-instruction-length limit. */
#define HYPE_MMIO_MAX_INSTR_BYTES 15

/* VALID-3 guest-physical -> host translation for host-side guest-memory
 * access (the AHCI DMA path, the fw_cfg DMA + string-I/O paths).
 * A NULL dma_map means "trusted identity-mapped guest" (the M4-5/ISO-2/
 * PCI-2/M5-2/M4-4/VIDEO-2 cooperating test guests, whose NPT identity-maps
 * RAM so guest-physical == host and whose DMA addresses this project itself
 * wrote) -- return the address unchecked, preserving their exact prior
 * behavior. A non-NULL map (FW-1's real OVMF/OS guest, whose RAM is
 * AllocateAnyPages-backed and NPT-remapped, so guest-physical != host)
 * routes every guest-supplied address through the bounds-checked VALID-1
 * lookup with its access length; a 0 return (out of range / straddling /
 * overrun / overflow) propagates as "reject" to the caller. */
static uint64_t guest_dma_xlate(const hype_gpa_map_t *dma_map, uint64_t gpa, uint64_t len) {
    if (dma_map == 0) {
        return gpa;
    }
    return hype_gpa_to_host(dma_map, gpa, len);
}

/* Copy n bytes 8 at a time (byte tail last) into a PRDT-described guest buffer.
 * __builtin_memcpy with a constant size lowers to a single unaligned mov
 * (x86_64 allows unaligned access), so there is no libc/memcpy dependency (this
 * is a freestanding build) and no strict-aliasing UB. ~8x fewer store ops than
 * the old byte loop for the flat-media / IDENTIFY PRDT copies. The streamed-media
 */
static void ahci_copy_fast(uint8_t *dst, const uint8_t *src, uint32_t n) {
    uint32_t k = 0;
    while (k + 8u <= n) {
        __builtin_memcpy(dst + k, src + k, 8);
        k += 8u;
    }
    while (k < n) {
        dst[k] = src[k];
        k++;
    }
}

/* Walks the guest's Command List (slot 0 only, this project's own
 * single-outstanding-command scope) -> Command Table -> ATAPI CDB,
 * dispatches it, copies the response into the PRDT-described guest
 * buffer(s), and updates the port's completion-observable state.
 * Every guest-memory access here is a plain pointer dereference, same
 * flat-identity-map reasoning as hype_svm_vcpu_handle_npf()'s own
 * instruction-byte fetch. Returns 0 if the command was a recognized
 * ATAPI PACKET command, -1 otherwise (a raw ATA command, or a Command
 * FIS that isn't even a Register H2D FIS) -- this project's own scope
 * is "one ATAPI CD-ROM," never a raw ATA disk on this port, so
 * anything else is fail-closed rather than guessed at, matching every
 * other MMIO/NPF handler's convention here. */
/* Non-static (VMX-2): vendor-neutral AHCI command-slot processor -- takes no
 * vcpu context, only the device models + guest DMA map, so the VMX MMIO handler
 * (vmcs_hw.c) reuses it verbatim rather than duplicating the command-list/PRDT/
 * FIS DMA. Declared in devices/ahci.h. */
/*
 * #309: complete an AHCI software reset. See hype_ahci_soft_reset() for the protocol; this
 * is the part that needs the guest's Received-FIS area, so it lives with the other
 * DMA-touching code rather than in the device model.
 *
 * Returns 0 on success (the slot is completed either way), -1 only if the guest's FIS area
 * fails its VALID-3 bounds check.
 */
static int complete_ahci_soft_reset(hype_ahci_t *ahci, uint64_t rx_fis_phys,
                                    const hype_gpa_map_t *dma_map, unsigned slot,
                                    uint8_t control_byte) {
    uint8_t fis[20];
    uint8_t *rx_fis_host;
    unsigned i;

    if (!hype_ahci_soft_reset(ahci, control_byte, slot)) {
        return 0; /* SRST asserted, or a Control write announcing nothing: no FIS to post */
    }

    rx_fis_host = (uint8_t *)(uintptr_t)guest_dma_xlate(dma_map, rx_fis_phys, 0x40u + 20u);
    if (rx_fis_host == 0) {
        hype_debug_print("ahci: slot %u reset -- received-FIS area gpa 0x%llx out of bounds\n",
                         slot, (unsigned long long)rx_fis_phys);
        return -1;
    }
    hype_ahci_build_signature_fis(fis, (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_DSC), 0,
                                  ahci->p_sig);
    for (i = 0; i < 20u; i++) {
        rx_fis_host[0x40 + i] = fis[i];
    }
    hype_ahci_set_pis(ahci, HYPE_AHCI_PIS_DHRS); /* #512: counted edge */
    if ((ahci->p_is & ahci->p_ie) != 0) {
        ahci->is |= HYPE_AHCI_IS_PORT0;
    }
    return 0;
}

/* #344: bounded completion trace, see its use below. */
static unsigned int g_atapi_completion_traced;

/*
 * #372: refuse a command when the guest has not enabled PCI Bus Master.
 *
 * Every structure this function touches -- the command list, the command table, each PRD's data
 * pointer, the receive FIS -- is reached by the controller MASTERING THE BUS. With BME clear the
 * hardware cannot issue any of those cycles, so the command sits in PxCI and never retires, and a
 * driver polling for completion spins forever. That is the failure a guest driver which forgot to
 * set the bit must be allowed to see here.
 *
 * Leaving PxCI set is the whole behaviour: returning "done" or clearing the slot would hide it.
 * Said once on the diagnostic channel, because an operator debugging their own guest driver
 * deserves the reason rather than hype's silence -- which would only move the confusion one layer
 * down, and is the same mistake as a counter that cannot observe its own subject.
 */
static int ahci_bus_master_refused(const hype_ahci_t *ahci, const char *what) {
    static int reported;
    if (ahci->bus_master != 0) {
        return 0;
    }
    if (!reported) {
        reported = 1;
        hype_debug_print("ahci: %s IGNORED -- the guest has not set PCI Bus Master Enable "
                         "(Command bit 2), so the controller cannot reach the command list or any "
                         "PRD. PxCI stays set and this command will never complete, exactly as on "
                         "real hardware. [#372]\n",
                         what);
    }
    return 1;
}

int process_ahci_command_slot(hype_ahci_t *ahci, hype_atapi_t *atapi,
                              const hype_gpa_map_t *dma_map, unsigned slot) {
    uint64_t cmd_list_phys =
        ((uint64_t)ahci->p_clb | ((uint64_t)ahci->p_clbu << 32)) + (uint64_t)slot * 32u;
    uint64_t rx_fis_phys = (uint64_t)ahci->p_fb | ((uint64_t)ahci->p_fbu << 32);
    uint8_t *cmd_hdr_bytes;
    hype_ahci_cmd_header_t hdr;
    const uint8_t *cmd_table_bytes;
    const uint8_t *prdt_bytes;
    uint8_t *rx_fis_host;
    hype_atapi_result_t result;
    uint8_t identify[HYPE_ATAPI_IDENTIFY_SIZE];
    int media_read_failed = 0; /* #287: backing-store read failed -> complete with ERR */
    const uint8_t *src;
    /* Default 0: the ATA paths (IDENTIFY PACKET / SET FEATURES) and the synth
     * ATAPI responses copy from a flat `src`; only a media-data ATAPI read on a
     * streamed backing sets this to 1 (below). Must be initialised or those paths
     * would take the streamed read with a stale media_offset -> spurious failure. */
    int stream_media = 0; /* GLADDER-10: media served on demand from a raw disk partition */
    uint64_t media_byte_off = 0; /* GLADDER-10(b): 64-bit byte offset = media_lba * sector size */
    uint32_t remaining;
    uint32_t transferred;
    uint32_t prd_idx;
    uint8_t status_reg;
    uint8_t error_reg;
    uint32_t pis_bit;
    int packet_pio_in = 0;
    uint8_t *d2h_fis;
    unsigned i;

    /* #372: before any of it -- can this controller master the bus at all? Returning 0 (not -1)
     * because nothing is WRONG: the guest asked for something the hardware would silently not do,
     * and the caller must not treat that as a decode failure and panic. PxCI stays set. */
    if (ahci_bus_master_refused(ahci, "PxCI write")) {
        return 0;
    }

    /* VALID-3: every guest-physical address the AHCI command structures
     * carry is guest-controlled, so each is translated through the VM's
     * bounds-checked gpa map (VALID-1) -- with its access length -- and
     * a rejected (0) translation fails the command rather than
     * dereferencing an out-of-range host pointer. The command header is
     * the 32-byte slot-0 entry. */
    cmd_hdr_bytes = (uint8_t *)(uintptr_t)guest_dma_xlate(dma_map, cmd_list_phys, 32u);
    if (cmd_hdr_bytes == 0) {
        /* #309: every refusal here is reported unconditionally, not behind g_ahci_trace.
         * The only caller treats -1 as fatal and panics with "unhandled AHCI ABAR MMIO",
         * which names the PxCI register rather than the command that was actually refused
         * -- one message covering a decoder gap, an unmodelled register and a rejected
         * command. Whatever the reason, it is worth a line when the guest is about to die. */
        hype_debug_print("ahci: slot %u refused -- command list at gpa 0x%llx out of bounds\n",
                         slot, (unsigned long long)cmd_list_phys);
        return -1;
    }

    hype_ahci_decode_cmd_header(cmd_hdr_bytes, &hdr);
    /* Command Table = 0x80-byte CFIS/ACMD/reserved block + prdtl 16-byte
     * PRDT entries. A malicious prdtl that would run the table off the
     * region is caught here (the length is computed in 64-bit so it
     * cannot wrap before the check). */
    cmd_table_bytes = (const uint8_t *)(uintptr_t)guest_dma_xlate(
        dma_map, hdr.cmd_table_phys, (uint64_t)0x80u + (uint64_t)hdr.prdtl * 16u);
    if (cmd_table_bytes == 0) {
        hype_debug_print("ahci: slot %u refused -- command table at gpa 0x%llx (prdtl=%u) out of "
                         "bounds\n",
                         slot, (unsigned long long)hdr.cmd_table_phys, (unsigned int)hdr.prdtl);
        return -1;
    }

    if (!hdr.is_atapi) {
        /* A plain H2D Register FIS command (Command Header's ATAPI bit
         * clear). A real AHCI driver issues two of these to an ATAPI
         * device during setup (EDK2 AhciModeInitialization):
         *   - IDENTIFY PACKET DEVICE (0xA1): PIO data-in of the fixed
         *     512-byte identify block. The driver waits for a PIO Setup
         *     FIS (PxIS.PSS) and requires PRDBC == 512.
         *   - SET FEATURES (0xEF): a no-data command selecting the
         *     transfer mode -- acknowledged with a data-less success
         *     (D2H FIS, PxIS.DHRS). */
        uint8_t ata_cmd = cmd_table_bytes[2];
        if (cmd_table_bytes[0] != 0x27u) {
            hype_debug_print("ahci: slot %u refused -- not a Register H2D FIS (type=0x%x cmd=0x%x)\n",
                             slot, (unsigned int)cmd_table_bytes[0], (unsigned int)ata_cmd);
            return -1;
        }
        /* #309: a Control-register write (C bit clear), not a command -- the software-reset
         * protocol FreeBSD runs before it will probe the port at all. */
        if (hype_ahci_h2d_is_control_write(cmd_table_bytes)) {
            return complete_ahci_soft_reset(ahci, rx_fis_phys, dma_map, slot, cmd_table_bytes[15]);
        }
        if (ata_cmd == HYPE_AHCI_ATA_CMD_IDENTIFY_PACKET_DEVICE) {
            hype_atapi_build_identify(atapi, identify);
            src = identify;
            remaining = HYPE_ATAPI_IDENTIFY_SIZE;
            status_reg = 0x50u; /* DRDY|DSC */
            error_reg = 0;
            /* #358: both bits, for the same reason as IDENTIFY DEVICE on the disk port -- this is
             * also a PIO data-in. It has worked with PSS alone because EDK2's ATAPI probe is
             * satisfied by the PIO Setup FIS, so this half is a correctness fix rather than a fix
             * for an observed failure; validated in the same run as the disk change, and the CD
             * still booting is the check that matters. */
            pis_bit = HYPE_AHCI_PIS_DHRS | HYPE_AHCI_PIS_PSS;
            if (g_ahci_trace) {
                hype_debug_print("ahci-trace: IDENTIFY PACKET DEVICE (0xA1) -> 512-byte PIO-in\n");
            }
        } else if (ata_cmd == HYPE_AHCI_ATA_CMD_SET_FEATURES) {
            src = identify; /* unused: no data transferred (remaining == 0) */
            remaining = 0;
            status_reg = 0x50u; /* DRDY|DSC, no error */
            error_reg = 0;
            pis_bit = HYPE_AHCI_PIS_DHRS;
            if (g_ahci_trace) {
                hype_debug_print("ahci-trace: SET FEATURES (0xEF) -> no-data ack\n");
            }
        } else {
            hype_debug_print("ahci: slot %u refused -- unmodelled ATA command 0x%x on the ATAPI "
                             "port (FIS type=0x%x)\n",
                             slot, (unsigned int)ata_cmd, (unsigned int)cmd_table_bytes[0]);
            return -1;
        }
    } else {
        uint8_t cdb[HYPE_ATAPI_CDB_MAX];
        if (cmd_table_bytes[0] != 0x27u || cmd_table_bytes[2] != 0xA0u) {
            /* not a Register H2D FIS carrying ATA_CMD_PACKET (0xA0) */
            hype_debug_print("ahci: slot %u refused -- ATAPI header but FIS type=0x%x cmd=0x%x, "
                             "expected 0x27/0xa0\n",
                             slot, (unsigned int)cmd_table_bytes[0], (unsigned int)cmd_table_bytes[2]);
            return -1;
        }
        for (i = 0; i < HYPE_ATAPI_CDB_MAX; i++) {
            cdb[i] = cmd_table_bytes[0x40 + i];
        }

        hype_atapi_execute_cdb(atapi, cdb, &result);

        if (g_ahci_trace) {
            /* #318: print the DECODED 32-bit LBA and block count for the read commands, not two
             * raw CDB bytes -- the whole point of this trace is comparing the requested LBAs
             * against the ISO's real directory extents, which bytes 2 and 5 alone cannot do. */
            uint32_t t_lba = ((uint32_t)cdb[2] << 24) | ((uint32_t)cdb[3] << 16) |
                             ((uint32_t)cdb[4] << 8) | (uint32_t)cdb[5];
            uint32_t t_cnt = (cdb[0] == 0xA8u)
                                 ? (((uint32_t)cdb[6] << 24) | ((uint32_t)cdb[7] << 16) |
                                    ((uint32_t)cdb[8] << 8) | (uint32_t)cdb[9])
                                 : (((uint32_t)cdb[7] << 8) | (uint32_t)cdb[8]);
            hype_debug_print(
                "ahci-trace: ATAPI CDB=0x%x lba=%u count=%u status=%s uses_media=%u len=%u\n",
                (unsigned int)cdb[0], (unsigned int)t_lba, (unsigned int)t_cnt,
                result.status == HYPE_ATAPI_STATUS_GOOD ? "GOOD" : "CHECK",
                (unsigned int)result.uses_media_data,
                (unsigned int)(result.uses_media_data ? result.media_length : result.synth_length));
        }

        /* GLADDER-10(a): media may be backed by a CHUNKED (non-contiguous) ISO
         * rather than a flat buffer. For flat media/synth, `src` is a plain
         * pointer advanced per PRD; for streamed media, `src` is unused and each
         * PRD reads from the chunk list at logical offset media_byte_off+transferred.
         * GLADDER-10(b): the byte offset is derived here from the 32-bit start
         * sector (media_lba) with a 64-bit multiply, so a >=4GB ISO (byte offset
         * past UINT32_MAX) addresses the right bytes -- the result struct only
         * needs to carry a 32-bit sector index (good to 8TB). */
        media_byte_off = (uint64_t)result.media_lba * (uint64_t)HYPE_ATAPI_SECTOR_SIZE;
        stream_media = result.uses_media_data && atapi->media_stream != 0;
        src = (result.uses_media_data && !stream_media)
                  ? (atapi->media_data + media_byte_off)
                  : (result.uses_media_data ? 0 : result.synth_data);
        remaining = result.uses_media_data ? result.media_length : result.synth_length;
        /* ATA STATUS register: DRDY|DSC always, +ERR on CHECK_CONDITION.
         * ATAPI convention: a failed PACKET command's ERROR register
         * carries the SCSI sense key in its upper nibble. */
        status_reg = (result.status == HYPE_ATAPI_STATUS_GOOD) ? 0x50u : 0x51u;
        error_reg = (result.status == HYPE_ATAPI_STATUS_GOOD) ? 0u : (uint8_t)(atapi->sense_key << 4);
        /* ATAPI PACKET data/no-data commands complete with a Device-to-
         * Host Register FIS (EDK2's AhciPioTransfer/AhciNonDataTransfer
         * wait on PxIS.DHRS for them). */
        pis_bit = HYPE_AHCI_PIS_DHRS;
        /* #318: ...but a PACKET command that moves data in PIO mode must ALSO be given a PIO
         * Setup FIS carrying the byte count, which is how a driver that reads the receive area
         * (rather than just the PxIS bit, as EDK2 does) learns how much arrived. Without it
         * OpenBSD's atapiscsi treats every READ(10) as suspect and re-interrogates the device
         * with TEST UNIT READY + REQUEST SENSE, tripling the command count per sector.
         * H2D Features bit 0 is the ATAPI DMA bit: set means a DMA transfer, which ends with
         * the D2H FIS alone and no PIO Setup. */
        packet_pio_in = (cmd_table_bytes[3] & 0x01u) == 0;
    }

    prdt_bytes = cmd_table_bytes + 0x80;
    prd_idx = 0;
    transferred = 0;
    g_atapi_xfers++;
    {
        /* #343: what the command ASKED for, before the PRDT list can cut it short. */
        g_atapi_req_bytes += (uint64_t)remaining;
    }
    while (remaining > 0 && prd_idx < hdr.prdtl) {
        hype_ahci_prdt_entry_t prd;
        uint32_t chunk;
        uint8_t *dst;

        hype_ahci_decode_prdt_entry(prdt_bytes + (uint32_t)prd_idx * 16u, &prd);
        chunk = (prd.byte_count < remaining) ? prd.byte_count : remaining;
        /* VALID-3: the PRD data buffer is guest-supplied -- bounds-check
         * [data_phys, data_phys+chunk) before writing the response into
         * it, so a guest-programmed PRD can never steer the copy at
         * hypervisor or another VM's memory. */
        dst = (uint8_t *)(uintptr_t)guest_dma_xlate(dma_map, prd.data_phys, chunk);
        if (dst == 0) {
            hype_debug_print("ahci: slot %u refused -- PRD %u buffer gpa 0x%llx len %u out of "
                             "bounds\n",
                             slot, (unsigned int)prd_idx, (unsigned long long)prd.data_phys,
                             (unsigned int)chunk);
            return -1;
        }
        if (stream_media) {
            /* GLADDER-10: fetch these bytes on demand from the raw ISO partition
             * (disk read via hype_ahci_host_read) instead of a RAM copy. */
            static unsigned g_stream_dbg = 0;
            int srr = hype_iso_stream_read(atapi->media_stream, media_byte_off + transferred, dst,
                                           chunk);
#if HYPE_343_VERIFY_READS
            /*
             * #343: read the SAME range again and compare it against what was just written into
             * guest memory.
             *
             * The guest faults on a page of its own kernel image that is absent, which is a hole in
             * a loaded file rather than a truncated one -- so the question is whether hype ever
             * hands the guest something other than the ISO's bytes. Aggregate counters have
             * answered what they can (stream failures zero, ATAPI short transfers identical in
             * clean runs); this compares content, per read, which is the only thing left that can
             * distinguish "delivered wrong bytes" from "delivered fine and the guest lost the page".
             *
             * DIAGNOSTIC ONLY, compile-time gated: it doubles the reads on this path. A mismatch is
             * reported with the offset and the first diverging byte so the failing range can be
             * matched against the kernel image's own layout.
             */
            if (srr == 0) {
                static uint8_t v343[4096];
                static unsigned v343_reported = 0;
                uint32_t vlen = (chunk <= sizeof(v343)) ? chunk : (uint32_t)sizeof(v343);
                if (hype_iso_stream_read(atapi->media_stream, media_byte_off + transferred, v343,
                                         vlen) == 0) {
                    uint32_t vi;
                    g_343_verified++;
                    for (vi = 0; vi < vlen; vi++) {
                        if (v343[vi] != dst[vi]) {
                            g_343_mismatch++;
                            if (v343_reported < 8u) {
                                v343_reported++;
                                hype_debug_print("fw-1 #343 MISMATCH: iso_off=%llu +%u delivered=%02x "
                                                 "reread=%02x (chunk=%u)\n",
                                                 (unsigned long long)(media_byte_off + transferred),
                                                 (unsigned)vi, (unsigned)dst[vi], (unsigned)v343[vi],
                                                 (unsigned)chunk);
                            }
                            break;
                        }
                    }
                }
            }
#endif
            /* #346: the loader stops after reading root-dir LBA 51 and never fetches /etc
             * (LBA 56), so dump the exact bytes delivered for the DIRECTORY sectors -- those
             * decide what it looks for next. QEMU reads 56; real hardware does not, with every
             * layer below byte-perfect, so what it PARSED from 51 is the open question. */
            if (result.media_lba == 51u || result.media_lba == 56u) {
                hype_debug_print("dirsec lba=%llu off=%llu: %02x %02x %02x %02x %02x %02x %02x %02x "
                                 "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                                 (unsigned long long)result.media_lba,
                                 (unsigned long long)(media_byte_off + transferred),
                                 dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], dst[6], dst[7],
                                 dst[8], dst[9], dst[10], dst[11], dst[12], dst[13], dst[14],
                                 dst[15]);
            }
            if (g_stream_dbg < 24u || srr != 0) {
                g_stream_dbg++;
                /* #346: include the first 8 bytes AS DELIVERED TO GUEST RAM. On real hardware the
                 * guests retry LBA 0/16 forever with every layer below proven byte-perfect -- this
                 * shows whether the LAST hop (this very copy) is where the bytes go wrong. */
                hype_debug_print("stream-rd #%u: off=%llu chunk=%u lba0=%llu isosz=%llu ret=%d "
                                 "dst=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                                 g_stream_dbg, (unsigned long long)(media_byte_off + transferred),
                                 (unsigned)chunk,
                                 (unsigned long long)atapi->media_stream->part_start_lba,
                                 (unsigned long long)atapi->media_stream->iso_size, srr,
                                 dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], dst[6], dst[7]);
            }
            if (srr != 0) {
                /*
                 * #287: a BACKING-STORE failure is not "this is not my command".
                 *
                 * Returning -1 here meant the caller fell through to its
                 * unhandled-MMIO path and PANICKED on the guest's next perfectly
                 * ordinary ABAR write -- blaming a register that is in fact modelled,
                 * eleven log lines away from the read that actually failed. Any
                 * transient host-disk error took down the hypervisor and every guest.
                 *
                 * Report what a real drive reports instead: MEDIUM ERROR / unrecovered
                 * read error. Guests and firmware both know how to handle that, and
                 * hype stays up. Same spirit as GLADDER-1 absorbing unhandled MMIO
                 * rather than dying.
                 */
                hype_atapi_set_media_error(atapi, HYPE_ATAPI_SENSE_KEY_MEDIUM_ERROR,
                                           HYPE_ATAPI_ASC_UNRECOVERED_READ_ERROR);
                media_read_failed = 1;
                break;
            }
        } else {
            ahci_copy_fast(dst, src, chunk);
            src += chunk;
        }
        remaining -= chunk;
        transferred += chunk;
        prd_idx++;
    }

    /*
     * #287: a backing-store read failed part-way. Complete the command with ERR set
     * and the sense already stashed, rather than returning early -- an early return
     * leaves PxCI set and the guest waits on a command that will never finish, which
     * is a hang instead of an error. PRDBC below reports the partial count, which is
     * what a real drive does on a short/failed transfer.
     */
    if (media_read_failed) {
        status_reg = (uint8_t)(0x50u | 0x01u); /* DRDY|DSC|ERR */
        error_reg = (uint8_t)(atapi->sense_key << 4);
    }

    /*
     * #343: the loop above exits when the PRDT list runs out, NOT only when the request is
     * satisfied -- so a guest whose PRDT does not cover its own block count gets a SHORT transfer
     * reported as success, with PRDBC honestly reporting the short count. A driver that checks
     * PRDBC notices; one that does not believes it read the whole thing and carries on with a
     * partially-filled buffer. Count it, because that is a silent wrong-data path and the reason
     * this counter exists is a FreeBSD guest that page-faulted on a page of its own kernel image.
     *
     * A counter rather than a trace: the stream trace is capped at 24 records and the kernel load
     * happens long after those, which is precisely why the first attempt at this had no evidence
     * either way (see #356 for the same lesson).
     */
    if (remaining > 0u) {
        g_atapi_short_xfers++;
        g_atapi_owed_bytes += (uint64_t)remaining;
    }
    g_atapi_done_bytes += (uint64_t)transferred;

    /* PRDBC (Command Header dword 1, byte offset 4): the count of bytes
     * actually transferred. EDK2's PIO-in path (AhciPioTransfer, used by
     * IDENTIFY PACKET DEVICE) checks PRDBC == the requested DataCount and
     * fails the command otherwise, so it must be written back into the
     * guest's command header. Harmless for the other paths that ignore
     * it. */
    cmd_hdr_bytes[4] = (uint8_t)(transferred & 0xFFu);
    cmd_hdr_bytes[5] = (uint8_t)((transferred >> 8) & 0xFFu);
    cmd_hdr_bytes[6] = (uint8_t)((transferred >> 16) & 0xFFu);
    cmd_hdr_bytes[7] = (uint8_t)((transferred >> 24) & 0xFFu);

    ahci->p_tfd = (uint32_t)status_reg | ((uint32_t)error_reg << 8);

    /* VALID-3: the Received FIS area is guest-supplied. Validate
     * [rx_fis, rx_fis+0x54) (the D2H Register FIS sits at offset 0x40,
     * 20 bytes) as one range -- computing the +0x40 on the host pointer
     * after translation, so a near-top guest address cannot overflow
     * before the check. */
    rx_fis_host = (uint8_t *)(uintptr_t)guest_dma_xlate(dma_map, rx_fis_phys, 0x40u + 20u);
    if (rx_fis_host == 0) {
        hype_debug_print("ahci: slot %u refused -- received-FIS area gpa 0x%llx out of bounds\n",
                         slot, (unsigned long long)rx_fis_phys);
        return -1;
    }
    /*
     * #314: a PIO data-in command must ALSO deliver a PIO Setup FIS at receive-area offset
     * 0x20, not merely latch PxIS.PSS.
     *
     * #262 slice 4 added this to the plain-ATA path and its comment claims the ATAPI path
     * "already does" it for IDENTIFY PACKET -- it does not; it sets the bit and nothing else,
     * so this receive area stayed whatever the guest left there. EDK2 waits on the PxIS.PSS
     * BIT, so the CD has always worked; FreeBSD reads the FIS itself, and its
     * ATAPI_IDENTIFY timed out on a completion hype had already finished (cs 00000000,
     * tfd 50, is 00000002).
     */
    if (packet_pio_in && transferred > 0) {
        pis_bit |= HYPE_AHCI_PIS_PSS;
    }
    if ((pis_bit & HYPE_AHCI_PIS_PSS) != 0) {
        hype_ahci_build_pio_setup_fis(rx_fis_host + 0x20, status_reg, error_reg, transferred);
    }

    d2h_fis = rx_fis_host + 0x40;
    hype_ahci_build_d2h_fis(d2h_fis, 0, status_reg, error_reg);

    /*
     * #344: what hype actually PUBLISHED for this command, not merely that it completed.
     *
     * The wedge profile is 100% CPU with ZERO VM exits and no output -- a guest spinning on
     * memory it already owns, not on MMIO. Two explanations for that are now eliminated by
     * reading the code rather than by measurement: the FIS is posted (process_ahci_command_slot
     * is shared by both backends, so the "VMX never posts it" idea was wrong), and it is posted
     * BEFORE PxCI is cleared, on the guest's own vCPU inside a VM exit, so there is no window in
     * which the guest could see stale bytes.
     *
     * That leaves CONTENT. EDK2's AhciPioTransfer checks PRDBC against the count it asked for and
     * fails the command otherwise; a guest that then polls the FIS area waits forever on a
     * transfer it believes incomplete. So record the three things it reads -- the byte count
     * written back into the command header, the PIO Setup FIS, and the D2H Register FIS -- for
     * the first commands of a run, which is where the wedge happens.
     *
     * Bounded, because the stream trace being capped at 24 records is exactly why an earlier
     * attempt at this had no evidence either way (#356, and the #343 counter above).
     */
    if (g_atapi_completion_traced < 24u) {
        g_atapi_completion_traced++;
        hype_debug_print(
            "ahci-cpl #%u slot=%u xfer=%u short=%u tfd=0x%04x st=0x%02x err=0x%02x pis=0x%08x | "
            "pio[0..3]=%02x%02x%02x%02x cnt=%02x%02x | d2h[0..3]=%02x%02x%02x%02x [#344]\n",
            g_atapi_completion_traced, slot, (unsigned)transferred, (unsigned)remaining,
            (unsigned)ahci->p_tfd, (unsigned)status_reg, (unsigned)error_reg, (unsigned)pis_bit,
            rx_fis_host[0x20], rx_fis_host[0x21], rx_fis_host[0x22], rx_fis_host[0x23],
            rx_fis_host[0x2C], rx_fis_host[0x2D], d2h_fis[0], d2h_fis[1], d2h_fis[2], d2h_fis[3]);
    }

    ahci->p_ci &= (uint32_t)~(1u << slot); /* this slot complete */
    /* Completion interrupt-status bit (PxIS.DHRS for D2H completions,
     * PxIS.PSS for PIO-in). A guest that polls waits on this directly;
     * one that took the interrupt-driven path (M4-6d2) enabled PxIE, so
     * also latch the port's bit in the global IS register -- its ISR
     * (Linux ahci_interrupt) reads IS first to learn which port fired.
     * The vCPU loop turns (GHC.IE && PxIS&PxIE) into a raised PIC IRQ
     * via hype_ahci_irq_pending(). */
    hype_ahci_set_pis(ahci, pis_bit); /* #512: counted edge */
    if ((ahci->p_is & ahci->p_ie) != 0) {
        ahci->is |= HYPE_AHCI_IS_PORT0;
    }
    return 0;
}

/* Shared body for the ATAPI AHCI NPF handler. dma_map (VALID-1/VALID-3)
 * translates + bounds-checks every guest-physical address this path
 * touches -- the faulting instruction fetch (when decode assists are
 * absent) and, in process_ahci_command_slot0(), every DMA structure the
 * guest programmed. A NULL map means "trusted identity-mapped guest"
 * (the M4-5/ISO-2/PCI-2 test guests: guest-physical == host, unchecked);
 * a non-NULL map is FW-1's real guest, whose OVMF/OS-supplied addresses
 * are validated against its actual guest-physical layout (RAM + flash),
 * so an out-of-range address is rejected rather than dereferenced. */
static int hype_svm_ahci_atapi_npf_common(struct hype_vcpu_ctx *real, hype_ahci_t *ahci,
                                           hype_atapi_t *atapi, uint64_t ahci_base_phys,
                                           const hype_gpa_map_t *dma_map,
                                           const uint8_t *guest_insn_bytes) {
    hype_svm_npf_t npf;
    hype_mmio_decode_t decoded;
    uint64_t *reg;
    uint32_t offset;
    const uint8_t *guest_bytes;

    hype_svm_decode_npf_info(real->vmcb->control.exitinfo1, real->vmcb->control.exitinfo2, &npf);

    if (npf.guest_phys_addr < ahci_base_phys ||
        npf.guest_phys_addr >= ahci_base_phys + HYPE_AHCI_MMIO_SIZE) {
        return -1;
    }
    offset = (uint32_t)(npf.guest_phys_addr - ahci_base_phys);

    {   /* #436: name the register a stalled guest polls. Latched MRU by offset,
         * printed every 200k accesses -- silent unless something spins. */
        static uint32_t off_key[8]; static uint64_t off_cnt[8]; static uint64_t tot;
        unsigned oi, slot = 8u;
        tot++;
        for (oi = 0; oi < 8u; oi++) {
            if (off_cnt[oi] != 0 && off_key[oi] == offset) { slot = oi; break; }
            if (off_cnt[oi] == 0 && slot == 8u) { slot = oi; off_key[oi] = offset; }
        }
        if (slot < 8u) { off_cnt[slot]++; }
        if ((tot % 200000ull) == 0ull) {
            hype_debug_print("fw-1 #436 AHCIPOLL tot=%llu: "
                             "[0]0x%x=%llu [1]0x%x=%llu [2]0x%x=%llu [3]0x%x=%llu\n",
                             (unsigned long long)tot,
                             (unsigned)off_key[0], (unsigned long long)off_cnt[0],
                             (unsigned)off_key[1], (unsigned long long)off_cnt[1],
                             (unsigned)off_key[2], (unsigned long long)off_cnt[2],
                             (unsigned)off_key[3], (unsigned long long)off_cnt[3]);
        }
    }

    /* Faulting-instruction bytes for MMIO decode. A caller that already
     * resolved them (FW-1: decode assists, else a guest page-table walk
     * of the virtual RIP -- the kernel's AHCI driver runs in its own
     * virtual address space, so RIP is NOT guest-physical) passes them
     * in. Otherwise (the identity-mapped test guests) fetch via the map,
     * where RIP == guest-physical == host. */
    if (guest_insn_bytes != 0) {
        guest_bytes = guest_insn_bytes;
    } else if (real->vmcb->control.num_bytes_fetched != 0) {
        guest_bytes = real->vmcb->control.guest_instruction_bytes;
    } else {
        guest_bytes = (const uint8_t *)(uintptr_t)guest_dma_xlate(dma_map, real->vmcb->save.rip, 1u);
        if (guest_bytes == 0) {
            return -1;
        }
    }
    if (hype_mmio_decode(guest_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (decoded.is_write != npf.is_write) {
        return -1;
    }

    /* #306: an immediate store carries its value in the instruction and has NO source
     * register -- the ModRM reg field is an opcode extension -- so the GPR lookup is
     * skipped rather than resolving register 0 and writing RAX to the device. */
    reg = decoded.has_imm ? 0 : gpr_ptr(real, decoded.reg);
    if (reg == 0 && !decoded.has_imm) {
        return -1;
    }

    if (decoded.is_write) {
        uint32_t value;
        if (decoded.mem_is_dst) {
            /* #307: a read-modify-write of this device register -- read it, combine,
             * and store the result, rather than storing the other operand alone. */
            uint32_t cur = 0;
            if (hype_ahci_mmio_read(ahci, offset, decoded.size_bytes, &cur) != 0) {
                return -1;
            }
            value = hype_mmio_rmw_value(&decoded, reg ? *reg : 0u, cur,
                                        &real->vmcb->save.rflags);
        } else {
            value = hype_mmio_store_value(&decoded, reg ? *reg : 0u);
        }
        if (HYPE_318_TRACE_MMIO && g_ahci_trace) {
            hype_debug_print("ahci-trace: ABAR write off=0x%x val=0x%x\n", (unsigned int)offset,
                              (unsigned int)value);
        }
        /* #311: does the guest's ISR ever run? A PxIS write is RW1C -- it is how a driver
         * acknowledges a completion -- so its absence means the handler was never entered,
         * whatever hype staged for injection. */
        if (offset == HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_IS) {
            /* The COUNT is reported separately from the samples, and on a rising decade, so a
             * trace cap can never again be mistaken for the guest's actual behaviour: reading
             * "12 acks" off a trace capped at 12 is what sent #311 chasing a PSS-vs-DHRS split
             * that may not exist. */
            if (real->vmcb->save.rip >= 0xffffffff80000000ull) {
            }
            static unsigned int pis_trace_n = 0;
            static unsigned int pis_total = 0;
            pis_total++;
            if (pis_trace_n < 12u) {
                pis_trace_n++;
                hype_debug_print("fw-1 PxIS-ACK#%02u val=0x%x p_is_before=0x%x\n",
                                 (unsigned int)pis_trace_n, (unsigned int)value,
                                 (unsigned int)ahci->p_is);
            }
            if (pis_total == 20u || pis_total == 50u || pis_total == 100u ||
                pis_total == 500u || pis_total == 1000u) {
                hype_debug_print("fw-1 PxIS-ACK-TOTAL=%u (still acking)\n", pis_total);
            }
        }
        if (hype_ahci_mmio_write(ahci, offset, decoded.size_bytes, value) != 0) {
            return -1;
        }
        if (offset == HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CI && ahci->p_ci != 0) {
            /* #311: how many commands the guest actually ISSUES. Paired with PxIS-ACK-TOTAL
             * this separates "the guest re-issues hundreds of commands" from "hype re-asserts
             * the line hundreds of times for one command" -- the two readings of 500+
             * acknowledgements, with completely different fixes. Totals, not a capped sample. */
            if (real->vmcb->save.rip >= 0xffffffff80000000ull) {
            }
            {
                static unsigned int ci_total = 0;
                ci_total++;
                if (ci_total == 5u || ci_total == 20u || ci_total == 50u || ci_total == 200u ||
                    ci_total == 500u) {
                    hype_debug_print("fw-1 PxCI-ISSUE-TOTAL=%u (p_ci=0x%x)\n", ci_total,
                                     (unsigned int)ahci->p_ci);
                }
            }
            /* The guest issues a command by setting that slot's PxCI bit;
             * libata cycles command slots by tag, so it is NOT always slot
             * 0 (a slot-1 command was exactly what stalled the CD-ROM scan
             * -- M4-6d2). Process every issued slot, lowest first; each
             * clears its own PxCI bit. */
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
        if (hype_ahci_mmio_read(ahci, offset, decoded.size_bytes, &value) != 0) {
            return -1;
        }
        if (HYPE_318_TRACE_MMIO && g_ahci_trace) {
            hype_debug_print("ahci-trace: ABAR read  off=0x%x val=0x%x\n", (unsigned int)offset,
                              (unsigned int)value);
        }
        hype_mmio_complete_read(&decoded, reg, value, &real->vmcb->save.rflags); /* #457 */
    }

    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

int hype_svm_vcpu_handle_ahci_npf(hype_vcpu_ctx_t *ctx, hype_ahci_t *ahci, hype_atapi_t *atapi,
                                   uint64_t ahci_base_phys) {
    /* NULL map + NULL insn: trusted identity-mapped test guest (guest-
     * physical == host, RIP == guest-physical; decode fetched via the
     * map internally). */
    return hype_svm_ahci_atapi_npf_common((struct hype_vcpu_ctx *)ctx, ahci, atapi, ahci_base_phys, 0, 0);
}

int hype_svm_vcpu_handle_ahci_npf_map(hype_vcpu_ctx_t *ctx, hype_ahci_t *ahci, hype_atapi_t *atapi,
                                       uint64_t ahci_base_phys, const hype_gpa_map_t *dma_map,
                                       const uint8_t *guest_insn_bytes) {
    return hype_svm_ahci_atapi_npf_common((struct hype_vcpu_ctx *)ctx, ahci, atapi, ahci_base_phys,
                                          dma_map, guest_insn_bytes);
}

/* Fills the D2H (Device to Host) completion FIS and clears PxCI's slot
 * 0 -- shared tail shape between the ATAPI and plain-ATA command
 * paths, byte-for-byte the same fields process_ahci_command_slot0()
 * already builds for ATAPI. */
static int complete_ahci_command_slot(hype_ahci_t *ahci, uint64_t rx_fis_phys, uint8_t status_reg,
                                      uint8_t error_reg, const hype_gpa_map_t *dma_map,
                                      unsigned slot, uint32_t pis_bit, uint32_t xfer_bytes,
                                      uint8_t *cmd_hdr_bytes) {
    /* #262 slice 3: rx_fis_phys is GUEST-physical. Identity holds for M5-2's
     * microtest (dma_map == 0) but not for the FW-1 guest, which remaps its RAM. */
    uint64_t rx_fis_host = guest_dma_xlate(dma_map, rx_fis_phys, 0x40u + 20u);
    uint8_t *d2h_fis;
    /*
     * #677: a rejected translation (guest PxFB/PxFBU pointing outside its own mapped
     * range) was being used unchecked below -- every other Received-FIS-area
     * translation in this file already refuses a 0 result (see
     * process_ahci_command_slot()'s own rx_fis_host check); this completion path,
     * shared by every plain-ATA command, did not. Found by the #602 fuzz harness.
     */
    if (rx_fis_host == 0) {
        hype_debug_print("ahci: slot %u refused -- received-FIS area gpa 0x%llx out of bounds\n",
                         slot, (unsigned long long)rx_fis_phys);
        return -1;
    }
    d2h_fis = (uint8_t *)(uintptr_t)(rx_fis_host + 0x40);

    /*
     * #262 slice 4: a PIO data-in command must also deliver a PIO Setup FIS at
     * receive-area offset 0x20. EDK2 drives the two device classes down different
     * paths -- ATAPI through AhciPacketCommandExecute, which waits on the D2H FIS
     * at 0x40, but plain-ATA PIO (IDENTIFY DEVICE) through AhciPioTransfer, which
     * waits at 0x20. Writing only the D2H FIS is enough for the CD and for Linux
     * (it polls PxCI), and is why the optical drive has always booted while the
     * disk did not: the guest firmware issued exactly one IDENTIFY, waited at 0x20
     * for a FIS that never arrived, and dropped the device.
     */
    if ((pis_bit & HYPE_AHCI_PIS_PSS) != 0) {
        hype_ahci_build_pio_setup_fis((uint8_t *)(uintptr_t)(rx_fis_host + 0x20), status_reg,
                                      error_reg, xfer_bytes);
    }

    /*
     * #358: PRDBC (Command Header dword 1, byte offset 4) -- the count of bytes actually
     * transferred, which the HBA is required to write back.
     *
     * This is what made the guest firmware refuse every guest DISK while the CD on an
     * identically-presented AHCI function worked. EDK2's AhciPioTransfer branches on the command
     * type, and only the plain-ATA branch checks it:
     *
     *   if (Read && (AtapiCommand == 0)) {
     *     AhciWaitUntilFisReceived (..., SataFisPioSetup);
     *     PrdCount = ...AhciCmdList[Slot].AhciCmdPrdbc;
     *     if (PrdCount == DataCount) Status = EFI_SUCCESS; else Status = EFI_DEVICE_ERROR;
     *   } else {
     *     AhciWaitUntilFisReceived (..., SataFisD2H);          // ATAPI: PRDBC never read
     *   }
     *
     * So IDENTIFY DEVICE read back 0 against an expected 512 and failed with
     * "PIO command failed at retry 0" -- one IDENTIFY, then the device dropped. The ATAPI path
     * has written PRDBC since #287 and takes the branch that ignores it anyway; the disk path
     * never wrote it and takes the branch that requires it.
     *
     * Linux and OpenBSD never noticed, which is why the disk works under both: they poll PxCI
     * and read the transfer length from the FIS rather than from the command header.
     */
    if (cmd_hdr_bytes != 0) {
        cmd_hdr_bytes[4] = (uint8_t)(xfer_bytes & 0xFFu);
        cmd_hdr_bytes[5] = (uint8_t)((xfer_bytes >> 8) & 0xFFu);
        cmd_hdr_bytes[6] = (uint8_t)((xfer_bytes >> 16) & 0xFFu);
        cmd_hdr_bytes[7] = (uint8_t)((xfer_bytes >> 24) & 0xFFu);
    }

    ahci->p_tfd = (uint32_t)status_reg | ((uint32_t)error_reg << 8);

    hype_ahci_build_d2h_fis(d2h_fis, 0, status_reg, error_reg);

    ahci->p_ci &= ~(1u << slot);
    /* PxIS.DHRS -- the D2H Register FIS interrupt bit a real driver
     * polls for a plain-ATA command's completion (same correction as
     * the ATAPI path; the M4-5/M5-2 cooperating test guests polled PxCI
     * and never depended on this bit). Latch the global IS port bit for
     * an interrupt-driven guest, same as the ATAPI path (M4-6d2). */
    hype_ahci_set_pis(ahci, pis_bit); /* #512: counted edge */
    if ((ahci->p_is & ahci->p_ie) != 0) {
        ahci->is |= HYPE_AHCI_IS_PORT0;
    }
    return 0;
}

/* M5-2's plain-ATA command dispatch, the H2D-FIS-command-byte-driven
 * counterpart to process_ahci_command_slot0()'s own ATAPI-only path.
 * Returns -1 for anything that isn't this handler's command (the
 * Command Header carries an ATAPI PACKET, or the H2D FIS isn't a valid
 * command FIS at all, or the command byte isn't one this project
 * models) so the caller can fall through to whichever other handler
 * actually owns it. */
/*
 * #94: move a backend-disk transfer through a guest PRDT whose entry
 * boundaries need not fall on sector boundaries. Windows' storahci builds
 * PRDs from whatever physical fragments the MDL has -- 1536-byte and
 * 512+1024-byte splits are routine -- and real AHCI hardware does not care.
 * The old per-PRD path refused any entry that split a sector (ABRT), which
 * failed every NTFS/FAT format and Setup's CreateSystemVolume with
 * "wrote 0 bytes" (measured: ATA-SHORT cmd=0xc8/0xca did=0 with prdtl=2..33).
 *
 * Full-sector spans inside one PRD go straight between guest RAM and the
 * backend; only a sector that straddles a PRD boundary is staged through a
 * 512-byte buffer. Returns 0 with *out_done = bytes moved (short if the PRDT
 * ran out -- the caller reports that via PRDBC), or -1 on a refused DMA
 * translation, or -2 on a backend I/O error.
 */
static int ahci_backend_rw_prdt(hype_ata_disk_t *disk, const hype_gpa_map_t *dma_map,
                                const uint8_t *prdt_bytes, uint16_t prdtl, uint64_t lba_base,
                                uint32_t total_bytes, int is_write, uint32_t *out_done) {
    unsigned idx = 0;
    uint32_t prd_off = 0;
    uint32_t done = 0;
    hype_ahci_prdt_entry_t prd;
    int prd_valid = 0;

    while (done < total_bytes) {
        uint32_t prd_rem;
        if (!prd_valid) {
            if (idx >= prdtl) {
                break; /* PRDT exhausted: genuine short transfer */
            }
            hype_ahci_decode_prdt_entry(prdt_bytes + (uint32_t)idx * 16u, &prd);
            prd_valid = 1;
        }
        prd_rem = prd.byte_count - prd_off;
        if (prd_rem == 0) {
            idx++;
            prd_off = 0;
            prd_valid = 0;
            continue;
        }
        if ((done % HYPE_ATA_SECTOR_SIZE) == 0u && prd_rem >= HYPE_ATA_SECTOR_SIZE) {
            /* Aligned full sectors within this PRD: one backend call. */
            uint32_t span = prd_rem;
            uint8_t *ptr;
            if (span > total_bytes - done) {
                span = total_bytes - done;
            }
            span -= span % HYPE_ATA_SECTOR_SIZE;
            ptr = (uint8_t *)(uintptr_t)guest_dma_xlate(dma_map, prd.data_phys + prd_off, span);
            if (ptr == 0) {
                return -1;
            }
            if (is_write ? hype_blk_backend_write(disk->be, lba_base + done / HYPE_ATA_SECTOR_SIZE,
                                                  span / HYPE_ATA_SECTOR_SIZE, ptr)
                         : hype_blk_backend_read(disk->be, lba_base + done / HYPE_ATA_SECTOR_SIZE,
                                                 span / HYPE_ATA_SECTOR_SIZE, ptr)) {
                return -2;
            }
            done += span;
            prd_off += span;
            continue;
        }
        {
            /* A sector that straddles PRD boundaries (or an unaligned PRD
             * tail): stage it. Reads fetch the sector first and scatter;
             * writes gather and store once the sector is complete. */
            uint8_t stage[HYPE_ATA_SECTOR_SIZE];
            uint32_t sec_off = 0;
            uint64_t lba = lba_base + done / HYPE_ATA_SECTOR_SIZE;
            if (!is_write && hype_blk_backend_read(disk->be, lba, 1u, stage)) {
                return -2;
            }
            while (sec_off < HYPE_ATA_SECTOR_SIZE) {
                uint32_t chunk;
                uint8_t *ptr;
                uint32_t i;
                if (!prd_valid) {
                    if (idx >= prdtl) {
                        *out_done = done + sec_off; /* short inside a sector */
                        return 0;
                    }
                    hype_ahci_decode_prdt_entry(prdt_bytes + (uint32_t)idx * 16u, &prd);
                    prd_valid = 1;
                }
                prd_rem = prd.byte_count - prd_off;
                if (prd_rem == 0) {
                    idx++;
                    prd_off = 0;
                    prd_valid = 0;
                    continue;
                }
                chunk = (prd_rem < HYPE_ATA_SECTOR_SIZE - sec_off) ? prd_rem
                                                                   : HYPE_ATA_SECTOR_SIZE - sec_off;
                ptr = (uint8_t *)(uintptr_t)guest_dma_xlate(dma_map, prd.data_phys + prd_off, chunk);
                if (ptr == 0) {
                    return -1;
                }
                if (is_write) {
                    for (i = 0; i < chunk; i++) {
                        stage[sec_off + i] = ptr[i];
                    }
                } else {
                    for (i = 0; i < chunk; i++) {
                        ptr[i] = stage[sec_off + i];
                    }
                }
                sec_off += chunk;
                prd_off += chunk;
            }
            if (is_write && hype_blk_backend_write(disk->be, lba, 1u, stage)) {
                return -2;
            }
            done += HYPE_ATA_SECTOR_SIZE;
        }
    }
    *out_done = done;
    return 0;
}

int process_ahci_ata_command_slot(hype_ahci_t *ahci, hype_ata_disk_t *disk,
                                  const hype_gpa_map_t *dma_map, unsigned slot) {
    uint64_t cmd_list_phys =
        ((uint64_t)ahci->p_clb | ((uint64_t)ahci->p_clbu << 32)) + (uint64_t)slot * 32u;
    uint64_t rx_fis_phys = (uint64_t)ahci->p_fb | ((uint64_t)ahci->p_fbu << 32);
    /* #262 slice 3: every address the guest hands us here is GUEST-physical, so it
     * goes through guest_dma_xlate. A NULL map means the trusted identity-mapped
     * microtest, matching the ATAPI path's convention exactly. */
    /* #358: writable -- PRDBC is written back into this header on completion, as a real HBA does. */
    uint8_t *cmd_hdr_bytes = (uint8_t *)(uintptr_t)guest_dma_xlate(dma_map, cmd_list_phys, 32u);
    hype_ahci_cmd_header_t hdr;
    const uint8_t *cmd_table_bytes;
    const uint8_t *prdt_bytes;
    hype_ahci_h2d_fis_t fis;
    uint8_t identify[HYPE_ATA_IDENTIFY_SIZE];
    const uint8_t *src = 0;
    uint8_t *dst_media = 0;
    uint32_t remaining;
    uint32_t prd_idx;
    uint64_t transferred = 0; /* #262: byte offset within this command, for backend LBAs */
    uint64_t lba_base = 0;    /* decoded per address size -- NOT fis.lba, which is the raw 48-bit field */
    uint8_t status_reg;
    uint8_t error_reg;
    /*
     * #262 slice 4: which PxIS bit signals completion depends on the command's
     * PROTOCOL, not just on success. IDENTIFY DEVICE is PIO data-in, and EDK2's
     * AhciPioTransfer waits on PxIS.PSS for it -- exactly as the ATAPI path
     * already does for IDENTIFY PACKET. Everything else here is DMA or no-data,
     * which completes with a D2H Register FIS (PxIS.DHRS).
     *
     * Signalling DHRS for IDENTIFY is invisible to Linux, which polls PxCI, but
     * the guest FIRMWARE times out waiting for PSS and drops the device: OVMF
     * issued one IDENTIFY, never read a sector, and reported "No bootable option
     * or device was found" -- with a perfectly good installed disk attached.
     */
    uint32_t pis_bit = HYPE_AHCI_PIS_DHRS;
    int is_write_direction = 0;

    /*
     * #672: the ATAPI sibling (process_ahci_command_slot) has always refused a rejected
     * translation here; this disk path never did, so a guest could point PxCLB/PxCLBU
     * (fully guest-controlled) outside its own mapped range and this function would
     * dereference the resulting NULL in hype_ahci_decode_cmd_header() below -- a
     * guest-triggerable host crash, not a guest-side fault, since hype has no process
     * boundary to contain it. Same refusal shape and message as the ATAPI path.
     */
    if (cmd_hdr_bytes == 0) {
        hype_debug_print("ahci: slot %u refused -- command list at gpa 0x%llx out of bounds\n",
                         slot, (unsigned long long)cmd_list_phys);
        return -1;
    }

    /* #372: the disk path masters the bus for exactly the same structures as the ATAPI one, so it
     * gets the same gate. 0, not -1: the caller panics on -1, and a guest that has not enabled bus
     * mastering is doing something the hardware ignores, not something undecodable. */
    if (ahci_bus_master_refused(ahci, "PxCI write (disk)")) {
        return 0;
    }

    hype_ahci_decode_cmd_header(cmd_hdr_bytes, &hdr);
    if (hdr.is_atapi) {
        return -1; /* not this handler's command -- the ATAPI path owns it */
    }

    cmd_table_bytes = (const uint8_t *)(uintptr_t)guest_dma_xlate(
        dma_map, hdr.cmd_table_phys, (uint64_t)0x80u + (uint64_t)hdr.prdtl * 16u);
    if (cmd_table_bytes == 0) {
        /* VALID-3: a rejected translation was being dereferenced immediately below -- the
         * ATAPI path has always checked this, the disk path never did. */
        hype_debug_print("ahci-disk: slot %u refused -- command table at gpa 0x%llx (prdtl=%u) out "
                         "of bounds\n",
                         slot, (unsigned long long)hdr.cmd_table_phys, (unsigned int)hdr.prdtl);
        return -1;
    }
    if (cmd_table_bytes[0] != 0x27u) {
        return -1; /* not a Register H2D FIS at all */
    }
    /* #309: the C bit distinguishes a command from a Control-register write. The disk HBA gets
     * reset the same way the optical one does -- FreeBSD attaches a channel on both -- so
     * handling this only on the path that happened to panic would just move the failure. */
    if (hype_ahci_h2d_is_control_write(cmd_table_bytes)) {
        return complete_ahci_soft_reset(ahci, rx_fis_phys, dma_map, slot, cmd_table_bytes[15]);
    }
    hype_ahci_decode_h2d_fis(cmd_table_bytes, &fis);

    /*
     * #262: trace the first commands this disk is ever asked for.
     *
     * The whole difficulty on this ticket has been not knowing WHICH half is
     * failing: "the guest firmware never issued a command to the disk" and "it
     * issued commands and rejected what came back" both present identically as
     * `BdsDxe: No bootable option or device was found.`, and they have nothing in
     * common as fixes. Three hypotheses were already spent guessing at the second
     * without evidence for it. Bounded to the first few so a live guest's steady
     * read traffic cannot flood the log.
     */
    {
        static unsigned trace_n = 0;
        if (trace_n < 12u) {
            trace_n++;
            hype_debug_print("fw-1 #262 ATACMD#%02u: cmd=0x%02x lba=0x%llx count=%u prdtl=%u\n",
                             trace_n, (unsigned)fis.command, (unsigned long long)fis.lba,
                             (unsigned)fis.count, (unsigned)hdr.prdtl);
        }
    }

    status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_DSC);
    error_reg = 0;
    remaining = 0;

    if (fis.command == HYPE_ATA_CMD_IDENTIFY_DEVICE) {
        hype_ata_disk_build_identify(disk, identify);
        src = identify;
        remaining = HYPE_ATA_IDENTIFY_SIZE;
        /*
         * #358: BOTH bits. A PIO data-in command raises PxIS.PSS when the PIO Setup FIS arrives
         * and PxIS.DHRS when the closing D2H Register FIS does; real hardware sets both, and the
         * D2H FIS is already written at receive-area offset 0x40 a few lines below regardless.
         *
         * This used to ASSIGN PSS, dropping DHRS. EDK2 waits at 0x20 for the PIO Setup FIS -- which
         * is why assigning PSS fixed the earlier symptom (#262) -- and then waits for the command to
         * COMPLETE, which is DHRS. So it got half of what it needed: it started the port, issued one
         * IDENTIFY, saw PSS, never saw DHRS, timed out, and stopped the port again. The evidence is
         * the port register pair, disk versus the CD on an identically-presented function:
         *   HBA[cd-works]:   p_is=0x00000003 (DHRS|PSS)  p_cmd=0x03000000
         *   HBA[sata-fails]: p_is=0x00000002 (PSS only)  p_cmd=0x00000000  <- port stopped again
         * Linux and OpenBSD never noticed because they poll PxCI.
         */
        pis_bit = HYPE_AHCI_PIS_DHRS | HYPE_AHCI_PIS_PSS;
    } else if (fis.command == HYPE_ATA_CMD_READ_DMA_EXT || fis.command == HYPE_ATA_CMD_READ_DMA ||
               fis.command == HYPE_ATA_CMD_WRITE_DMA_EXT || fis.command == HYPE_ATA_CMD_WRITE_DMA) {
        int lba48 = hype_ata_cmd_is_lba48(fis.command);
        uint32_t sector_count = lba48 ? hype_ata_disk_resolve_sector_count(fis.count)
                                      : hype_ata_resolve_sector_count28(fis.count);
        lba_base = lba48 ? fis.lba : hype_ata_lba28_from_fis(fis.lba, fis.device);
        is_write_direction = (fis.command == HYPE_ATA_CMD_WRITE_DMA_EXT ||
                              fis.command == HYPE_ATA_CMD_WRITE_DMA)
                                 ? 1
                                 : 0;
        if (is_write_direction) {
            /* #94: the first writes the guest ever issues, and their fate --
             * "format wrote 0 bytes" names the symptom but not which layer
             * refused. Bounded like the ATACMD trace above. */
            static unsigned wtrace_n = 0;
            if (wtrace_n < 8u) {
                wtrace_n++;
                hype_debug_print("fw-1 #94 ATAWRITE#%u: cmd=0x%02x lba=0x%llx count=%u prdtl=%u "
                                 "in_bounds=%d be_total=%llu\n",
                                 wtrace_n, (unsigned)fis.command, (unsigned long long)lba_base,
                                 (unsigned)sector_count, (unsigned)hdr.prdtl,
                                 disk->be != 0
                                     ? (lba_base + sector_count <= disk->be->total_sectors)
                                     : hype_ata_disk_range_in_bounds(disk, lba_base, sector_count),
                                 (unsigned long long)(disk->be != 0 ? disk->be->total_sectors : 0));
            }
        }
        if (disk->be != 0 ? (lba_base + sector_count <= disk->be->total_sectors)
                          : hype_ata_disk_range_in_bounds(disk, lba_base, sector_count)) {
            uint8_t *media_at = (disk->be != 0) ? 0 : disk->media + lba_base * HYPE_ATA_SECTOR_SIZE;
            if (is_write_direction) {
                dst_media = media_at;
            } else {
                src = media_at;
            }
            remaining = sector_count * HYPE_ATA_SECTOR_SIZE;
        } else {
            status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_ERR);
            error_reg = 0x10u; /* IDNF -- ID Not Found, the real ATA convention for an out-of-range LBA */
        }
    } else if (fis.command == HYPE_ATA_CMD_FLUSH_CACHE_EXT ||
               fis.command == HYPE_ATA_CMD_FLUSH_CACHE ||
               fis.command == HYPE_ATA_CMD_STANDBY_IMMEDIATE ||
               fis.command == HYPE_ATA_CMD_SET_FEATURES) {
        /*
         * Nothing to stream -- an immediate, no-data completion. SET FEATURES is
         * not optional: libata issues it to select the UDMA mode that IDENTIFY
         * advertises, and an unrecognized command here returns -1, which never
         * completes the slot and shows up in the guest as a qc timeout rather
         * than as an unsupported command.
         */
    } else {
        /*
         * An unmodelled command byte must still COMPLETE, with ABRT, the way real
         * hardware retires a command it does not support. Returning -1 here leaves
         * the slot's PxCI bit set and the MMIO write unhandled, so the guest retries
         * the same instruction forever and the whole vCPU wedges -- not just its
         * disk I/O. (Returning -1 stays correct for the ATAPI-header case above:
         * that genuinely belongs to another handler, which will clear the slot.)
         */
        {   /* #94: name the opcode being retired with ABRT -- an OS that needed
             * it sees only a failed I/O. Bounded. */
            static unsigned abrt_n = 0;
            if (abrt_n < 8u) {
                abrt_n++;
                hype_debug_print("fw-1 #94 ATA-ABRT#%u: unmodelled cmd=0x%02x count=%u prdtl=%u\n",
                                 abrt_n, (unsigned)fis.command, (unsigned)fis.count,
                                 (unsigned)hdr.prdtl);
            }
        }
        status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_ERR);
        error_reg = 0x04u; /* ABRT */
    }

    prdt_bytes = cmd_table_bytes + 0x80;
    prd_idx = 0;
    /* #94: backend R/W goes through the PRD-cursor engine above, which
     * tolerates sector-splitting PRD boundaries. The per-PRD loop below still
     * serves the synthesised transfers (IDENTIFY) and the RAM-media path. */
    if (disk->be != 0 && remaining > 0 && error_reg == 0 &&
        (is_write_direction || fis.command == HYPE_ATA_CMD_READ_DMA ||
         fis.command == HYPE_ATA_CMD_READ_DMA_EXT)) {
        uint32_t done = 0;
        uint32_t requested = remaining;
        int erc = ahci_backend_rw_prdt(disk, dma_map, prdt_bytes, hdr.prdtl, lba_base, requested,
                                       is_write_direction, &done);
        if (erc == -1) {
            return -1; /* refused DMA translation: same contract as the loop below */
        }
        if (erc == -2) {
            static unsigned befail_n = 0;
            if (befail_n < 8u) {
                befail_n++;
                hype_debug_print("fw-1 #94 ATA-BE-FAIL#%u: cmd=0x%02x lba=0x%llx done=%u\n",
                                 befail_n, (unsigned)fis.command, (unsigned long long)lba_base,
                                 (unsigned)done);
            }
            status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_ERR);
            error_reg = 0x10u;
        }
        transferred = done;
        remaining = requested - done;
        prd_idx = hdr.prdtl; /* the loop below must not re-run this transfer */
    }
    while (remaining > 0 && prd_idx < hdr.prdtl) {
        hype_ahci_prdt_entry_t prd;
        uint32_t chunk;

        hype_ahci_decode_prdt_entry(prdt_bytes + (uint32_t)prd_idx * 16u, &prd);
        chunk = (prd.byte_count < remaining) ? prd.byte_count : remaining;

        if (disk->be != 0 && fis.command != HYPE_ATA_CMD_IDENTIFY_DEVICE) {
            /*
             * #262 slice 1: storage lives behind a blk_backend, so DMA straight
             * between guest RAM and the backend instead of a RAM `media` array.
             * IDENTIFY is excluded: it is a synthesised response, not disk content.
             */
            uint64_t lba_off;
            uint32_t nsec;
            if (hype_ata_prd_sector_range(transferred, chunk, &lba_off, &nsec) != 0) {
                status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_ERR);
                error_reg = 0x04u; /* ABRT: a PRD that splits a sector is not a transfer
                                    * we model, and guessing would hide the mismatch */
                break;
            }
            if (is_write_direction) {
                if (hype_blk_backend_write(
                        disk->be, lba_base + lba_off, nsec,
                        (const void *)(uintptr_t)guest_dma_xlate(dma_map, prd.data_phys, chunk)) !=
                    0) {
                    static unsigned wfail_n = 0;
                    if (wfail_n < 8u) {
                        wfail_n++;
                        hype_debug_print("fw-1 #94 ATAWRITE-FAIL#%u: backend write lba=%llu "
                                         "nsec=%u chunk=%u\n",
                                         wfail_n, (unsigned long long)(lba_base + lba_off),
                                         (unsigned)nsec, (unsigned)chunk);
                    }
                    status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_ERR);
                    error_reg = 0x10u;
                    break;
                }
            } else {
                if (hype_blk_backend_read(
                        disk->be, lba_base + lba_off, nsec,
                        (void *)(uintptr_t)guest_dma_xlate(dma_map, prd.data_phys, chunk)) != 0) {
                    status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_ERR);
                    error_reg = 0x10u;
                    break;
                }
            }
        } else if (is_write_direction) {
            const uint8_t *guest_src =
                (const uint8_t *)(uintptr_t)guest_dma_xlate(dma_map, prd.data_phys, chunk);
            /*
             * #675: a rejected translation (guest PRD pointing outside its own mapped range)
             * was being dereferenced unchecked -- found by the #602 fuzz harness. Every other
             * guest_dma_xlate() call site in this function already refuses a 0 translation;
             * this pair of flat-media branches did not.
             *
             * An ATA error completion (DRDY|ERR), not `return -1`: this loop's own
             * backend-read/write-failure branches a few lines above treat an unreachable DMA
             * target the same way -- the command header and command table already decoded
             * fine, so this is a per-command transfer error a real controller reports on the
             * completion, not a reason to escalate to the caller's fatal "unhandled AHCI ABAR
             * MMIO" panic (that path is for #672's command-list check, where nothing about the
             * command is coherent yet).
             */
            if (guest_src == 0) {
                status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_ERR);
                error_reg = 0x10u;
                break;
            }
            ahci_copy_fast(dst_media, guest_src, chunk);
            dst_media += chunk;
        } else {
            uint8_t *guest_dst =
                (uint8_t *)(uintptr_t)guest_dma_xlate(dma_map, prd.data_phys, chunk);
            /* #675: same as the write-direction check just above, other direction. */
            if (guest_dst == 0) {
                status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_ERR);
                error_reg = 0x10u;
                break;
            }
            ahci_copy_fast(guest_dst, src, chunk);
            src += chunk;
        }
        transferred += chunk;
        remaining -= chunk;
        prd_idx++;
    }

    if (remaining > 0) {
        /* #94: the PRDT ran out before the command's byte count was satisfied --
         * a silent short transfer. Windows' format writes died exactly here. */
        static unsigned short_n = 0;
        if (short_n < 12u) {
            short_n++;
            hype_debug_print("fw-1 #94 ATA-SHORT#%u: cmd=0x%02x lba=0x%llx wanted=%u did=%u "
                             "prdtl=%u write=%d\n",
                             short_n, (unsigned)fis.command, (unsigned long long)lba_base,
                             (unsigned)(remaining + (uint32_t)transferred), (unsigned)transferred,
                             (unsigned)hdr.prdtl, is_write_direction);
        }
    }
    return complete_ahci_command_slot(ahci, rx_fis_phys, status_reg, error_reg, dma_map, slot,
                                      pis_bit, (uint32_t)transferred, cmd_hdr_bytes);
}

/*
 * #262 slice 3: shared body. `dma_map` is 0 for a trusted identity-mapped guest
 * (M5-2's microtest) and the VM's real map for the FW-1 guest, which remaps its RAM.
 * `guest_insn_bytes` lets the caller supply already-fetched instruction bytes; when
 * it is 0 the bytes are read at the guest RIP, translated through the same map. Both
 * mirror hype_svm_ahci_atapi_npf_common's contract, so the two controllers are
 * handled the same way rather than each having its own rules.
 */
/* #440: every register WRITE the guest makes to a disk-model HBA, as
 * (offset, value) pairs -- names the last thing a silent storahci did. */
volatile uint32_t g_440_ata_wr_ring[64];
volatile uint32_t g_440_ata_wr_total;

static int hype_svm_ahci_disk_npf_common(hype_vcpu_ctx_t *ctx, hype_ahci_t *ahci,
                                         hype_ata_disk_t *disk, uint64_t ahci_base_phys,
                                         const hype_gpa_map_t *dma_map,
                                         const uint8_t *guest_insn_bytes) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_npf_t npf;
    hype_mmio_decode_t decoded;
    uint64_t *reg;
    uint32_t offset;
    const uint8_t *guest_bytes;

    hype_svm_decode_npf_info(real->vmcb->control.exitinfo1, real->vmcb->control.exitinfo2, &npf);

    if (npf.guest_phys_addr < ahci_base_phys ||
        npf.guest_phys_addr >= ahci_base_phys + HYPE_AHCI_MMIO_SIZE) {
        return -1;
    }
    offset = (uint32_t)(npf.guest_phys_addr - ahci_base_phys);

    /* save.rip is a GUEST virtual/physical address. Dereferencing it as a host
     * pointer is what page-faulted hype when slice 2's routing was first wired up. */
    guest_bytes = (guest_insn_bytes != 0)
                      ? guest_insn_bytes
                      : (const uint8_t *)(uintptr_t)guest_dma_xlate(
                            dma_map, real->vmcb->save.rip, HYPE_MMIO_MAX_INSTR_BYTES);
    if (guest_bytes == 0) {
        return -1;
    }
    if (hype_mmio_decode(guest_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (decoded.is_write != npf.is_write) {
        return -1;
    }

    /* #306: an immediate store carries its value in the instruction and has NO source
     * register -- the ModRM reg field is an opcode extension -- so the GPR lookup is
     * skipped rather than resolving register 0 and writing RAX to the device. */
    reg = decoded.has_imm ? 0 : gpr_ptr(real, decoded.reg);
    if (reg == 0 && !decoded.has_imm) {
        return -1;
    }

    if (decoded.is_write) {
        uint32_t value;
        if (decoded.mem_is_dst) {
            /* #307: a read-modify-write of this device register -- read it, combine,
             * and store the result, rather than storing the other operand alone. */
            uint32_t cur = 0;
            if (hype_ahci_mmio_read(ahci, offset, decoded.size_bytes, &cur) != 0) {
                return -1;
            }
            value = hype_mmio_rmw_value(&decoded, reg ? *reg : 0u, cur,
                                        &real->vmcb->save.rflags);
        } else {
            value = hype_mmio_store_value(&decoded, reg ? *reg : 0u);
        }
        if (hype_ahci_mmio_write(ahci, offset, decoded.size_bytes, value) != 0) {
            return -1;
        }
        g_440_ata_wr_ring[g_440_ata_wr_total % 64u] = (offset << 20) | (value & 0xFFFFFu);
        g_440_ata_wr_total++;
        if (offset == HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CI && ahci->p_ci != 0) {
            /* #262 slice 3: same lesson the ATAPI path already learned in M4-6d2 --
             * libata issues by tag, so a command is NOT always in slot 0. Only its
             * internal commands (IDENTIFY, SET FEATURES) get tag 0; the first
             * block-layer read lands in another slot. Handling slot 0 alone left
             * that read sitting in PxCI forever: the guest enumerated sda, then
             * silently never read it -- no error, no timeout, just no partitions. */
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
        if (hype_ahci_mmio_read(ahci, offset, decoded.size_bytes, &value) != 0) {
            return -1;
        }
        hype_mmio_complete_read(&decoded, reg, value, &real->vmcb->save.rflags); /* #457 */
    }

    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

int hype_svm_vcpu_handle_ahci_disk_npf(hype_vcpu_ctx_t *ctx, hype_ahci_t *ahci,
                                       hype_ata_disk_t *disk, uint64_t ahci_base_phys) {
    /* Identity-mapped callers (M5-2's microtest): no map, fetch the instruction
     * bytes at the guest RIP. Behaviour is exactly as before this slice. */
    return hype_svm_ahci_disk_npf_common(ctx, ahci, disk, ahci_base_phys, 0, 0);
}

int hype_svm_vcpu_handle_ahci_disk_npf_map(hype_vcpu_ctx_t *ctx, hype_ahci_t *ahci,
                                           hype_ata_disk_t *disk, uint64_t ahci_base_phys,
                                           const hype_gpa_map_t *dma_map,
                                           const uint8_t *guest_insn_bytes) {
    return hype_svm_ahci_disk_npf_common(ctx, ahci, disk, ahci_base_phys, dma_map,
                                         guest_insn_bytes);
}

int hype_svm_vcpu_handle_debug_port_ioio(hype_vcpu_ctx_t *ctx, uint16_t base_port,
                                         const hype_gpa_map_t *dma_map, uint8_t *out_bytes,
                                         unsigned int out_cap, unsigned int *out_n) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_ioio_t io;

    if (out_bytes == 0 || out_n == 0 || out_cap == 0u) {
        return -1;
    }
    *out_n = 0;
    hype_svm_decode_ioio_info1(real->vmcb->control.exitinfo1, &io);
    if (io.port != base_port) {
        return -1;
    }

    if (io.is_in) {
        /* 0xE9 = the QEMU/bochs debug-port presence signature OVMF's
         * PlatformDebugLibIoPort checks before enabling the channel. */
        real->vmcb->save.rax = (real->vmcb->save.rax & ~0xFFULL) | 0xE9u;
        real->vmcb->save.rip = real->vmcb->control.exitinfo2;
        return 1;
    }

    /*
     * #286: OUTS/`rep outsb`, not a byte in AL.
     *
     * EDK2's PlatformDebugLibIoPort writes its DEBUG text with IoWriteFifo8(), which
     * compiles to `rep outsb` -- so the data lives in guest memory at DS:RSI, not in RAX.
     * Taking RAX's low byte gave one unrelated byte per exit and discarded the string,
     * which is why a DEBUG firmware produced 13,900 port writes and not one readable line:
     * every byte failed the printable-ASCII filter and was dropped. Same shape as the
     * fw_cfg string-IN bug SVM-STRIO (#104) fixed, in the other direction.
     */
    if (io.is_string) {
        hype_svm_string_io_plan_t plan;
        uint64_t host;
        uint64_t u;

        if (hype_svm_build_string_io_plan(&io, real->gprs[6] /* RSI */, real->gprs[1] /* RCX */,
                                          real->vmcb->save.ds.base, real->vmcb->save.rflags,
                                          &plan) != 0) {
            return -1;
        }
        if (plan.byte_count != 0u) {
            host = guest_dma_xlate(dma_map, plan.low_gpa, plan.byte_count);
            if (host == 0) {
                return -1; /* guest buffer out of range -- reject, never read host memory */
            }
            for (u = 0; u < plan.count && *out_n < out_cap; u++) {
                uint64_t addr = plan.descending
                                    ? (plan.start_gpa - u * (uint64_t)plan.unit_bytes)
                                    : (plan.start_gpa + u * (uint64_t)plan.unit_bytes);
                uint64_t off = addr - plan.low_gpa;
                uint8_t b;
                for (b = 0; b < plan.unit_bytes && *out_n < out_cap; b++) {
                    out_bytes[(*out_n)++] = ((const uint8_t *)(uintptr_t)host)[off + b];
                }
            }
        }
        /* The whole transfer is consumed whether or not it fitted the caller's buffer:
         * this is a diagnostic sink, and leaving RCX/RSI mid-string would have the guest
         * re-issue bytes hype already took. A truncated line is visible; a desynchronised
         * rep would corrupt the firmware's own state. */
        real->gprs[6] = plan.new_index_reg; /* RSI */
        real->gprs[1] = plan.new_count_reg; /* RCX */
    } else {
        out_bytes[0] = (uint8_t)(real->vmcb->save.rax & 0xFFu);
        *out_n = 1u;
    }

    real->vmcb->save.rip = real->vmcb->control.exitinfo2;
    return 0;
}

int hype_svm_vcpu_handle_uart_ioio(hype_vcpu_ctx_t *ctx, hype_guest_uart_t *uart, uint16_t base_port) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_ioio_t io;
    uint32_t offset;

    hype_svm_decode_ioio_info1(real->vmcb->control.exitinfo1, &io);

    if (io.port < base_port || io.port >= (uint32_t)base_port + HYPE_GUEST_UART_NREGS) {
        return -1;
    }
    offset = (uint32_t)io.port - base_port;

    if (io.is_in) {
        uint8_t value = hype_guest_uart_read(uart, offset);
        real->vmcb->save.rax = (real->vmcb->save.rax & ~0xFFULL) | value;
    } else {
        uint8_t wv = (uint8_t)(real->vmcb->save.rax & 0xFFu);
        /* M4-6d7 DIAG: log COM1 IER writes (offset 1, DLAB clear). An IER write
         * with THRI (bit1) / RDI (bit0) set is the 8250 driver's startup/tx
         * path -- timestamps whether userspace ever opens or writes ttyS0. */
        if (base_port == 0x3F8u && offset == 1u &&
            (uart->lcr & 0x80u) == 0u && wv != 0u) {
            static unsigned ier_log_n = 0;
            if (ier_log_n < 64u) {
                ier_log_n++;
                hype_debug_print("fw-1 UARTIER=0x%x rip=0x%llx\n", (unsigned)wv,
                                 (unsigned long long)real->vmcb->save.rip);
            }
        }
        hype_guest_uart_write(uart, offset, wv);
    }

    /* EXITINFO2 is the resume RIP, same convenience the other IOIO
     * handlers use. */
    real->vmcb->save.rip = real->vmcb->control.exitinfo2;
    return 0;
}

int hype_svm_vcpu_handle_pci_ecam_npf(hype_vcpu_ctx_t *ctx, hype_pci_t *pci, uint64_t ecam_base_phys,
                                       const uint8_t *guest_insn_bytes) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_npf_t npf;
    hype_mmio_decode_t decoded;
    hype_pci_ecam_addr_t addr;
    uint64_t *reg;

    hype_svm_decode_npf_info(real->vmcb->control.exitinfo1, real->vmcb->control.exitinfo2, &npf);

    /* Both bounds matter, not just the lower one: PCI-2 introduces a
     * second NPT-trapped region (a device's own dynamically-BAR-
     * programmed MMIO window) that could otherwise be mistaken for an
     * ECAM access if this only checked "at or past the base." */
    if (npf.guest_phys_addr < ecam_base_phys ||
        npf.guest_phys_addr >= ecam_base_phys + HYPE_PCI_ECAM_BUS0_SIZE) {
        return -1;
    }

    if (guest_insn_bytes == 0 || hype_mmio_decode(guest_insn_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (decoded.is_write != npf.is_write) {
        return -1;
    }

    /* #306: an immediate store carries its value in the instruction and has NO source
     * register -- the ModRM reg field is an opcode extension -- so the GPR lookup is
     * skipped rather than resolving register 0 and writing RAX to the device. */
    reg = decoded.has_imm ? 0 : gpr_ptr(real, decoded.reg);
    if (reg == 0 && !decoded.has_imm) {
        return -1;
    }

    hype_pci_decode_ecam_offset(npf.guest_phys_addr - ecam_base_phys, &addr);

    if (decoded.is_write) {
        uint32_t value;
        if (decoded.mem_is_dst) {
            /* #307: a read-modify-write of this device register -- read it, combine,
             * and store the result, rather than storing the other operand alone. */
            uint32_t cur = 0;
            hype_pci_config_read(pci, &addr, decoded.size_bytes, &cur);
            value = hype_mmio_rmw_value(&decoded, reg ? *reg : 0u, cur,
                                        &real->vmcb->save.rflags);
        } else {
            value = hype_mmio_store_value(&decoded, reg ? *reg : 0u);
        }
        hype_pci_config_write(pci, &addr, decoded.size_bytes, value);
    } else {
        uint32_t value = 0;
        hype_pci_config_read(pci, &addr, decoded.size_bytes, &value);
        hype_mmio_complete_read(&decoded, reg, value, &real->vmcb->save.rflags); /* #457 */
    }

    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

int hype_svm_vcpu_handle_bochs_vbe_npf(hype_vcpu_ctx_t *ctx, hype_bochs_vbe_t *dev,
                                        uint64_t mmio_base_phys, const uint8_t *insn) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_npf_t npf;
    hype_mmio_decode_t decoded;
    uint64_t *reg;
    uint32_t offset;
    const uint8_t *guest_bytes;

    hype_svm_decode_npf_info(real->vmcb->control.exitinfo1, real->vmcb->control.exitinfo2, &npf);

    if (npf.guest_phys_addr < mmio_base_phys ||
        npf.guest_phys_addr >= mmio_base_phys + HYPE_BOCHS_VBE_MMIO_SIZE) {
        return -1;
    }
    offset = (uint32_t)(npf.guest_phys_addr - mmio_base_phys);

    /*
     * #565: prefer caller-supplied instruction bytes, fetched through the guest page walk.
     *
     * The guest RIP is a guest-VIRTUAL address and is only dereferenceable as a host pointer for
     * an IDENTITY-MAPPED guest. This handler was written for the in-binary VIDEO-3 self-test,
     * which was exactly that -- so it never needed `insn`. A real VM remaps its RAM, so
     * dereferencing its RIP here reads unrelated host memory, the decode fails, and the access
     * comes back as "a register hype does not model" when the register was fine all along. NULL
     * keeps the identity fast path for any caller that still has one.
     */
    guest_bytes = (insn != 0) ? insn : (const uint8_t *)(uintptr_t)real->vmcb->save.rip;
    if (hype_mmio_decode(guest_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (decoded.is_write != npf.is_write) {
        return -1;
    }
    if (decoded.size_bytes != 2u) {
        return -1; /* DISPI registers are architecturally 16-bit only */
    }

    /* #306: an immediate store carries its value in the instruction and has NO source
     * register -- the ModRM reg field is an opcode extension -- so the GPR lookup is
     * skipped rather than resolving register 0 and writing RAX to the device. */
    reg = decoded.has_imm ? 0 : gpr_ptr(real, decoded.reg);
    if (reg == 0 && !decoded.has_imm) {
        return -1;
    }

    if (offset < HYPE_BOCHS_VBE_DISPI_OFFSET || offset >= HYPE_BOCHS_VBE_DISPI_OFFSET + HYPE_BOCHS_VBE_DISPI_SIZE) {
        /* Reserved area of the MMIO BAR -- reads as 0, writes ignored,
         * same convention devices/ahci.h's own MMIO model uses. */
        if (!decoded.is_write) {
            hype_mmio_complete_read(&decoded, reg, 0, &real->vmcb->save.rflags); /* #457 */
        }
        real->vmcb->save.rip += decoded.instr_len;
        return 0;
    }

    if (decoded.is_write) {
        uint32_t value;
        if (decoded.mem_is_dst) {
            /* #307: a read-modify-write of this device register -- read it, combine,
             * and store the result, rather than storing the other operand alone. */
            uint16_t cur16 = 0;
            if (hype_bochs_vbe_mmio_read(dev, offset - HYPE_BOCHS_VBE_DISPI_OFFSET, &cur16) != 0) {
                return -1;
            }
            value = hype_mmio_rmw_value(&decoded, reg ? *reg : 0u, cur16,
                                        &real->vmcb->save.rflags);
        } else {
            value = hype_mmio_store_value(&decoded, reg ? *reg : 0u);
        }
        if (hype_bochs_vbe_mmio_write(dev, offset - HYPE_BOCHS_VBE_DISPI_OFFSET, (uint16_t)value) != 0) {
            return -1;
        }
    } else {
        uint16_t value = 0;
        if (hype_bochs_vbe_mmio_read(dev, offset - HYPE_BOCHS_VBE_DISPI_OFFSET, &value) != 0) {
            return -1;
        }
        hype_mmio_complete_read(&decoded, reg, value, &real->vmcb->save.rflags); /* #457 */
    }

    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

/* #690: SVM MMIO handler for the Bochs VBE BAR0 (linear framebuffer VRAM) -- a plain memory
 * window, not a register set, so this reads/writes the raw byte array directly. Mirror of the
 * VMX twin; 1/2/4-byte accesses only (no generic 8-byte path in this decode helper family). */
int hype_svm_vcpu_handle_bochs_vbe_vram_npf(hype_vcpu_ctx_t *ctx, uint8_t *vram,
                                            uint64_t mmio_base_phys, const uint8_t *insn) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_npf_t npf;
    hype_mmio_decode_t decoded;
    uint64_t *reg;
    uint32_t offset;
    const uint8_t *guest_bytes;

    hype_svm_decode_npf_info(real->vmcb->control.exitinfo1, real->vmcb->control.exitinfo2, &npf);

    if (npf.guest_phys_addr < mmio_base_phys ||
        npf.guest_phys_addr >= mmio_base_phys + HYPE_BOCHS_VBE_VRAM_SIZE) {
        return -1;
    }
    offset = (uint32_t)(npf.guest_phys_addr - mmio_base_phys);

    guest_bytes = (insn != 0) ? insn : (const uint8_t *)(uintptr_t)real->vmcb->save.rip;
    if (hype_mmio_decode(guest_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (decoded.is_write != npf.is_write) {
        return -1;
    }
    if (decoded.size_bytes != 1u && decoded.size_bytes != 2u && decoded.size_bytes != 4u) {
        return -1;
    }

    reg = decoded.has_imm ? 0 : gpr_ptr(real, decoded.reg);
    if (reg == 0 && !decoded.has_imm) {
        return -1;
    }

    if (decoded.is_write) {
        uint32_t value;
        if (decoded.mem_is_dst) {
            uint32_t cur = 0;
            if (hype_bochs_vbe_vram_read(vram, HYPE_BOCHS_VBE_VRAM_SIZE, offset,
                                         decoded.size_bytes, &cur) != 0) {
                return -1;
            }
            value = hype_mmio_rmw_value(&decoded, reg ? *reg : 0u, cur, &real->vmcb->save.rflags);
        } else {
            value = hype_mmio_store_value(&decoded, reg ? *reg : 0u);
        }
        if (hype_bochs_vbe_vram_write(vram, HYPE_BOCHS_VBE_VRAM_SIZE, offset, decoded.size_bytes,
                                      value) != 0) {
            return -1;
        }
    } else {
        uint32_t value = 0;
        if (hype_bochs_vbe_vram_read(vram, HYPE_BOCHS_VBE_VRAM_SIZE, offset, decoded.size_bytes,
                                     &value) != 0) {
            return -1;
        }
        hype_mmio_complete_read(&decoded, reg, value, &real->vmcb->save.rflags); /* #457 */
    }

    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

/*
 * #591: guest-facing xHCI controller MMIO. Same decode/RIP-advance skeleton as the virtio-blk
 * handler; the model's own mmio_read/write take the width and drive the ring DMA through dma_map
 * (a doorbell write processes the command ring and posts events into guest memory).
 */
int hype_svm_vcpu_handle_xhci_npf(hype_vcpu_ctx_t *ctx, hype_xhci_dev_t *dev,
                                  const hype_gpa_map_t *dma_map, uint64_t mmio_base_phys,
                                  const uint8_t *insn) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_npf_t npf;
    hype_mmio_decode_t decoded;
    uint64_t *reg;
    uint32_t offset;
    const uint8_t *guest_bytes;

    hype_svm_decode_npf_info(real->vmcb->control.exitinfo1, real->vmcb->control.exitinfo2, &npf);

    if (npf.guest_phys_addr < mmio_base_phys ||
        npf.guest_phys_addr >= mmio_base_phys + HYPE_GXHCI_BAR_SIZE) {
        return -1;
    }
    offset = (uint32_t)(npf.guest_phys_addr - mmio_base_phys);

    guest_bytes = (insn != 0) ? insn : (const uint8_t *)(uintptr_t)real->vmcb->save.rip;
    if (hype_mmio_decode(guest_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (decoded.is_write != npf.is_write) {
        return -1;
    }
    reg = decoded.has_imm ? 0 : gpr_ptr(real, decoded.reg);
    if (reg == 0 && !decoded.has_imm) {
        return -1;
    }

    if (decoded.is_write) {
        uint64_t value;
        if (decoded.mem_is_dst) {
            uint64_t cur = 0;
            if (hype_xhci_dev_mmio_read(dev, offset, decoded.size_bytes, &cur) != 0) {
                return -1;
            }
            value = hype_mmio_rmw_value(&decoded, reg ? *reg : 0u, (uint32_t)cur,
                                        &real->vmcb->save.rflags);
        } else {
            value = hype_mmio_store_value(&decoded, reg ? *reg : 0u);
        }
        if (hype_xhci_dev_mmio_write(dev, offset, decoded.size_bytes, value, dma_map) != 0) {
            return -1;
        }
    } else {
        uint64_t value = 0;
        if (hype_xhci_dev_mmio_read(dev, offset, decoded.size_bytes, &value) != 0) {
            return -1;
        }
        hype_mmio_complete_read(&decoded, reg, value, &real->vmcb->save.rflags);
    }

    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

/* GLADDER-1: absorb an MMIO NPF to an UNMODELED guest-physical region. Real
 * hardware returns all-ones for reads of absent MMIO and drops writes; hype
 * previously PANICked on any unhandled NPF, so a fuller kernel probing a chipset
 * region hype doesn't model (e.g. ICH9 RCBA 0xFED1Cxxx) killed the whole guest
 * and we saw only the FIRST missing region. This mirrors the device handlers'
 * decode/writeback/RIP-advance, but a read yields all-ones and a write is
 * dropped. Returns 0 if it decoded + absorbed, -1 if the faulting instruction
 * couldn't be decoded (the caller must NOT blindly advance RIP in that case).
 * Callers must try every real device handler FIRST -- this is the fallback only
 * for genuinely-unmapped regions. */
int hype_svm_vcpu_absorb_mmio_npf(hype_vcpu_ctx_t *ctx, const uint8_t *guest_insn_bytes) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_mmio_decode_t decoded;

    if (guest_insn_bytes == 0 ||
        hype_mmio_decode(guest_insn_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    {
        uint32_t allones = (decoded.size_bytes >= 4u)  ? 0xFFFFFFFFu
                           : (decoded.size_bytes == 2u) ? 0xFFFFu
                                                        : 0xFFu;
        /* #306: see the note on the other handlers -- an immediate store has no source
         * register. */
        uint64_t *reg = decoded.has_imm ? 0 : gpr_ptr(real, decoded.reg);

        if (reg == 0 && !decoded.has_imm) {
            return -1;
        }
        if (!decoded.is_write) {
            /* #457: shared completion -- the immediate CMP is a READ with reg NULL, and this
             * direct merge would have dereferenced it. */
            hype_mmio_complete_read(&decoded, reg, allones, &real->vmcb->save.rflags);
        } else if (decoded.mem_is_dst) {
            /* #307: the write half is dropped like any other, but the FLAGS an RMW sets are
             * still observable to the guest's next branch, and they are computed against the
             * all-ones an absent bus returns. Leaving them stale is the silent-wrong-branch
             * failure #305 exists to avoid. */
            (void)hype_mmio_rmw_value(&decoded, reg ? *reg : 0u, allones,
                                      &real->vmcb->save.rflags);
        }
    }
    /* writes to absent MMIO are dropped */
    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

/*
 * #436: the HPET's MMIO block. Same shape as the LAPIC handler below, with one
 * difference that matters: HPET registers are 64 bits and a guest may access
 * either a whole register or one 32-bit half, so both widths are honoured
 * rather than rejected. Returns 0 when the access was the HPET's, -1 otherwise.
 */
#include "../../../devices/tpm_crb.h"

/*
 * #433: the TPM 2.0 CRB window. Same decode shape as HPET, but 1/2/4-byte accesses (a tpm_crb
 * driver does ioread32/iowrite32 on the control registers and byte/word copies into the data
 * buffer). The pure model (devices/tpm_crb.c) does everything else, including running the command
 * when the guest rings CTRL_START.
 */

/*
 * #590: walk the guest's OWN page tables to turn a guest-LINEAR address into guest-physical.
 * guest_dma_xlate does only guest-physical -> host, which is enough for an identity-mapped
 * microtest but not for a real OS whose kernel pointers are high virtual addresses. 4-level
 * long-mode walk (the only mode hype's guests run in), honouring 1 GiB and 2 MiB large pages;
 * each table page is read host-side via guest_dma_xlate. Returns the guest-physical address, or
 * ~0 on a not-present entry or an untranslatable table page.
 */
static uint64_t guest_linear_to_phys(const hype_gpa_map_t *dma_map, uint64_t cr3, uint64_t laddr) {
    uint64_t table = cr3 & 0x000FFFFFFFFFF000ull;
    int level;
    /* PML4 -> PDPT -> PD -> PT; shift 39/30/21/12 */
    static const unsigned shift[4] = {39u, 30u, 21u, 12u};
    for (level = 0; level < 4; level++) {
        uint64_t host = guest_dma_xlate(dma_map, table, 4096u);
        uint64_t e;
        unsigned idx;
        if (host == 0) {
            return ~0ull;
        }
        idx = (unsigned)((laddr >> shift[level]) & 0x1FFu);
        e = ((const uint64_t *)(uintptr_t)host)[idx];
        if ((e & 1ull) == 0ull) {
            return ~0ull; /* not present */
        }
        if (level > 0 && level < 3 && (e & (1ull << 7)) != 0ull) {
            /* large page: PS set at PDPT (1 GiB) or PD (2 MiB) */
            uint64_t mask = (level == 1) ? 0x000FFFFFC0000000ull : 0x000FFFFFFFE00000ull;
            uint64_t off_mask = (level == 1) ? 0x3FFFFFFFull : 0x1FFFFFull;
            return (e & mask) | (laddr & off_mask);
        }
        table = e & 0x000FFFFFFFFFF000ull;
    }
    return table | (laddr & 0xFFFull);
}

int hype_svm_vcpu_handle_tpm_crb_npf(hype_vcpu_ctx_t *ctx, hype_tpm_crb_t *crb,
                                     uint64_t crb_base_phys, const hype_gpa_map_t *dma_map,
                                     const uint8_t *guest_insn_bytes) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_npf_t npf;
    hype_mmio_decode_t decoded;
    uint64_t *reg;
    uint32_t offset;

    hype_svm_decode_npf_info(real->vmcb->control.exitinfo1, real->vmcb->control.exitinfo2, &npf);
    if (npf.guest_phys_addr < crb_base_phys ||
        npf.guest_phys_addr >= crb_base_phys + HYPE_TPM_CRB_SIZE) {
        return -1;
    }
    offset = (uint32_t)(npf.guest_phys_addr - crb_base_phys);
    /* #590: a `rep movs` bulk copy (the driver snapshots the control area / drains the buffer)
     * -- hype_mmio_decode handles only single movs. Emulate the string copy element by element,
     * the CRB side through the model and the RAM side through the NPT translation. */
    {
        unsigned int elem = 0, ilen = 0;
        int is_rep = 0;
        if (guest_insn_bytes != 0 &&
            hype_tpm_crb_decode_movs(guest_insn_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &elem, &ilen,
                                     &is_rep)) {
            uint64_t *rsi = gpr_ptr(real, 6u);
            uint64_t *rdi = gpr_ptr(real, 7u);
            uint64_t *rcx = gpr_ptr(real, 1u);
            uint64_t cr3 = real->vmcb->save.cr3;
            uint64_t count = is_rep ? (rcx ? *rcx : 0u) : 1u;
            int df = (real->vmcb->save.rflags & (1ull << 10)) ? 1 : 0;
            int64_t step = df ? -(int64_t)elem : (int64_t)elem;
            int to_mmio; /* 1 if RDI (dest) is the CRB window */
            uint64_t rsi_phys, rdi_phys;
            if (rsi == 0 || rdi == 0) {
                return -1;
            }
            /* The registers hold guest-LINEAR addresses -- for the MMIO side too (the kernel's
             * ioremap VA), so neither equals the physical base. Walk both to guest-physical and
             * let the one landing in the CRB window name the MMIO side. */
            rsi_phys = guest_linear_to_phys(dma_map, cr3, *rsi);
            rdi_phys = guest_linear_to_phys(dma_map, cr3, *rdi);
            if (rdi_phys >= crb_base_phys && rdi_phys < crb_base_phys + HYPE_TPM_CRB_SIZE) {
                to_mmio = 1;
            } else if (rsi_phys >= crb_base_phys && rsi_phys < crb_base_phys + HYPE_TPM_CRB_SIZE) {
                to_mmio = 0;
            } else {
                return -1; /* neither side is the CRB -- not ours */
            }
            while (count > 0u) {
                uint64_t mmio_phys = guest_linear_to_phys(dma_map, cr3, to_mmio ? *rdi : *rsi);
                uint64_t ram_phys = guest_linear_to_phys(dma_map, cr3, to_mmio ? *rsi : *rdi);
                uint32_t moff;
                uint64_t ram_host;
                unsigned k;
                if (mmio_phys == ~0ull || ram_phys == ~0ull) {
                    return -1;
                }
                if (mmio_phys < crb_base_phys ||
                    mmio_phys + elem > crb_base_phys + HYPE_TPM_CRB_SIZE) {
                    return -1;
                }
                moff = (uint32_t)(mmio_phys - crb_base_phys);
                ram_host = guest_dma_xlate(dma_map, ram_phys, elem);
                if (ram_host == 0) {
                    return -1;
                }
                if (to_mmio) {
                    uint64_t v = 0;
                    for (k = 0; k < elem; k++) v |= (uint64_t)((uint8_t *)(uintptr_t)ram_host)[k] << (8u * k);
                    hype_tpm_crb_write(crb, moff, elem, v);
                } else {
                    uint64_t v = hype_tpm_crb_read(crb, moff, elem);
                    for (k = 0; k < elem; k++) ((uint8_t *)(uintptr_t)ram_host)[k] = (uint8_t)(v >> (8u * k));
                }
                *rsi = (uint64_t)((int64_t)*rsi + step);
                *rdi = (uint64_t)((int64_t)*rdi + step);
                count--;
            }
            if (is_rep && rcx) *rcx = 0u;
            real->vmcb->save.rip += ilen;
            return 0;
        }
    }
    if (guest_insn_bytes == 0 ||
        hype_mmio_decode(guest_insn_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (decoded.is_write != npf.is_write) {
        return -1;
    }
    if (decoded.size_bytes != 1u && decoded.size_bytes != 2u && decoded.size_bytes != 4u) {
        return -1;
    }
    reg = decoded.has_imm ? 0 : gpr_ptr(real, decoded.reg);
    if (reg == 0 && !decoded.has_imm) {
        return -1;
    }
    if (decoded.is_write) {
        uint64_t value;
        if (decoded.mem_is_dst) {
            uint64_t cur = hype_tpm_crb_read(crb, offset, decoded.size_bytes);
            value = hype_mmio_rmw_value(&decoded, reg ? *reg : 0u, (uint32_t)cur,
                                        &real->vmcb->save.rflags);
        } else {
            value = decoded.has_imm ? decoded.imm_value : *reg;
        }
        hype_tpm_crb_write(crb, offset, decoded.size_bytes, value);
    } else {
        uint64_t value = hype_tpm_crb_read(crb, offset, decoded.size_bytes);
        if (reg == 0) {
            return -1;
        }
        hype_mmio_complete_read(&decoded, reg, (uint32_t)value, &real->vmcb->save.rflags);
    }
    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

int hype_svm_vcpu_handle_hpet_npf(hype_vcpu_ctx_t *ctx, hype_hpet_t *hpet,
                                   uint64_t hpet_base_phys, const uint8_t *guest_insn_bytes) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_npf_t npf;
    hype_mmio_decode_t decoded;
    uint64_t *reg;
    uint32_t offset;

    hype_svm_decode_npf_info(real->vmcb->control.exitinfo1, real->vmcb->control.exitinfo2, &npf);

    if (npf.guest_phys_addr < hpet_base_phys ||
        npf.guest_phys_addr >= hpet_base_phys + HYPE_HPET_MMIO_SIZE) {
        return -1;
    }
    offset = (uint32_t)(npf.guest_phys_addr - hpet_base_phys);

    if (guest_insn_bytes == 0 ||
        hype_mmio_decode(guest_insn_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (decoded.is_write != npf.is_write) {
        return -1;
    }
    if (decoded.size_bytes != 4u && decoded.size_bytes != 8u) {
        goto undecoded;
    }

    reg = decoded.has_imm ? 0 : gpr_ptr(real, decoded.reg);
    if (reg == 0 && !decoded.has_imm) {
        goto undecoded;
    }

    if (decoded.is_write) {
        uint64_t value;
        if (decoded.mem_is_dst) {
            uint64_t cur = hype_hpet_read(hpet, offset, decoded.size_bytes);
            value = hype_mmio_rmw_value(&decoded, reg ? *reg : 0u, (uint32_t)cur,
                                        &real->vmcb->save.rflags);
        } else {
            value = decoded.has_imm ? decoded.imm_value : *reg;
        }
        hype_hpet_write(hpet, offset, decoded.size_bytes, value);
    } else {
        uint64_t value = hype_hpet_read(hpet, offset, decoded.size_bytes);
        if (reg == 0) {
            return -1;
        }
        if (decoded.size_bytes == 8u) {
            *reg = value;
        } else {
            hype_mmio_complete_read(&decoded, reg, (uint32_t)value, &real->vmcb->save.rflags); /* #457 */
        }
    }

    real->vmcb->save.rip += decoded.instr_len;
    return 0;

undecoded:
    /*
     * #436: an HPET access this decoder cannot handle must be visible, not
     * silently handed to the unhandled-MMIO catch-all -- that absorbs the
     * access and answers all-ones, which a guest reads as a live register
     * full of set bits rather than as the register it asked for. Report the
     * form once so the gap is nameable.
     */
    {
        static unsigned undec_n = 0;
        if (undec_n < 8u) {
            undec_n++;
            hype_debug_print("fw-1 #436 HPET-UNDECODED off=0x%x w=%d size=%u imm=%d rip=0x%llx\n",
                             (unsigned)offset, npf.is_write, (unsigned)decoded.size_bytes,
                             decoded.has_imm, (unsigned long long)real->vmcb->save.rip);
        }
    }
    return -1;
}

/*
 * #457: the FW-1-grade pflash NPF handler. hype_svm_vcpu_handle_npf() above reads the faulting
 * instruction via save.rip AS A HOST POINTER, which is only true for M4-3's identity-mapped
 * microtest -- a live guest's RIP is guest-virtual under guest paging on a remapped NPT, so the
 * caller resolves the instruction bytes (decode assist or page-table walk) and passes them in,
 * exactly as the LAPIC/IO-APIC handlers already take them. Same decode surface as the LAPIC
 * handler (MOV/imm/RMW/ALU forms), but 1/2/4-byte accesses are all legal on a flash window.
 */
int hype_svm_vcpu_handle_pflash_npf_insn(hype_vcpu_ctx_t *ctx, hype_pflash_t *pf,
                                         uint64_t pf_base_phys, const uint8_t *guest_insn_bytes) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_npf_t npf;
    hype_mmio_decode_t decoded;
    uint64_t *reg;
    uint32_t offset;

    hype_svm_decode_npf_info(real->vmcb->control.exitinfo1, real->vmcb->control.exitinfo2, &npf);

    if (npf.guest_phys_addr < pf_base_phys || npf.guest_phys_addr >= pf_base_phys + pf->size) {
        return -1;
    }
    offset = (uint32_t)(npf.guest_phys_addr - pf_base_phys);

    if (guest_insn_bytes == 0 ||
        hype_mmio_decode(guest_insn_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (decoded.is_write != npf.is_write) {
        return -1;
    }

    reg = decoded.has_imm ? 0 : gpr_ptr(real, decoded.reg);
    if (reg == 0 && !decoded.has_imm) {
        return -1;
    }

    if (decoded.is_write) {
        uint32_t value;
        if (decoded.mem_is_dst) {
            uint32_t cur = 0;
            if (hype_pflash_read(pf, offset, decoded.size_bytes, &cur) != 0) {
                return -1;
            }
            value = hype_mmio_rmw_value(&decoded, reg ? *reg : 0u, cur,
                                        &real->vmcb->save.rflags);
        } else {
            value = hype_mmio_store_value(&decoded, reg ? *reg : 0u);
        }
        if (hype_pflash_write(pf, offset, decoded.size_bytes, value) != 0) {
            return -1;
        }
    } else {
        uint32_t value = 0;
        if (hype_pflash_read(pf, offset, decoded.size_bytes, &value) != 0) {
            return -1;
        }
        /* #457: shared read-completion -- handles MOV, the register ALU forms, and the
         * immediate CMP where reg is NULL. */
        hype_mmio_complete_read(&decoded, reg, value, &real->vmcb->save.rflags);
    }

    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

/*
 * #457: arm a flush-this-guest for the NEXT entry, after a runtime NPT edit. The run path
 * already clears TLB_CONTROL after every VMRUN (#244), so this is consumed exactly once.
 */
void hype_svm_vcpu_request_tlb_flush(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    if (real == 0 || real->vmcb == 0) {
        return;
    }
    real->vmcb->control.guest_asid_tlb_ctl =
        (real->vmcb->control.guest_asid_tlb_ctl & 0xFFFFFFFFull) |
        ((uint64_t)HYPE_SVM_TLB_CTL_FLUSH_GUEST << 32);
}

int hype_svm_vcpu_handle_lapic_npf(hype_vcpu_ctx_t *ctx, hype_guest_lapic_t *lapic,
                                    uint64_t lapic_base_phys, const uint8_t *guest_insn_bytes) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_npf_t npf;
    hype_mmio_decode_t decoded;
    uint64_t *reg;
    uint32_t offset;

    hype_svm_decode_npf_info(real->vmcb->control.exitinfo1, real->vmcb->control.exitinfo2, &npf);

    if (npf.guest_phys_addr < lapic_base_phys ||
        npf.guest_phys_addr >= lapic_base_phys + HYPE_GUEST_LAPIC_MMIO_SIZE) {
        return -1;
    }
    offset = (uint32_t)(npf.guest_phys_addr - lapic_base_phys);

    if (guest_insn_bytes == 0 || hype_mmio_decode(guest_insn_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (decoded.is_write != npf.is_write) {
        return -1;
    }
    if (decoded.size_bytes != 4u) {
        return -1; /* xAPIC registers are 32-bit dword accesses only */
    }

    /* #306: an immediate store has no source register -- see the IO-APIC handler. */
    reg = decoded.has_imm ? 0 : gpr_ptr(real, decoded.reg);
    if (reg == 0 && !decoded.has_imm) {
        return -1;
    }

    if (decoded.is_write) {
        uint32_t value;
        if (decoded.mem_is_dst) {
            /* #307: a read-modify-write of this device register -- read it, combine,
             * and store the result, rather than storing the other operand alone. */
            uint32_t cur = 0;
            if (hype_guest_lapic_read(lapic, offset, decoded.size_bytes, &cur) != 0) {
                return -1;
            }
            value = hype_mmio_rmw_value(&decoded, reg ? *reg : 0u, cur,
                                        &real->vmcb->save.rflags);
        } else {
            value = hype_mmio_store_value(&decoded, reg ? *reg : 0u);
        }
        if (hype_guest_lapic_write(lapic, offset, decoded.size_bytes, value) != 0) {
            return -1;
        }
    } else {
        uint32_t value = 0;
        if (hype_guest_lapic_read(lapic, offset, decoded.size_bytes, &value) != 0) {
            return -1;
        }
        /*
         * #305: ALU forms with the device register as memory source (FreeBSD reads the
         * Spurious Interrupt Vector as `and edx, [rcx+0xf0]`); #457: plus the immediate
         * CMP, where reg is NULL. One shared completion -- hype_mmio_complete_read --
         * because every hand-rolled copy of this tail was a NULL *reg dereference waiting
         * for the imm form.
         */
        hype_mmio_complete_read(&decoded, reg, value, &real->vmcb->save.rflags);
    }

    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

/* M4-6b3: route a guest MMIO access to the emulated I/O APIC (0xFEC00000).
 * Same thin-shim shape as hype_svm_vcpu_handle_lapic_npf: decode the faulting
 * instruction, dispatch a 32-bit read/write to the pure hype_ioapic_* model,
 * advance RIP. Returns 0 if handled, -1 to let the caller try the next region
 * / absorb. */
int hype_svm_vcpu_handle_ioapic_npf(hype_vcpu_ctx_t *ctx, hype_ioapic_t *ioapic,
                                    uint64_t ioapic_base_phys, const uint8_t *guest_insn_bytes) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_npf_t npf;
    hype_mmio_decode_t decoded;
    uint64_t *reg;
    uint32_t offset;

    hype_svm_decode_npf_info(real->vmcb->control.exitinfo1, real->vmcb->control.exitinfo2, &npf);

    if (npf.guest_phys_addr < ioapic_base_phys ||
        npf.guest_phys_addr >= ioapic_base_phys + HYPE_IOAPIC_MMIO_SIZE) {
        return -1;
    }
    offset = (uint32_t)(npf.guest_phys_addr - ioapic_base_phys);

    if (guest_insn_bytes == 0 ||
        hype_mmio_decode(guest_insn_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (decoded.is_write != npf.is_write) {
        return -1;
    }
    if (decoded.size_bytes != 4u) {
        return -1; /* IOREGSEL/IOWIN are 32-bit accesses only */
    }

    /*
     * #306: an immediate store has NO source register -- the ModRM reg field is an opcode
     * extension -- so the GPR lookup is skipped rather than resolving register 0 and
     * writing RAX to the device. FreeBSD selects IO-APIC registers exactly this way:
     * `mov dword [rbx], 1`.
     */
    reg = decoded.has_imm ? 0 : gpr_ptr(real, decoded.reg);
    if (reg == 0 && !decoded.has_imm) {
        return -1;
    }

    if (decoded.is_write) {
        uint32_t value;
        if (decoded.mem_is_dst) {
            /* #307: a read-modify-write of this device register -- read it, combine,
             * and store the result, rather than storing the other operand alone. */
            uint32_t cur = 0;
            if (hype_ioapic_mmio_read(ioapic, offset, &cur) != 0) {
                return -1;
            }
            value = hype_mmio_rmw_value(&decoded, reg ? *reg : 0u, cur,
                                        &real->vmcb->save.rflags);
        } else {
            value = hype_mmio_store_value(&decoded, reg ? *reg : 0u);
        }
        /* M4-6d7 DIAG: RTE-write timeline. Log every redirection-entry write
         * for the ISA GSIs of interest (1=kbd, 3=COM2, 4=COM1) plus the first
         * 24 writes overall, with the resulting full RTE -- proves whether the
         * guest ever unmasks the serial IRQ and what it programs. */
        if (offset == HYPE_IOAPIC_REG_IOWIN && ioapic->ioregsel >= HYPE_IOAPIC_INDEX_REDIR_BASE) {
            uint32_t rel = (ioapic->ioregsel & 0xFFu) - HYPE_IOAPIC_INDEX_REDIR_BASE;
            uint32_t gsi = rel / 2u;
            /*
             * Bounded PER GSI rather than globally. A single global cap let one chatty line
             * consume the whole budget and hide the others; exempting the device GSIs from the
             * cap entirely (as #311 briefly did, to see whether the guest ever unmasked them)
             * is worse -- a guest that masks a level source for the duration of each handler,
             * as FreeBSD's intr_execute_handlers does, rewrites that entry on EVERY interrupt.
             * Uncapped that produced 500+ lines and 95% of a boot log, drowning the guest
             * console this trace exists to be read beside. Four per GSI answers "did it ever
             * get programmed, and unmasked" without being able to flood.
             */
            static unsigned rte_log_n[24] = {0};
            if (gsi < 24u && rte_log_n[gsi] < 4u) {
                rte_log_n[gsi]++;
                hype_debug_print("fw-1 RTEWR gsi=%u %s=0x%x rip=0x%llx\n", gsi,
                                 (rel & 1u) ? "hi" : "lo", value,
                                 (unsigned long long)real->vmcb->save.rip);
            }
        }
        if (hype_ioapic_mmio_write(ioapic, offset, value) != 0) {
            return -1;
        }
    } else {
        uint32_t value = 0;
        if (hype_ioapic_mmio_read(ioapic, offset, &value) != 0) {
            return -1;
        }
        hype_mmio_complete_read(&decoded, reg, value, &real->vmcb->save.rflags); /* #457 */
    }

    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

/*
 * #268: say something when a request is refused. Before this, a rejected chain
 * failed silently from the operator's point of view, which is how a spec-legal
 * guest request came to present as a mysterious stall with nothing in the log.
 *
 * Rate-limited to the first few, and deliberately so: this sits on the I/O path,
 * and a guest that produces one bad chain usually produces thousands. An
 * unbounded print would bury the rest of the log -- the exact failure mode #238
 * was about -- and the first occurrence is the informative one anyway. The
 * counter is unsynchronised across VMs for the same reason the write stats are:
 * a lost increment on a diagnostic beats a lock on the I/O path.
 */
#define HYPE_VIRTIO_BLK_REJECT_LOG_MAX 8u

static uint32_t g_virtio_blk_rejects;
static void (*g_virtio_blk_reject_sink)(const char *why);

void hype_virtio_blk_set_reject_sink(void (*sink)(const char *why)) {
    g_virtio_blk_reject_sink = sink;
    g_virtio_blk_rejects = 0;
}

/* Variant that names the offending value. A reject reason without the number is
 * half a diagnostic: "unsupported request type" sent the reader back to the spec
 * to guess which, on the first real-hardware run this logging ever did. */
static void virtio_blk_reject_val(const char *why, uint32_t value) {
    g_virtio_blk_rejects++;
    if (g_virtio_blk_rejects > HYPE_VIRTIO_BLK_REJECT_LOG_MAX) {
        return;
    }
    if (g_virtio_blk_reject_sink != 0) {
        g_virtio_blk_reject_sink(why);
        return;
    }
    hype_debug_print("virtio-blk: request REJECTED (#%u): %s (0x%x)\n",
                     (unsigned)g_virtio_blk_rejects, why, (unsigned)value);
    if (g_virtio_blk_rejects == HYPE_VIRTIO_BLK_REJECT_LOG_MAX) {
        hype_debug_print("virtio-blk: further rejections will not be logged\n");
    }
}

static void virtio_blk_reject(const char *why) {
    g_virtio_blk_rejects++;
    if (g_virtio_blk_rejects > HYPE_VIRTIO_BLK_REJECT_LOG_MAX) {
        return;
    }
    if (g_virtio_blk_reject_sink != 0) {
        g_virtio_blk_reject_sink(why);
        return;
    }
    hype_debug_print("virtio-blk: request REJECTED (#%u): %s\n", (unsigned)g_virtio_blk_rejects,
                     why);
    if (g_virtio_blk_rejects == HYPE_VIRTIO_BLK_REJECT_LOG_MAX) {
        hype_debug_print("virtio-blk: further rejections will not be logged\n");
    }
}

/*
 * Fetch descriptor `index` from this device's descriptor table, bounds-checking
 * the index against queue_size and translating the 16-byte entry through the
 * VALID-3 gpa map. Returns -1 if either check fails.
 */
static int virtq_fetch_desc(const hype_virtio_blk_t *dev, const hype_gpa_map_t *dma_map,
                            uint16_t index, hype_virtq_desc_t *out) {
    const uint8_t *dp;

    if (index >= dev->queue_size) {
        return -1;
    }
    dp = (const uint8_t *)(uintptr_t)guest_dma_xlate(
        dma_map, dev->queue_desc + (uint64_t)index * 16u, 16u);
    if (dp == 0) {
        return -1;
    }
    hype_virtq_decode_desc(dp, out);
    return 0;
}

/*
 * #268: validate a whole request chain and locate its status descriptor WITHOUT
 * performing any I/O, then let the caller re-walk it to transfer data.
 *
 * The two passes are the point. Validating as we transferred would leave a
 * half-applied write behind whenever a chain turned out to be malformed
 * partway through -- so a malformed chain is rejected having changed nothing,
 * which is the property the old fixed-3 walk had for free and which the
 * variable-length walk has to earn.
 *
 * This covers the chain's SHAPE, which is the part that is guest-controlled
 * bookkeeping rather than data. It is deliberately not a promise of atomicity
 * for the transfer itself: a segment whose length is unusable, or a backend that
 * errors on the third of four segments, still completes the request with IOERR
 * after earlier segments have already landed. That matches a real disk, where an
 * error partway through a transfer does not un-write what preceded it -- IOERR
 * means "distrust this whole request", not "nothing happened".
 *
 * Chain shape per the virtio spec: header -> zero or more data segments ->
 * status. Every descriptor except the last carries NEXT, so the status
 * descriptor is exactly the one without it, and the data segments are exactly
 * the descriptors between. ZERO data segments is legal, not an error: a FLUSH
 * request carries no data at all, so its chain is just header -> status.
 */
static int virtq_validate_chain(const hype_virtio_blk_t *dev, const hype_gpa_map_t *dma_map,
                                uint16_t head, hype_virtq_desc_t *out_header,
                                hype_virtq_desc_t *out_status) {
    hype_virtq_desc_t d;
    uint32_t steps = 0;
    uint16_t cur;

    if (virtq_fetch_desc(dev, dma_map, head, out_header) != 0) {
        return -1;
    }
    /* A chain with no NEXT on its header has nowhere to put the status byte. */
    if ((out_header->flags & HYPE_VIRTQ_DESC_F_NEXT) == 0) {
        return -1;
    }
    cur = out_header->next;
    for (;;) {
        if (virtq_fetch_desc(dev, dma_map, cur, &d) != 0) {
            return -1;
        }
        /*
         * A legal chain visits each descriptor at most once, so more than
         * queue_size hops proves the guest built a cycle (A -> B -> A). The
         * bound is mandatory rather than defensive: the descriptor indices are
         * guest-controlled, and an unbounded follow-the-NEXT loop would spin
         * this core inside hype forever, taking that VM's dispatch loop with
         * it. The old fixed-3 walk could not loop at all, so the bound has to
         * arrive together with the loop that needs it.
         */
        steps++;
        if (steps > (uint32_t)dev->queue_size) {
            return -1;
        }
        if ((d.flags & HYPE_VIRTQ_DESC_F_NEXT) == 0) {
            *out_status = d;
            return 0;
        }
        cur = d.next;
    }
}

/*
 * Walks every newly-submitted chain in the (single) virtqueue since this
 * device's own last_avail_idx bookkeeping, processing each as a virtio_blk_req:
 * a header descriptor, any number of data segments, and a status descriptor.
 * Returns -1 if a chain is malformed (bad index, untranslatable address, cyclic
 * NEXT list, or no status descriptor); 0 otherwise.
 *
 * #268: this used to require EXACTLY three descriptors and reject anything
 * else. Two consequences, both fixed here:
 *   - A header + N-data + status chain was refused outright, capping every
 *     request at one contiguous segment. Linux produces multi-segment chains
 *     whenever a request spans non-contiguous physical pages, which is the
 *     normal case above a page once memory is fragmented -- and since hype
 *     advertises no VIRTIO_BLK_F_SEG_MAX, a conforming driver has no way to
 *     learn of a limit and is entitled to send them.
 *   - A 2-descriptor FLUSH chain (header -> status, no data) hit the same
 *     rejection. That path did not merely refuse the request: returning -1
 *     aborts the notify WITHOUT advancing last_avail_idx or writing a status
 *     byte, so the request is never completed and the guest waits on it
 *     forever. The FLUSH branch below was therefore unreachable in practice.
 *
 * No segment-count limit is imposed, so there is nothing to advertise via
 * VIRTIO_BLK_F_SEG_MAX: the walk streams one segment at a time and needs no
 * array to hold them.
 */
int process_virtio_blk_queue(hype_virtio_blk_t *dev, const hype_blk_backend_t *be,
                             const hype_gpa_map_t *dma_map) {
    /* VALID-3: the virtqueue base addresses (desc/avail/used), every descriptor
     * index, and every buffer pointer are guest-supplied. Each guest-physical
     * address is translated through this VM's bounds-checked gpa map
     * (guest_dma_xlate -> 0 when out of range) BEFORE it is dereferenced, so a
     * malicious/garbled queue can never steer a read or write at hype's own or
     * another VM's memory. For an identity-mapped caller (M5-1) dma_map is NULL
     * and the translation returns the address unchanged. Descriptor indices are
     * additionally bounded by queue_size. */
    uint16_t qsz = dev->queue_size;
    const uint8_t *avail_base;
    uint8_t *used_base;
    uint16_t avail_idx;
    uint32_t drained = 0; /* #265: chains this kick found already pending */

    if (qsz == 0u) {
        return -1;
    }
    /*
     * #372: a device that cannot master the bus cannot reach the virtqueue either.
     *
     * Same gate as the AHCI paths, same return convention: 0, because the guest asked for
     * something the hardware would silently not do. Nothing is consumed from the avail ring and
     * nothing is placed in the used ring, so the driver waits forever -- which is the point.
     */
    if (dev->bus_master == 0) {
        static int reported;
        if (!reported) {
            reported = 1;
            hype_debug_print("virtio-blk: queue notify IGNORED -- the guest has not set PCI Bus "
                             "Master Enable (Command bit 2), so the device cannot reach the "
                             "virtqueue. No request will ever complete, exactly as on real "
                             "hardware. [#372]\n");
        }
        return 0;
    }
    /* avail ring: flags(2) + idx(2) + ring(2*qsz) + used_event(2). */
    avail_base = (const uint8_t *)(uintptr_t)guest_dma_xlate(dma_map, dev->queue_driver,
                                                             4u + 2u * (uint64_t)qsz + 2u);
    /* used ring: flags(2) + idx(2) + elems(8*qsz) + avail_event(2). */
    used_base = (uint8_t *)(uintptr_t)guest_dma_xlate(dma_map, dev->queue_device,
                                                      4u + 8u * (uint64_t)qsz + 2u);
    if (avail_base == 0 || used_base == 0) {
        return -1;
    }
    avail_idx = (uint16_t)(avail_base[2] | (avail_base[3] << 8));

    while (dev->last_avail_idx != avail_idx) {
        uint16_t ring_index = (uint16_t)(dev->last_avail_idx % qsz);
        uint16_t head_desc =
            (uint16_t)(avail_base[4 + 2 * ring_index] | (avail_base[4 + 2 * ring_index + 1] << 8));
        hype_virtq_desc_t header_desc, status_desc;
        const uint8_t *hdr;
        uint32_t req_type;
        uint64_t sector;
        uint8_t status_value;
        uint32_t used_len;
        uint16_t used_idx;
        uint16_t used_ring_index;
        uint32_t elem_off;

        /* Validate the whole chain first (bounds, translatability, no cycle) so a
         * malformed request is rejected before any data moves. */
        if (virtq_validate_chain(dev, dma_map, head_desc, &header_desc, &status_desc) != 0) {
            virtio_blk_reject("malformed descriptor chain");
            return -1;
        }

        /* virtio_blk_req header: type(4) + reserved(4) + sector(8) = 16 bytes. */
        hdr = (const uint8_t *)(uintptr_t)guest_dma_xlate(dma_map, header_desc.addr, 16u);
        if (hdr == 0) {
            return -1;
        }
        req_type = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) |
                   ((uint32_t)hdr[3] << 24);
        sector = (uint64_t)hdr[8] | ((uint64_t)hdr[9] << 8) | ((uint64_t)hdr[10] << 16) |
                 ((uint64_t)hdr[11] << 24) | ((uint64_t)hdr[12] << 32) | ((uint64_t)hdr[13] << 40) |
                 ((uint64_t)hdr[14] << 48) | ((uint64_t)hdr[15] << 56);

        /* GLADDER/M5-7a: dispatch through the bounds-gated hype_blk_backend vtable
         * (#89) instead of a raw host buffer, so the frontend is backend-agnostic
         * (file / physical / qcow2). virtio-blk data is always a whole-sector
         * multiple; the LBA+count bounds check lives inside hype_blk_backend_*.
         * The guest data buffer is itself translated+bounded before the copy. */
        if (req_type == HYPE_VIRTIO_BLK_T_OUT || req_type == HYPE_VIRTIO_BLK_T_IN) {
            /*
             * #268: transfer every data segment in the chain, not just the first.
             * The LBA advances by each segment's sector count, so a scattered
             * buffer lands as one contiguous run on the backend -- which is
             * exactly what the guest asked for and what a real device does.
             */
            uint16_t cur = header_desc.next;
            uint64_t seg_lba = sector;
            uint64_t xfer_bytes = 0;
            uint32_t nsegs = 0;
            const char *err = 0;
            /*
             * #295: a WRITE chain's segments are gathered and issued as ONE vectored backend call
             * per batch instead of one call per segment. Within a request the segments are
             * contiguous on disk BY CONSTRUCTION (one virtio_blk_req has one start sector and its
             * data runs from there), so the batch needs no adjacency decision -- only a size cap.
             * The cap matches the seg_max hype advertises: a driver that honours it always fits
             * one batch; one that never negotiated SEG_MAX may exceed it, and then each full batch
             * flushes as its own (still contiguous) vectored call.
             *
             * Reads stay per-segment: the measured cost was the write path's one-command-per-4KiB
             * round trip (#265/#295), and the read path's throughput has never been the complaint.
             */
            hype_blk_seg_t wsegs[HYPE_VIRTIO_BLK_SEG_MAX];
            uint32_t nw = 0;
            uint64_t wbatch_lba = sector;

            for (;;) {
                hype_virtq_desc_t seg;
                uint32_t nsec;
                void *gbuf;

                /* Cannot fail: virtq_validate_chain() already walked this exact
                 * list. Checked anyway rather than assuming, since a failure here
                 * would otherwise be a dereference of an untranslated address. */
                if (virtq_fetch_desc(dev, dma_map, cur, &seg) != 0) {
                    err = "descriptor vanished mid-chain";
                    break;
                }
                if ((seg.flags & HYPE_VIRTQ_DESC_F_NEXT) == 0) {
                    break; /* this is the status descriptor -- chain done */
                }
                nsec = seg.len / HYPE_VIRTIO_BLK_SECTOR_SIZE;
                /*
                 * Each segment must itself be a whole number of sectors. The spec
                 * only constrains the total, but the backend is addressed in
                 * sectors, so a segment that splits one would need a bounce
                 * buffer this freestanding build has nowhere to allocate. Such a
                 * request is COMPLETED with IOERR rather than left dangling: the
                 * guest gets an error it can report, instead of an I/O that never
                 * returns. Linux's block layer aligns every segment to the
                 * logical block size, so this is not a case it can produce.
                 */
                if ((seg.len % HYPE_VIRTIO_BLK_SECTOR_SIZE) != 0u || nsec == 0u) {
                    err = "data segment is not a whole number of sectors";
                    break;
                }
                gbuf = (void *)(uintptr_t)guest_dma_xlate(dma_map, seg.addr, seg.len);
                if (gbuf == 0) {
                    err = "data segment failed bounds check";
                    break;
                }
                if (req_type == HYPE_VIRTIO_BLK_T_OUT) {
                    if (nw == HYPE_VIRTIO_BLK_SEG_MAX) {
                        if (hype_blk_backend_writev(be, wbatch_lba, wsegs, nw) != 0) {
                            err = "backend rejected the transfer";
                            break;
                        }
                        wbatch_lba = seg_lba;
                        nw = 0;
                    }
                    wsegs[nw].buf = gbuf;
                    wsegs[nw].count = nsec;
                    nw++;
                } else if (hype_blk_backend_read(be, seg_lba, nsec, gbuf) != 0) {
                    err = "backend rejected the transfer";
                    break;
                }
                seg_lba += nsec;
                xfer_bytes += seg.len;
                nsegs++;
                cur = seg.next;
            }
            if (err == 0 && nw != 0u &&
                hype_blk_backend_writev(be, wbatch_lba, wsegs, nw) != 0) {
                err = "backend rejected the transfer";
            }

            if (err == 0 && nsegs == 0u) {
                /* A read/write with no data buffer at all. Not a chain-shape
                 * error (the shape is legal, it is what FLUSH uses) -- it is a
                 * meaningless request, so complete it with IOERR. */
                err = "read/write request carries no data segment";
            }
            if (err != 0) {
                virtio_blk_reject(err);
                status_value = HYPE_VIRTIO_BLK_S_IOERR;
                used_len = 1;
            } else {
                status_value = HYPE_VIRTIO_BLK_S_OK;
                /* used_len counts what the DEVICE wrote into guest memory: the
                 * data for a read, and the status byte in both directions. */
                used_len = (req_type == HYPE_VIRTIO_BLK_T_IN) ? (uint32_t)(xfer_bytes + 1u) : 1u;
            }
        } else if (req_type == HYPE_VIRTIO_BLK_T_GET_ID) {
            /*
             * #310: hand back the device's serial string. FreeBSD's vtblk issues this during
             * attach and reports "error getting device identifier: 45" when it is refused.
             *
             * Deliberately NOT folded into the T_IN path above: that path requires every
             * segment to be a whole-sector multiple, and this one carries a single 20-byte
             * device-writable segment, so reusing it would IOERR the request instead.
             */
            hype_virtq_desc_t seg;

            if (virtq_fetch_desc(dev, dma_map, header_desc.next, &seg) != 0) {
                virtio_blk_reject("GET_ID: data descriptor vanished mid-chain");
                status_value = HYPE_VIRTIO_BLK_S_IOERR;
                used_len = 1;
            } else if ((seg.flags & HYPE_VIRTQ_DESC_F_NEXT) == 0) {
                /* No data descriptor at all -- the next link is already the status byte. */
                virtio_blk_reject("GET_ID: chain carries no data descriptor");
                status_value = HYPE_VIRTIO_BLK_S_IOERR;
                used_len = 1;
            } else {
                /*
                 * Write at most what the guest offered AND at most the field width, then
                 * translate for exactly that many bytes. A short buffer is the guest's
                 * business; overrunning it would be hype's.
                 */
                uint32_t n = (seg.len < HYPE_VIRTIO_BLK_ID_BYTES) ? seg.len
                                                                  : HYPE_VIRTIO_BLK_ID_BYTES;
                uint8_t *gbuf = (uint8_t *)(uintptr_t)guest_dma_xlate(dma_map, seg.addr, n);
                if (gbuf == 0) {
                    virtio_blk_reject("GET_ID: data segment failed bounds check");
                    status_value = HYPE_VIRTIO_BLK_S_IOERR;
                    used_len = 1;
                } else {
                    uint32_t i;
                    for (i = 0; i < n; i++) {
                        gbuf[i] = dev->serial[i];
                    }
                    status_value = HYPE_VIRTIO_BLK_S_OK;
                    /* used_len counts what the device wrote, plus the status byte. */
                    used_len = n + 1u;
                }
            }
        } else if (req_type == HYPE_VIRTIO_BLK_T_FLUSH) {
            /* Synchronous backend: writes already durable, so FLUSH is a no-op ACK
             * (a real guest issues FLUSH; returning UNSUPP would stall its I/O).
             * #268: this branch is only now REACHABLE. A FLUSH chain carries no
             * data descriptor, so the old exactly-3-descriptor requirement failed
             * it before this switch was ever consulted -- and failed it by
             * aborting the notify, which never completed the request at all. */
            status_value = HYPE_VIRTIO_BLK_S_OK;
            used_len = 1;
        } else {
            virtio_blk_reject_val("unsupported request type", req_type);
            status_value = HYPE_VIRTIO_BLK_S_UNSUPP;
            used_len = 1;
        }

        {
            uint8_t *st = (uint8_t *)(uintptr_t)guest_dma_xlate(dma_map, status_desc.addr, 1u);
            if (st != 0) {
                *st = status_value;
            }
        }

        used_idx = (uint16_t)(used_base[2] | (used_base[3] << 8));
        used_ring_index = (uint16_t)(used_idx % dev->queue_size);
        elem_off = 4u + 8u * used_ring_index;
        used_base[elem_off + 0] = (uint8_t)(head_desc & 0xFFu);
        used_base[elem_off + 1] = (uint8_t)((head_desc >> 8) & 0xFFu);
        used_base[elem_off + 2] = 0;
        used_base[elem_off + 3] = 0;
        used_base[elem_off + 4] = (uint8_t)(used_len & 0xFFu);
        used_base[elem_off + 5] = (uint8_t)((used_len >> 8) & 0xFFu);
        used_base[elem_off + 6] = (uint8_t)((used_len >> 16) & 0xFFu);
        used_base[elem_off + 7] = (uint8_t)((used_len >> 24) & 0xFFu);
        used_idx = (uint16_t)(used_idx + 1u);
        used_base[2] = (uint8_t)(used_idx & 0xFFu);
        used_base[3] = (uint8_t)((used_idx >> 8) & 0xFFu);

        dev->isr_status |= 0x01u;
        dev->last_avail_idx = (uint16_t)(dev->last_avail_idx + 1u);
        drained++;
    }

    /* #265: record the queue depth this kick saw. Counted here rather than from
     * avail_idx arithmetic so a kick that found nothing new contributes nothing
     * -- an empty notify says nothing about how deeply the guest queues. */
    hype_virtio_blk_depth_record(hype_virtio_blk_depth(), drained);
    return 0;
}

int hype_svm_vcpu_handle_virtio_blk_npf(hype_vcpu_ctx_t *ctx, hype_virtio_blk_t *dev,
                                         const hype_blk_backend_t *be, const hype_gpa_map_t *dma_map,
                                         uint64_t mmio_base_phys, const uint8_t *insn) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_npf_t npf;
    hype_mmio_decode_t decoded;
    uint64_t *reg;
    uint32_t offset;
    const uint8_t *guest_bytes;

    hype_svm_decode_npf_info(real->vmcb->control.exitinfo1, real->vmcb->control.exitinfo2, &npf);

    if (npf.guest_phys_addr < mmio_base_phys ||
        npf.guest_phys_addr >= mmio_base_phys + HYPE_VIRTIO_BLK_BAR_SIZE) {
        return -1;
    }
    offset = (uint32_t)(npf.guest_phys_addr - mmio_base_phys);

    /* Prefer caller-supplied instruction bytes (fetched via guest page-walk):
     * the guest RIP is a guest-VIRTUAL address, only dereferenceable as a host
     * pointer for an identity-mapped guest (M5-1). A paging/RAM-remapping guest
     * (FW-1) must pass `insn`; NULL keeps the identity fast-path. */
    guest_bytes = (insn != 0) ? insn : (const uint8_t *)(uintptr_t)real->vmcb->save.rip;
    if (hype_mmio_decode(guest_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (decoded.is_write != npf.is_write) {
        return -1;
    }

    /* #306: an immediate store carries its value in the instruction and has NO source
     * register -- the ModRM reg field is an opcode extension -- so the GPR lookup is
     * skipped rather than resolving register 0 and writing RAX to the device. */
    reg = decoded.has_imm ? 0 : gpr_ptr(real, decoded.reg);
    if (reg == 0 && !decoded.has_imm) {
        return -1;
    }

    if (offset >= HYPE_VIRTIO_BLK_BAR_COMMON_CFG_OFFSET &&
        offset < HYPE_VIRTIO_BLK_BAR_COMMON_CFG_OFFSET + HYPE_VIRTIO_COMMON_CFG_SIZE) {
        uint32_t region_offset = offset - HYPE_VIRTIO_BLK_BAR_COMMON_CFG_OFFSET;
        if (decoded.is_write) {
            uint32_t value;
            if (decoded.mem_is_dst) {
                /* #307: a read-modify-write of this device register -- read it, combine,
                 * and store the result, rather than storing the other operand alone. */
                uint32_t cur = 0;
                if (hype_virtio_blk_common_cfg_read(dev, region_offset, decoded.size_bytes, &cur) != 0) {
                    return -1;
                }
                value = hype_mmio_rmw_value(&decoded, reg ? *reg : 0u, cur,
                                            &real->vmcb->save.rflags);
            } else {
                value = hype_mmio_store_value(&decoded, reg ? *reg : 0u);
            }
            if (hype_virtio_blk_common_cfg_write(dev, region_offset, decoded.size_bytes, value) != 0) {
                return -1;
            }
        } else {
            uint32_t value = 0;
            if (hype_virtio_blk_common_cfg_read(dev, region_offset, decoded.size_bytes, &value) != 0) {
                return -1;
            }
            hype_mmio_complete_read(&decoded, reg, value, &real->vmcb->save.rflags); /* #457 */
        }
    } else if (offset >= HYPE_VIRTIO_BLK_BAR_NOTIFY_CFG_OFFSET &&
               offset < HYPE_VIRTIO_BLK_BAR_NOTIFY_CFG_OFFSET + 4u) {
        if (decoded.is_write) {
            if (hype_virtio_blk_is_queue_ready(dev)) {
                if (process_virtio_blk_queue(dev, be, dma_map) != 0) {
                    return -1;
                }
            }
        } else {
            hype_mmio_complete_read(&decoded, reg, 0, &real->vmcb->save.rflags); /* #457 */
        }
    } else if (offset == HYPE_VIRTIO_BLK_BAR_ISR_CFG_OFFSET) {
        if (!decoded.is_write) {
            uint8_t value = hype_virtio_blk_isr_read(dev);
            hype_mmio_complete_read(&decoded, reg, value, &real->vmcb->save.rflags); /* #457 */
        }
    } else if (offset >= HYPE_VIRTIO_BLK_BAR_DEVICE_CFG_OFFSET &&
               offset < HYPE_VIRTIO_BLK_BAR_DEVICE_CFG_OFFSET + HYPE_VIRTIO_BLK_CFG_SIZE) {
        if (!decoded.is_write) {
            uint32_t value = 0;
            uint32_t region_offset = offset - HYPE_VIRTIO_BLK_BAR_DEVICE_CFG_OFFSET;
            if (hype_virtio_blk_device_cfg_read(dev, region_offset, decoded.size_bytes, &value) != 0) {
                return -1;
            }
            hype_mmio_complete_read(&decoded, reg, value, &real->vmcb->save.rflags); /* #457 */
        }
    } else {
        /* Reserved area of the MMIO BAR -- reads as 0, writes ignored. */
        if (!decoded.is_write) {
            hype_mmio_complete_read(&decoded, reg, 0, &real->vmcb->save.rflags); /* #457 */
        }
    }

    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

int hype_svm_vcpu_handle_virtio_net_npf(hype_vcpu_ctx_t *ctx, hype_virtio_net_t *dev,
                                        const hype_gpa_map_t *dma_map, uint64_t mmio_base_phys,
                                        hype_virtio_net_tx_fn sink, void *user, uint8_t *scratch,
                                        unsigned int scratch_len,
                                        hype_virtio_net_ring_stats_t *stats, const uint8_t *insn) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_npf_t npf;
    hype_mmio_decode_t decoded;
    uint64_t *reg;
    uint32_t offset;
    const uint8_t *guest_bytes;

    hype_svm_decode_npf_info(real->vmcb->control.exitinfo1, real->vmcb->control.exitinfo2, &npf);

    if (npf.guest_phys_addr < mmio_base_phys ||
        npf.guest_phys_addr >= mmio_base_phys + HYPE_VIRTIO_BLK_BAR_SIZE) {
        return -1;
    }
    offset = (uint32_t)(npf.guest_phys_addr - mmio_base_phys);

    /* The guest RIP is a guest-VIRTUAL address, only dereferenceable as a host pointer for an
     * identity-mapped guest. A real VM must pass `insn`, fetched through the page walk; NULL keeps
     * the identity fast path the microtests use. #565 is what this costs when it is missing: the
     * decode fails and the access is reported as a register hype does not model. */
    guest_bytes = (insn != 0) ? insn : (const uint8_t *)(uintptr_t)real->vmcb->save.rip;
    if (hype_mmio_decode(guest_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (decoded.is_write != npf.is_write) {
        return -1;
    }

    /* #306: an immediate store carries its value in the instruction and has no source register --
     * the ModRM reg field is an opcode extension -- so the GPR lookup is skipped rather than
     * resolving register 0 and writing RAX to the device. */
    reg = decoded.has_imm ? 0 : gpr_ptr(real, decoded.reg);
    if (reg == 0 && !decoded.has_imm) {
        return -1;
    }

    if (offset >= HYPE_VIRTIO_BLK_BAR_COMMON_CFG_OFFSET &&
        offset < HYPE_VIRTIO_BLK_BAR_COMMON_CFG_OFFSET + HYPE_VIRTIO_COMMON_CFG_SIZE) {
        uint32_t region_offset = offset - HYPE_VIRTIO_BLK_BAR_COMMON_CFG_OFFSET;
        if (decoded.is_write) {
            uint32_t value;
            if (decoded.mem_is_dst) {
                /* #307: a read-modify-write of this register -- read it, combine, store the
                 * result, rather than storing the other operand alone. */
                uint32_t cur = 0;
                if (hype_virtio_net_common_cfg_read(dev, region_offset, decoded.size_bytes,
                                                    &cur) != 0) {
                    return -1;
                }
                value = hype_mmio_rmw_value(&decoded, reg ? *reg : 0u, cur,
                                            &real->vmcb->save.rflags);
            } else {
                value = hype_mmio_store_value(&decoded, reg ? *reg : 0u);
            }
            if (hype_virtio_net_common_cfg_write(dev, region_offset, decoded.size_bytes,
                                                 value) != 0) {
                return -1;
            }
        } else {
            uint32_t value = 0;
            if (hype_virtio_net_common_cfg_read(dev, region_offset, decoded.size_bytes,
                                                &value) != 0) {
                return -1;
            }
            hype_mmio_complete_read(&decoded, reg, value, &real->vmcb->save.rflags);
        }
    } else if (offset >= HYPE_VIRTIO_BLK_BAR_NOTIFY_CFG_OFFSET &&
               offset < HYPE_VIRTIO_BLK_BAR_NOTIFY_CFG_OFFSET +
                            HYPE_VIRTIO_NET_NUM_QUEUES * HYPE_VIRTIO_BLK_BAR_NOTIFY_CFG_MULTIPLIER) {
        /*
         * WHICH queue was rung comes from the offset. One doorbell per queue at a 4-byte stride, as
         * the notify capability's multiplier advertises. A single doorbell for both -- which is all
         * a one-queue device needs -- would make a receive notify drain the transmit ring, and that
         * failure looks like packets moving only when traffic happens to flow the other way.
         */
        uint32_t slot = (offset - HYPE_VIRTIO_BLK_BAR_NOTIFY_CFG_OFFSET) /
                        HYPE_VIRTIO_BLK_BAR_NOTIFY_CFG_MULTIPLIER;
        if (decoded.is_write) {
            if (slot == HYPE_VIRTIO_NET_VQ_TX) {
                /* A negative return means the ring could not be walked at all, and nothing was
                 * consumed. That is not an instruction-emulation failure -- the store itself
                 * succeeded -- so RIP still advances and the guest is not re-faulted forever on a
                 * doorbell write it is entitled to make. */
                (void)hype_virtio_net_drain_tx(dev, dma_map, sink, user, scratch, scratch_len,
                                               stats);
            }
            /*
             * A receive notify means the driver has POSTED buffers, not that a frame is waiting.
             * There is nothing to do here: hype delivers into those buffers when a frame arrives,
             * from the dispatch loop. Accepting the write and doing nothing is correct, and is
             * different from ignoring it -- the descriptors the driver just published are read on
             * the next delivery.
             */
        } else {
            hype_mmio_complete_read(&decoded, reg, 0, &real->vmcb->save.rflags);
        }
    } else if (offset == HYPE_VIRTIO_BLK_BAR_ISR_CFG_OFFSET) {
        if (!decoded.is_write) {
            uint8_t value = hype_virtio_net_isr_read(dev); /* read-to-clear */
            hype_mmio_complete_read(&decoded, reg, value, &real->vmcb->save.rflags);
        }
    } else if (offset >= HYPE_VIRTIO_BLK_BAR_DEVICE_CFG_OFFSET &&
               offset < HYPE_VIRTIO_BLK_BAR_DEVICE_CFG_OFFSET + HYPE_VIRTIO_NET_CFG_SIZE) {
        if (!decoded.is_write) {
            uint32_t value = 0;
            uint32_t region_offset = offset - HYPE_VIRTIO_BLK_BAR_DEVICE_CFG_OFFSET;
            if (hype_virtio_net_device_cfg_read(dev, region_offset, decoded.size_bytes,
                                                &value) != 0) {
                return -1;
            }
            hype_mmio_complete_read(&decoded, reg, value, &real->vmcb->save.rflags);
        }
        /* Writes to the device config are dropped: the MAC is hype's, not the driver's. A driver
         * that wants a different address uses the control queue, which this device does not offer
         * (see the feature list in devices/virtio_net.h). */
    } else {
        /* Reserved area of the BAR -- reads as 0, writes ignored, the same convention every other
         * MMIO model here uses. */
        if (!decoded.is_write) {
            hype_mmio_complete_read(&decoded, reg, 0, &real->vmcb->save.rflags);
        }
    }

    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

/*
 * NET-3 (#82): the guest e1000's register window. Same decode/dispatch shape as the virtio-net
 * handler above, with a far simpler body -- one flat register space, no sub-regions to route between.
 */
int hype_svm_vcpu_handle_e1000_dev_npf(hype_vcpu_ctx_t *ctx, hype_e1000_dev_t *dev,
                                       const hype_gpa_map_t *dma_map, uint64_t mmio_base_phys,
                                       hype_virtio_net_tx_fn sink, void *user, uint8_t *scratch,
                                       unsigned int scratch_len,
                                       hype_virtio_net_ring_stats_t *stats, const uint8_t *insn) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_npf_t npf;
    hype_mmio_decode_t decoded;
    uint64_t *reg;
    uint32_t offset;
    const uint8_t *guest_bytes;

    hype_svm_decode_npf_info(real->vmcb->control.exitinfo1, real->vmcb->control.exitinfo2, &npf);

    if (npf.guest_phys_addr < mmio_base_phys ||
        npf.guest_phys_addr >= mmio_base_phys + HYPE_E1000_DEV_BAR_SIZE) {
        return -1;
    }
    offset = (uint32_t)(npf.guest_phys_addr - mmio_base_phys);

    guest_bytes = (insn != 0) ? insn : (const uint8_t *)(uintptr_t)real->vmcb->save.rip;
    if (hype_mmio_decode(guest_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (decoded.is_write != npf.is_write) {
        return -1;
    }
    reg = decoded.has_imm ? 0 : gpr_ptr(real, decoded.reg);
    if (reg == 0 && !decoded.has_imm) {
        return -1;
    }

    if (decoded.is_write) {
        uint32_t value;
        if (decoded.mem_is_dst) {
            uint32_t cur = 0;
            if (hype_e1000_dev_reg_read(dev, offset, decoded.size_bytes, &cur) != 0) {
                return -1;
            }
            value = hype_mmio_rmw_value(&decoded, reg ? *reg : 0u, cur, &real->vmcb->save.rflags);
        } else {
            value = hype_mmio_store_value(&decoded, reg ? *reg : 0u);
        }
        if (hype_e1000_dev_reg_write(dev, offset, decoded.size_bytes, value) != 0) {
            return -1;
        }
        /*
         * A WRITE TO THE TRANSMIT TAIL IS THE DOORBELL. Unlike virtio there is no separate notify
         * region: the driver advances TDT and that is the kick, so the drain has to hang off this
         * one register write. Missing it produces a NIC that accepts descriptors and never sends --
         * and the guest's own counters would show frames queued, which reads like hype losing them.
         */
        if (offset == HYPE_E1000_REG_TDT) {
            (void)hype_e1000_dev_drain_tx(dev, dma_map, sink, user, scratch, scratch_len, stats);
        }
    } else {
        uint32_t value = 0;
        if (hype_e1000_dev_reg_read(dev, offset, decoded.size_bytes, &value) != 0) {
            return -1;
        }
        hype_mmio_complete_read(&decoded, reg, value, &real->vmcb->save.rflags);
    }

    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

int hype_svm_vcpu_handle_fw_cfg_ioio(hype_vcpu_ctx_t *ctx, hype_fw_cfg_t *fw,
                                     const hype_gpa_map_t *dma_map) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_ioio_t io;

    hype_svm_decode_ioio_info1(real->vmcb->control.exitinfo1, &io);

    if (io.port == 0x510u) {
        if (io.is_in) {
            return -1;
        }
        hype_fw_cfg_select(fw, (uint16_t)(real->vmcb->save.rax & 0xFFFFu));
    } else if (io.port == 0x511u) {
        if (!io.is_in) {
            return -1; /* no writable fw_cfg files via the classic port in this scope */
        }
        if (io.is_string) {
            /* SVM-STRIO: `rep insb`/`insw`/`insd` from the data port. This is how
             * OVMF's QemuFwCfgLib fetches the signature/revision and every classic
             * (non-DMA) read: IoReadFifo8 -> one string-IN exit, not one exit per
             * byte. Emulate the whole transfer, writing to guest memory at
             * [ES:RDI], honoring REP count and RFLAGS.DF. Getting this wrong (the
             * old 1-byte-to-AL behavior) corrupts OVMF's fw_cfg probe so it
             * concludes fw_cfg is absent and installs no ACPI. */
            hype_svm_string_io_plan_t plan;
            uint64_t host;
            uint64_t u;

            if (hype_svm_build_string_io_plan(&io, real->gprs[7] /* RDI */, real->gprs[1] /* RCX */,
                                              real->vmcb->save.es.base, real->vmcb->save.rflags,
                                              &plan) != 0) {
                return -1;
            }
            if (plan.byte_count != 0) {
                host = guest_dma_xlate(dma_map, plan.low_gpa, plan.byte_count);
                if (host == 0) {
                    return -1; /* guest buffer out of range -> reject, don't scribble host memory */
                }
                for (u = 0; u < plan.count; u++) {
                    uint64_t addr = plan.descending ? (plan.start_gpa - u * (uint64_t)plan.unit_bytes)
                                                     : (plan.start_gpa + u * (uint64_t)plan.unit_bytes);
                    uint64_t off = addr - plan.low_gpa;
                    uint8_t b;
                    for (b = 0; b < plan.unit_bytes; b++) {
                        ((uint8_t *)(uintptr_t)host)[off + b] = hype_fw_cfg_read_byte(fw);
                    }
                }
            }
            real->gprs[7] = plan.new_index_reg; /* RDI */
            real->gprs[1] = plan.new_count_reg; /* RCX */
        } else {
            real->vmcb->save.rax = (real->vmcb->save.rax & ~0xFFULL) | hype_fw_cfg_read_byte(fw);
        }
    } else if (io.port == 0x514u) {
        if (io.is_in) {
            return -1;
        }
        hype_fw_cfg_dma_addr_high(fw, (uint32_t)(real->vmcb->save.rax & 0xFFFFFFFFu));
    } else if (io.port == 0x518u) {
        uint64_t access_phys;

        if (io.is_in) {
            return -1;
        }

        access_phys = hype_fw_cfg_dma_addr_low(fw, (uint32_t)(real->vmcb->save.rax & 0xFFFFFFFFu));

        /*
         * #667: the whole access-struct-translate / data-buffer-translate / execute / write-back
         * sequence now lives in hype_fw_cfg_dma_op_run() (devices/fw_cfg.c), shared verbatim with
         * the VMX handler, so the VALID-3 GPA-rejection path is independently unit-testable
         * without a full vcpu context. A failed access-struct translation (FW-1's RAM is NOT
         * identity-mapped) is the only case that refuses the exit outright; a bad data-buffer GPA
         * still completes with HYPE_FW_CFG_DMA_CTL_ERROR written back, exactly as before.
         */
        if (hype_fw_cfg_dma_op_run(fw, dma_map, access_phys) != 0) {
            return -1;
        }
    } else {
        return -1;
    }

    /* EXITINFO2 gives the resume RIP directly, same convenience
     * hype_svm_vcpu_handle_ioio() already relies on. */
    real->vmcb->save.rip = real->vmcb->control.exitinfo2;
    return 0;
}

void hype_svm_vcpu_enable_apic_accel_ops(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_vcpu_enable_apic_accel(real->vmcb);
}

/* FW-1e: the per-VM-exit CLGI/VMLOAD/VMRUN trace below brackets the riskiest
 * instruction sequence (see its own comment) -- invaluable during bring-up to
 * localize a VMRUN hang on new hardware. RT-2c: default OFF now that VMRUN is
 * hardware-proven; it emitted 3 lines PER exit (the "long loop of vmrun/stgi"
 * seen at startup, from the regression guests). Re-enable via
 * hype_svm_set_vmrun_trace(1) if a fresh VMRUN-path hang ever needs localizing. */
static int g_vmrun_trace = 0;

void hype_svm_set_vmrun_trace(int enabled) {
    g_vmrun_trace = enabled ? 1 : 0;
}

int hype_svm_vcpu_run(hype_vcpu_ctx_t *ctx, hype_vmexit_info_t *info) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    uint64_t host_tsc_aux = 0; /* #275 */
    uint64_t vmcb_phys = (uint64_t)(uintptr_t)real->vmcb;

    /* Real-hardware debugging: this brackets the single riskiest
     * instruction sequence in the whole boot path. If the last line
     * seen (screen or serial) is the "about to" one below with no
     * matching "VMRUN returned", the fault/hang is inside CLGI/VMLOAD/
     * VMRUN itself -- real bare-metal SVM has fault paths (e.g. an
     * invalid VMCB field bare metal validates more strictly than
     * nested/emulated SVM does) that this project's own QEMU+KVM
     * nested-SVM validation may simply never have exercised. */
    if (g_vmrun_trace) {
        hype_debug_print("svm: about to CLGI/VMLOAD/VMRUN (vmcb_phys=0x%llx)...\n",
                          (unsigned long long)vmcb_phys);
    }
    /* #260: load the guest's x87/SSE state LAST -- after the trace print above,
     * which uses XMM like any other compiled C here -- and save it back FIRST on
     * exit, before the trace print below. Nothing between these two calls may
     * touch vector registers; clgi/vmload/vmsave/stgi are bare instructions. */
    /*
     * #275: run the guest under ITS TSC_AUX, and put hype's back afterwards.
     *
     * Skipped entirely until the guest has actually written the MSR, so the common
     * case costs nothing -- same gating as the VMX XCR0 swap. Two RDMSR/WRMSR pairs
     * per entry is not free, which is why it is conditional rather than unconditional.
     */
    if (real->tsc_aux_valid) {
        host_tsc_aux = svm_rdmsr(HYPE_SVM_MSR_TSC_AUX);
        svm_wrmsr(HYPE_SVM_MSR_TSC_AUX, real->tsc_aux);
    }
    /*
     * #436: CLGI must come FIRST. hype_fpu_restore() puts the GUEST's x87/SSE
     * state in the registers; a host interrupt (the 1ms AP preempt tick)
     * landing between the restore and CLGI runs the ISR dispatch chain, whose
     * compiled C uses XMM (xorps/movups struct zeroing -- measured in
     * isr_decode.o) with NO FPU save in the ISR stubs. That silently zeroed
     * guest vector registers, and a guest resuming a GUID-compare or struct
     * move mid-loop with zeroed XMM walks off otherwise-valid structures --
     * the timing-dependent DxeCore list-walk hangs that blocked every Windows
     * boot. With GIF clear the tick stays pending until the post-#VMEXIT
     * STGI, which is after hype_fpu_save() -- the guest state is never live
     * across any host ISR.
     */
    clgi();
    hype_fpu_restore(&real->fpu);
    vmload(vmcb_phys);
    vmrun_full(real, vmcb_phys);
    /*
     * #244: the flush-this-guest armed at create has now happened. Clear TLB_CONTROL
     * so it does not repeat on every entry -- a permanent per-VMRUN flush would be
     * correct but would throw away this guest's whole nested TLB on every exit, and
     * hype exits often. The ASID field (bits 31:0) is left alone.
     */
    if ((real->vmcb->control.guest_asid_tlb_ctl >> 32) != 0ull) {
        real->vmcb->control.guest_asid_tlb_ctl &= 0xFFFFFFFFull;
    }
    hype_fpu_save(&real->fpu);
    if (real->tsc_aux_valid) {
        svm_wrmsr(HYPE_SVM_MSR_TSC_AUX, host_tsc_aux); /* #275 */
    }
    if (g_vmrun_trace) {
        hype_debug_print("svm: VMRUN returned -- about to VMSAVE/STGI...\n");
    }
    vmsave(vmcb_phys);
    stgi();
    if (g_vmrun_trace) {
        hype_debug_print("svm: STGI done, exitcode=0x%llx\n",
                          (unsigned long long)real->vmcb->control.exitcode);
    }

    info->reason = real->vmcb->control.exitcode;
    info->qualification = real->vmcb->control.exitinfo1;
    info->guest_rip = real->vmcb->save.rip;

    /*
     * #315 (APM Vol 2 §15.7.2/§15.7.3): the intercept may have landed WHILE the guest was delivering
     * an event through its own IDT, in which case EXITINTINFO holds the only surviving copy. For an
     * external interrupt the ack cycle already consumed the vector, so dropping it loses a device
     * interrupt silently.
     *
     * Decided by a pure function (hype_svm_decide_event_replay) rather than inline, and deliberately
     * conservative: only an ack'd INTR/NMI is re-staged. An exception is reproduced by restarting the
     * faulting instruction, so re-injecting it would deliver it twice; a software interrupt has
     * next-RIP semantics hype does not model. Anything hype cannot re-stage safely is REPORTED, never
     * guessed at -- a wrong vector delivered into a guest IDT is far harder to attribute than a log
     * line saying hype declined.
     *
     * EVENTINJ is checked, not assumed: the processor clears it on a successful VMRUN (measured --
     * it is what got #313 rejected), so a still-valid EVENTINJ here means an injection this exit has
     * already claimed the one slot available.
     */
    {
        hype_svm_evtinfo_t e;
        hype_svm_evtreplay_t d;
        int pending_inject = (real->vmcb->control.eventinj & HYPE_SVM_EVENTINJ_V) != 0ULL;

        hype_svm_decode_exitintinfo(real->vmcb->control.exitintinfo, pending_inject, &e);
        d = hype_svm_decide_event_replay(&e);
        if (d == HYPE_SVM_EVTREPLAY_REINJECT) {
            real->vmcb->control.eventinj =
                ((uint64_t)e.vector & HYPE_SVM_EVENTINJ_VECTOR_MASK) |
                ((uint64_t)e.type << HYPE_SVM_EVENTINJ_TYPE_SHIFT) | HYPE_SVM_EVENTINJ_V |
                (e.has_error_code ? (HYPE_SVM_EVENTINJ_EV |
                                     ((uint64_t)e.error_code << HYPE_SVM_EVENTINJ_ERRORCODE_SHIFT))
                                  : 0ULL);
            g_evtreplay_restaged++;
        } else if (d == HYPE_SVM_EVTREPLAY_REFUSE) {
            /* Unconditional, not behind a trace flag: this is rare by construction, and a silent
             * refusal is exactly the lost-interrupt symptom the ticket is about. */
            hype_debug_print("svm: EXITINTINFO type=%u vec=0x%x %s (exitcode 0x%llx)\n", e.type,
                             e.vector, hype_svm_evtreplay_str(d),
                             (unsigned long long)info->reason);
            g_evtreplay_refused++;
        }
    }

    return (info->reason == HYPE_SVM_EXITCODE_INVALID) ? -1 : 0;
}

int hype_svm_vcpu_handle_npf(hype_vcpu_ctx_t *ctx, hype_pflash_t *pf, uint64_t pf_base_phys) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_npf_t npf;
    hype_mmio_decode_t decoded;
    uint64_t *reg;
    uint32_t offset;
    const uint8_t *guest_bytes;

    hype_svm_decode_npf_info(real->vmcb->control.exitinfo1, real->vmcb->control.exitinfo2, &npf);

    if (npf.guest_phys_addr < pf_base_phys) {
        return -1;
    }
    offset = (uint32_t)(npf.guest_phys_addr - pf_base_phys);

    /* AMD Decode Assist (VMCB's num_bytes_fetched/guest_instruction_bytes)
     * was the original plan here, but confirmed empirically -- via a
     * real QEMU/KVM run under this project's own nested-SVM dev
     * environment -- that it is NOT reliably populated even when the
     * underlying CPU's own CPUID leaf 0x8000000A advertises the
     * feature (nested SVM emulation's own gap, not this project's).
     * Reading the faulting instruction directly out of guest memory at
     * RIP sidesteps that gap entirely and is at least as correct: this
     * project's guest/NPT setup is a flat identity map (guest-virtual
     * == guest-physical == host-physical), so vmcb->save.rip is
     * already a valid host pointer with no translation needed -- the
     * exact same assumption M3-5's test-guest setup already relies on
     * when it writes the guest's payload bytes directly via a raw
     * pointer before the guest ever runs. */
    guest_bytes = (const uint8_t *)(uintptr_t)real->vmcb->save.rip;

    if (hype_mmio_decode(guest_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }

    /* Defense-in-depth: EXITINFO1's own write bit and the decoded
     * instruction's direction must agree -- a mismatch means either
     * decode went wrong or this handler is being called for a fault
     * that isn't really the decoded instruction's, and it is not safe
     * to guess which. */
    if (decoded.is_write != npf.is_write) {
        return -1;
    }

    /* #306: an immediate store carries its value in the instruction and has NO source
     * register -- the ModRM reg field is an opcode extension -- so the GPR lookup is
     * skipped rather than resolving register 0 and writing RAX to the device. */
    reg = decoded.has_imm ? 0 : gpr_ptr(real, decoded.reg);
    if (reg == 0 && !decoded.has_imm) {
        return -1;
    }

    if (decoded.is_write) {
        uint32_t value;
        if (decoded.mem_is_dst) {
            /* #307: a read-modify-write of this device register -- read it, combine,
             * and store the result, rather than storing the other operand alone. */
            uint32_t cur = 0;
            if (hype_pflash_read(pf, offset, decoded.size_bytes, &cur) != 0) {
                return -1;
            }
            value = hype_mmio_rmw_value(&decoded, reg ? *reg : 0u, cur,
                                        &real->vmcb->save.rflags);
        } else {
            value = hype_mmio_store_value(&decoded, reg ? *reg : 0u);
        }
        if (hype_pflash_write(pf, offset, decoded.size_bytes, value) != 0) {
            return -1;
        }
    } else {
        uint32_t value = 0;
        if (hype_pflash_read(pf, offset, decoded.size_bytes, &value) != 0) {
            return -1;
        }
        /* #457: shared completion -- also covers the immediate CMP, where reg is NULL. */
        hype_mmio_complete_read(&decoded, reg, value, &real->vmcb->save.rflags);
    }

    /* Same "next-RIP-for-free" convenience as HLT/IOIO, just sourced
     * from the decoder's own computed instruction length instead of an
     * EXITINFO2 the hardware doesn't provide for NPF (EXITINFO2 there
     * is the faulting *address*, already consumed above). */
    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

/*
 * The MSR the guest was accessing at the last MSR intercept (guest RCX).
 * Exists so the "unhandled guest MSR access" fatal can NAME the register:
 * without it the message carries only the read/write bit and a RIP, which is
 * not enough to decide whether to model the MSR or to look for a bug.
 */
uint32_t hype_svm_vcpu_get_msr_index(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    return (uint32_t)real->gprs[1]; /* RCX */
}

void hype_svm_vcpu_set_hv_enabled(hype_vcpu_ctx_t *ctx, int enabled) {
    ((struct hype_vcpu_ctx *)ctx)->hv_enabled = enabled ? 1 : 0;
}

/* ---- #202 slice 6a: NVMe BAR0 MMIO ------------------------------------------------------------- */

int hype_svm_vcpu_handle_nvme_npf(hype_vcpu_ctx_t *ctx, hype_nvme_t *dev,
                                  const hype_nvme_ctx_t *nctx, uint64_t mmio_base_phys,
                                  uint32_t bar_size, const uint8_t *insn) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_npf_t npf;
    hype_mmio_decode_t decoded;
    uint64_t *reg;
    uint32_t offset;
    const uint8_t *guest_bytes;

    if (dev == 0 || nctx == 0) {
        return -1;
    }
    hype_svm_decode_npf_info(real->vmcb->control.exitinfo1, real->vmcb->control.exitinfo2, &npf);

    /* bar_size comes from the caller so this can never emulate a wider window than the BAR claims. */
    if (npf.guest_phys_addr < mmio_base_phys ||
        npf.guest_phys_addr >= mmio_base_phys + (uint64_t)bar_size) {
        return -1;
    }
    offset = (uint32_t)(npf.guest_phys_addr - mmio_base_phys);

    guest_bytes = (insn != 0) ? insn : (const uint8_t *)(uintptr_t)real->vmcb->save.rip;
    if (hype_mmio_decode(guest_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (decoded.is_write != npf.is_write) {
        return -1;
    }
    /* #306: an immediate store has no source register -- the ModRM reg field is an opcode extension. */
    reg = decoded.has_imm ? 0 : gpr_ptr(real, decoded.reg);
    if (reg == 0 && !decoded.has_imm) {
        return -1;
    }

    if (decoded.is_write) {
        uint32_t value = hype_mmio_store_value(&decoded, reg ? *reg : 0u);
        unsigned int qid;
        int is_cq;

        hype_nvme_mmio_write32(dev, offset, value);
        /*
         * A SUBMISSION queue doorbell is the guest saying "there is work". hype has no worker thread,
         * so the queue is drained HERE, synchronously, before the guest resumes -- a doorbell whose
         * commands are never fetched is indistinguishable from a dead controller.
         *
         * Completion-queue doorbells only move the consumer index; there is nothing to do for them.
         */
        if (hype_nvme_doorbell_decode(offset, &qid, &is_cq) == 0 && !is_cq) {
            /* #372: the refusal itself lives in hype_nvme_process_sq (it owns the state), but the
             * REPORT has to be here: devices/nvme.c is host-unit-tested, and calling
             * hype_debug_print from inside it faults the test binary -- the #296 lesson. */
            if (dev->bus_master == 0) {
                static int reported;
                if (!reported) {
                    reported = 1;
                    hype_debug_print("nvme: doorbell IGNORED -- the guest has not set PCI Bus "
                                     "Master Enable (Command bit 2), so the controller cannot "
                                     "fetch an SQE, walk a PRP or post a completion. This command "
                                     "will never complete, exactly as on real hardware. [#372]\n");
                }
            }
            (void)hype_nvme_process_sq(dev, qid, nctx);
        }
    } else {
        uint32_t value = hype_nvme_mmio_read32(dev, offset);
        /*
         * MOV only. Unlike the LAPIC (#305), no firmware or OS reads an NVMe register with an ALU form:
         * these are plain 32-bit register loads. Refusing anything else keeps hype from silently
         * emulating half an instruction -- if a guest ever does it, the refusal says so rather than
         * leaving RFLAGS stale.
         */
        if (decoded.op != HYPE_MMIO_ALU_MOV) {
            return -1;
        }
        hype_mmio_complete_read(&decoded, reg, value, &real->vmcb->save.rflags); /* #457 */
    }

    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

void hype_svm_vcpu_set_topology(hype_vcpu_ctx_t *ctx, uint32_t apic_id, uint32_t vcpu_count,
                                uint32_t threads_per_core) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    real->cpuid_topo.apic_id = apic_id;
    real->cpuid_topo.vcpu_count = vcpu_count ? vcpu_count : 1u;
    real->cpuid_topo.threads_per_core = threads_per_core ? threads_per_core : 1u;
}
