#include "svm.h"

#include "../../../core/guest_mem.h"

#include "../../../core/fatal.h"

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
};

static hype_vmcb_t g_vmcb __attribute__((aligned(4096)));
static struct hype_vcpu_ctx g_ctx;

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
static inline void vmrun_full(uint64_t vmcb_phys) {
    uint64_t clobbered_rax = vmcb_phys;
    __asm__ volatile("mov %[rcx], %%rcx\n\t"
                      "mov %[rdx], %%rdx\n\t"
                      "mov %[rbx], %%rbx\n\t"
                      "mov %[rbp], %%rbp\n\t"
                      "mov %[rsi], %%rsi\n\t"
                      "mov %[rdi], %%rdi\n\t"
                      "mov %[r8], %%r8\n\t"
                      "mov %[r9], %%r9\n\t"
                      "mov %[r10], %%r10\n\t"
                      "mov %[r11], %%r11\n\t"
                      "mov %[r12], %%r12\n\t"
                      "mov %[r13], %%r13\n\t"
                      "mov %[r14], %%r14\n\t"
                      "mov %[r15], %%r15\n\t"
                      "vmrun %%rax\n\t"
                      "mov %%rcx, %[rcx]\n\t"
                      "mov %%rdx, %[rdx]\n\t"
                      "mov %%rbx, %[rbx]\n\t"
                      "mov %%rbp, %[rbp]\n\t"
                      "mov %%rsi, %[rsi]\n\t"
                      "mov %%rdi, %[rdi]\n\t"
                      "mov %%r8, %[r8]\n\t"
                      "mov %%r9, %[r9]\n\t"
                      "mov %%r10, %[r10]\n\t"
                      "mov %%r11, %[r11]\n\t"
                      "mov %%r12, %[r12]\n\t"
                      "mov %%r13, %[r13]\n\t"
                      "mov %%r14, %[r14]\n\t"
                      "mov %%r15, %[r15]\n\t"
                      : "+a"(clobbered_rax), [rcx] "+m"(g_ctx.gprs[1]), [rdx] "+m"(g_ctx.gprs[2]),
                        [rbx] "+m"(g_ctx.gprs[3]), [rbp] "+m"(g_ctx.gprs[5]), [rsi] "+m"(g_ctx.gprs[6]),
                        [rdi] "+m"(g_ctx.gprs[7]), [r8] "+m"(g_ctx.gprs[8]), [r9] "+m"(g_ctx.gprs[9]),
                        [r10] "+m"(g_ctx.gprs[10]), [r11] "+m"(g_ctx.gprs[11]), [r12] "+m"(g_ctx.gprs[12]),
                        [r13] "+m"(g_ctx.gprs[13]), [r14] "+m"(g_ctx.gprs[14]), [r15] "+m"(g_ctx.gprs[15])
                      :
                      : "memory", "cc", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "r8", "r9", "r10",
                        "r11", "r12", "r13", "r14", "r15");
}

static inline void vmsave(uint64_t vmcb_phys) {
    __asm__ volatile("vmsave %%rax" : : "a"(vmcb_phys) : "memory");
}

static void reset_gprs(void) {
    unsigned i;
    for (i = 0; i < 16; i++) {
        g_ctx.gprs[i] = 0;
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

    hype_vmcb_build_realmode_guest(&g_vmcb, guest_rip, guest_rsp, (uint64_t)(uintptr_t)g_iopm,
                                    (uint64_t)(uintptr_t)g_msrpm);

    /* 0 means "no nested paging" (M2's original, still-supported
     * scope) -- a real NPT root is always a nonzero, page-aligned
     * physical address. See vmcb.h's HYPE_SVM_INT_CTL_AVIC_ENABLE
     * comment: AVIC additionally requires this to have been called. */
    if (ept_or_npt_root != 0) {
        hype_vmcb_enable_nested_paging(&g_vmcb, ept_or_npt_root);
    }

    g_ctx.vmcb = &g_vmcb;
    reset_gprs();
    return &g_ctx;
}

hype_vcpu_ctx_t *hype_svm_vcpu_create_long_mode(uint64_t entry_rip, uint64_t guest_cr3, uint64_t rsp,
                                                 uint64_t npt_root) {
    unsigned i;

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

    hype_vmcb_build_long_mode_guest(&g_vmcb, entry_rip, guest_cr3, rsp, (uint64_t)(uintptr_t)g_iopm,
                                     (uint64_t)(uintptr_t)g_msrpm);

    if (npt_root != 0) {
        hype_vmcb_enable_nested_paging(&g_vmcb, npt_root);
    }

    g_ctx.vmcb = &g_vmcb;
    reset_gprs();
    return &g_ctx;
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
            rc = hype_pic_emu_io_write(pic, io.port, (uint8_t)(real->vmcb->save.rax & 0xFFu));
        }
    } else if (io.port >= 0x40u && io.port <= 0x43u) {
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

void hype_svm_vcpu_handle_cpuid(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    uint32_t eax_in = (uint32_t)real->vmcb->save.rax;
    uint32_t ecx_in = (uint32_t)real->gprs[1]; /* RCX */
    hype_cpuid_result_t host_real;
    hype_cpuid_result_t out;

    real_cpuid(eax_in, ecx_in, &host_real);
    hype_cpuid_emulate(eax_in, ecx_in, &host_real, &out);

    /* CPUID zero-extends all four registers to their full 64-bit width
     * in 64-bit mode -- assigning a uint32_t into a uint64_t field
     * already does that zero-extension. */
    real->vmcb->save.rax = out.eax;
    real->gprs[3] = out.ebx; /* RBX */
    real->gprs[1] = out.ecx; /* RCX */
    real->gprs[2] = out.edx; /* RDX */

    real->vmcb->save.rip += 2; /* CPUID is always exactly 2 bytes (0F A2) */
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

    if (io.port == HYPE_PCI_CF8_PORT) {
        if (io.is_in) {
            uint32_t value = hype_pci_cf8_read(pci);
            real->vmcb->save.rax =
                hype_mmio_merge_read_value(real->vmcb->save.rax, value, io.size_bytes, io.size_bytes == 4);
        } else {
            hype_pci_cf8_write(pci, hype_mmio_extract_write_value(real->vmcb->save.rax, io.size_bytes));
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

void hype_svm_set_ps2_trace(int enabled) {
    g_ps2_trace = enabled ? 1 : 0;
}

/* FW-1h diagnostic: trace every AHCI command-slot dispatch (the CDB
 * opcode, whether it is an ATAPI PACKET, and the resulting status), so
 * we can see exactly what OVMF's AtaAtapiPassThru/ScsiDisk stack asks
 * the emulated CD-ROM for during boot-device discovery. Off by default;
 * FW-1's guest turns it on right before launch. */
static int g_ahci_trace = 0;

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
            if (hype_ps2_mouse_has_pending_byte(mouse)) {
                value = hype_ps2_mouse_read_byte(mouse);
            } else {
                hype_ps2_kbd_io_read(kbd, HYPE_PS2_PORT_DATA, &value);
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
            if (hype_ps2_mouse_has_pending_byte(mouse)) {
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

    if (g_ps2_trace) {
        hype_debug_print("fw-1 ps2| %s 0x%x %s=0x%x rip=0x%llx\n", io.is_in ? "IN " : "OUT",
                          (unsigned int)io.port, io.is_in ? "->" : "<-", (unsigned int)traced_value,
                          (unsigned long long)real->vmcb->save.rip);
    }

    /* EXITINFO2 gives the resume RIP directly, same "next-RIP-for-free"
     * convenience hype_svm_vcpu_handle_ioio() itself already relies on. */
    real->vmcb->save.rip = real->vmcb->control.exitinfo2;
    return 0;
}

#define HYPE_FW_1_ACPI_PM_TIMER_PORT 0x608u
#define HYPE_FW_1_ACPI_PM_TIMER_MASK 0x00FFFFFFu /* 24-bit -- TMR_VAL_EXT unset in this project's own FADT */

int hype_svm_vcpu_handle_acpi_pm_timer_ioio(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_ioio_t io;

    hype_svm_decode_ioio_info1(real->vmcb->control.exitinfo1, &io);

    if (io.port != HYPE_FW_1_ACPI_PM_TIMER_PORT) {
        return -1;
    }

    if (io.is_in) {
        uint32_t value = (uint32_t)real_rdtsc() & HYPE_FW_1_ACPI_PM_TIMER_MASK;
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

/* This project's own current single-IRQ-source scope (see
 * hype_svm_vcpu_request_interrupt()'s own comment) -- one pending
 * vector is all that's ever needed right now. */
static int g_pending_irq_valid = 0;
static uint8_t g_pending_irq_vector = 0;

/* M4-6d2 DIAG: interrupt-injection path counters (INT-1/INT-2). */
static unsigned long long g_int_eventinj = 0;   /* accepted immediately (direct EVENTINJ) */
static unsigned long long g_int_vintr_defer = 0; /* couldn't accept -> VINTR window armed */
static unsigned long long g_int_vintr_window = 0;/* VINTR window fired -> deferred inject */
static unsigned long long g_int_defer_overwrite = 0; /* a deferral clobbered an unserviced pending vector */

void hype_svm_vcpu_get_int_diag(unsigned long long *eventinj, unsigned long long *defer,
                                 unsigned long long *window, unsigned long long *overwrite) {
    *eventinj = g_int_eventinj;
    *defer = g_int_vintr_defer;
    *window = g_int_vintr_window;
    *overwrite = g_int_defer_overwrite;
}

void hype_svm_vcpu_get_intr_state(hype_vcpu_ctx_t *ctx, hype_svm_intr_state_t *out) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    out->rflags = real->vmcb->save.rflags;
    out->interrupt_shadow = real->vmcb->control.interrupt_shadow;
    out->eventinj = real->vmcb->control.eventinj;
    out->vintr = real->vmcb->control.vintr;
    out->can_accept =
        hype_svm_can_accept_interrupt(real->vmcb->save.rflags, real->vmcb->control.interrupt_shadow);
    out->pending_valid = g_pending_irq_valid;
    out->pending_vector = g_pending_irq_vector;
}

void hype_svm_vcpu_request_interrupt(hype_vcpu_ctx_t *ctx, uint8_t vector) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;

    if (hype_svm_can_accept_interrupt(real->vmcb->save.rflags, real->vmcb->control.interrupt_shadow)) {
        real->vmcb->control.eventinj = hype_svm_encode_eventinj_intr(vector);
        g_int_eventinj++;
        return;
    }

    if (g_pending_irq_valid) {
        g_int_defer_overwrite++; /* a second vector deferred before the first was delivered */
    }
    real->vmcb->control.vintr = hype_svm_arm_vintr_request(real->vmcb->control.vintr);
    real->vmcb->control.intercept_misc1 |= HYPE_SVM_INTERCEPT_VINTR;
    g_pending_irq_valid = 1;
    g_pending_irq_vector = vector;
    g_int_vintr_defer++;
}

void hype_svm_vcpu_handle_vintr_window(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;

    real->vmcb->control.vintr = hype_svm_disarm_vintr_request(real->vmcb->control.vintr);
    real->vmcb->control.intercept_misc1 &= ~HYPE_SVM_INTERCEPT_VINTR;

    if (g_pending_irq_valid) {
        uint8_t vector = g_pending_irq_vector;
        g_pending_irq_valid = 0;
        g_int_vintr_window++;
        /* This window firing at all means the guest can accept an
         * interrupt right now -- hype_svm_vcpu_request_interrupt()'s
         * own can-accept check will take the direct-EVENTINJ path. */
        hype_svm_vcpu_request_interrupt(ctx, vector);
    }
}

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
    if (!g_pending_irq_valid) {
        return 0;
    }
    if ((real->vmcb->control.eventinj & HYPE_SVM_EVENTINJ_V) != 0) {
        return 0; /* an event is already staged for the next VMRUN */
    }
    if (!hype_svm_can_accept_interrupt(real->vmcb->save.rflags, real->vmcb->control.interrupt_shadow)) {
        return 0;
    }
    real->vmcb->control.eventinj = hype_svm_encode_eventinj_intr(g_pending_irq_vector);
    g_pending_irq_valid = 0;
    real->vmcb->control.vintr = hype_svm_disarm_vintr_request(real->vmcb->control.vintr);
    real->vmcb->control.intercept_misc1 &= ~HYPE_SVM_INTERCEPT_VINTR;
    g_int_eventinj++;
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
}

void hype_svm_vcpu_set_rip(hype_vcpu_ctx_t *ctx, uint64_t rip) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    real->vmcb->save.rip = rip;
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

uint64_t hype_svm_vcpu_get_cr3(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    return real->vmcb->save.cr3;
}

int hype_svm_vcpu_handle_msr(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    int is_write = (real->vmcb->control.exitinfo1 & 0x1ULL) != 0;
    uint32_t msr_number = (uint32_t)real->gprs[1]; /* RCX */
    hype_msr_action_t action;

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

    action = hype_msr_decide(msr_number, is_write);

    switch (action) {
    case HYPE_MSR_ACTION_READ_APIC_BASE: {
        uint64_t value = hype_msr_apic_base_value();
        real->vmcb->save.rax = (uint64_t)(uint32_t)value;
        real->gprs[2] = (uint64_t)(uint32_t)(value >> 32); /* RDX */
        break;
    }
    case HYPE_MSR_ACTION_READWRITE_EFER:
        if (is_write) {
            uint64_t value =
                ((uint64_t)(uint32_t)real->gprs[2] << 32) | (uint64_t)(uint32_t)real->vmcb->save.rax;
            real->vmcb->save.efer = value;
        } else {
            real->vmcb->save.rax = (uint64_t)(uint32_t)real->vmcb->save.efer;
            real->gprs[2] = (uint64_t)(uint32_t)(real->vmcb->save.efer >> 32);
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
            if (!is_write) {
                real->vmcb->save.rax = 0;
                real->gprs[2] = 0;
            }
            /* permissive: WRMSR ignored, RDMSR returns 0 -- discovery only */
            real->vmcb->save.rip += 2;
            return 0;
        }
        return -1;
    }

    real->vmcb->save.rip += 2; /* RDMSR/WRMSR are always exactly 2 bytes (0F 32 / 0F 30) */
    return 0;
}

/* Max bytes hype_mmio_decode() could ever need for the narrow
 * instruction forms it supports (prefix + REX + two-byte opcode +
 * ModRM + SIB + disp32 -- the longest case, MOVZX with a SIB-plus-
 * disp32 memory operand) -- comfortably under x86's own 15-byte
 * legal-instruction-length limit. */
#define HYPE_MMIO_MAX_INSTR_BYTES 15

/* VALID-3 guest-physical -> host translation for the AHCI DMA path.
 * A NULL dma_map means "trusted identity-mapped guest" (the M4-5/ISO-2/
 * PCI-2/M5-2 cooperating test guests, whose NPT identity-maps RAM so
 * guest-physical == host and whose DMA addresses this project itself
 * wrote) -- return the address unchecked, preserving their exact prior
 * behavior. A non-NULL map (FW-1's real OVMF/OS guest) routes every
 * guest-supplied address through the bounds-checked VALID-1 lookup with
 * its access length; a 0 return (out of range / straddling / overrun /
 * overflow) propagates as "reject" to the caller. */
static uint64_t ahci_dma_xlate(const hype_gpa_map_t *dma_map, uint64_t gpa, uint64_t len) {
    if (dma_map == 0) {
        return gpa;
    }
    return hype_gpa_to_host(dma_map, gpa, len);
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
static int process_ahci_command_slot(hype_ahci_t *ahci, hype_atapi_t *atapi,
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
    const uint8_t *src;
    uint32_t remaining;
    uint32_t transferred;
    uint32_t prd_idx;
    uint8_t status_reg;
    uint8_t error_reg;
    uint32_t pis_bit;
    uint8_t *d2h_fis;
    unsigned i;

    /* VALID-3: every guest-physical address the AHCI command structures
     * carry is guest-controlled, so each is translated through the VM's
     * bounds-checked gpa map (VALID-1) -- with its access length -- and
     * a rejected (0) translation fails the command rather than
     * dereferencing an out-of-range host pointer. The command header is
     * the 32-byte slot-0 entry. */
    cmd_hdr_bytes = (uint8_t *)(uintptr_t)ahci_dma_xlate(dma_map, cmd_list_phys, 32u);
    if (cmd_hdr_bytes == 0) {
        return -1;
    }

    hype_ahci_decode_cmd_header(cmd_hdr_bytes, &hdr);
    /* Command Table = 0x80-byte CFIS/ACMD/reserved block + prdtl 16-byte
     * PRDT entries. A malicious prdtl that would run the table off the
     * region is caught here (the length is computed in 64-bit so it
     * cannot wrap before the check). */
    cmd_table_bytes = (const uint8_t *)(uintptr_t)ahci_dma_xlate(
        dma_map, hdr.cmd_table_phys, (uint64_t)0x80u + (uint64_t)hdr.prdtl * 16u);
    if (cmd_table_bytes == 0) {
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
            if (g_ahci_trace) {
                hype_debug_print("ahci-trace: non-ATAPI slot0 with bad FIS type=0x%x cmd=0x%x\n",
                                  (unsigned int)cmd_table_bytes[0], (unsigned int)ata_cmd);
            }
            return -1;
        }
        if (ata_cmd == HYPE_AHCI_ATA_CMD_IDENTIFY_PACKET_DEVICE) {
            hype_atapi_build_identify(atapi, identify);
            src = identify;
            remaining = HYPE_ATAPI_IDENTIFY_SIZE;
            status_reg = 0x50u; /* DRDY|DSC */
            error_reg = 0;
            pis_bit = HYPE_AHCI_PIS_PSS;
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
            if (g_ahci_trace) {
                hype_debug_print("ahci-trace: unhandled non-ATAPI command slot0 -- FIS type=0x%x cmd=0x%x\n",
                                  (unsigned int)cmd_table_bytes[0], (unsigned int)ata_cmd);
            }
            return -1;
        }
    } else {
        uint8_t cdb[HYPE_ATAPI_CDB_MAX];
        if (cmd_table_bytes[0] != 0x27u || cmd_table_bytes[2] != 0xA0u) {
            if (g_ahci_trace) {
                hype_debug_print(
                    "ahci-trace: ATAPI slot0 but FIS type/cmd unexpected (fistype=0x%x cmd=0x%x)\n",
                    (unsigned int)cmd_table_bytes[0], (unsigned int)cmd_table_bytes[2]);
            }
            return -1; /* not a Register H2D FIS carrying ATA_CMD_PACKET (0xA0) */
        }
        for (i = 0; i < HYPE_ATAPI_CDB_MAX; i++) {
            cdb[i] = cmd_table_bytes[0x40 + i];
        }

        hype_atapi_execute_cdb(atapi, cdb, &result);

        if (g_ahci_trace) {
            hype_debug_print(
                "ahci-trace: ATAPI CDB=0x%x lba=%u/%u status=%s uses_media=%u len=%u\n",
                (unsigned int)cdb[0], (unsigned int)cdb[2], (unsigned int)cdb[5],
                result.status == HYPE_ATAPI_STATUS_GOOD ? "GOOD" : "CHECK",
                (unsigned int)result.uses_media_data,
                (unsigned int)(result.uses_media_data ? result.media_length : result.synth_length));
        }

        src = result.uses_media_data ? (atapi->media_data + result.media_offset) : result.synth_data;
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
    }

    prdt_bytes = cmd_table_bytes + 0x80;
    prd_idx = 0;
    transferred = 0;
    while (remaining > 0 && prd_idx < hdr.prdtl) {
        hype_ahci_prdt_entry_t prd;
        uint32_t chunk;
        uint8_t *dst;
        uint32_t j;

        hype_ahci_decode_prdt_entry(prdt_bytes + (uint32_t)prd_idx * 16u, &prd);
        chunk = (prd.byte_count < remaining) ? prd.byte_count : remaining;
        /* VALID-3: the PRD data buffer is guest-supplied -- bounds-check
         * [data_phys, data_phys+chunk) before writing the response into
         * it, so a guest-programmed PRD can never steer the copy at
         * hypervisor or another VM's memory. */
        dst = (uint8_t *)(uintptr_t)ahci_dma_xlate(dma_map, prd.data_phys, chunk);
        if (dst == 0) {
            return -1;
        }
        for (j = 0; j < chunk; j++) {
            dst[j] = src[j];
        }
        src += chunk;
        remaining -= chunk;
        transferred += chunk;
        prd_idx++;
    }

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
    rx_fis_host = (uint8_t *)(uintptr_t)ahci_dma_xlate(dma_map, rx_fis_phys, 0x40u + 20u);
    if (rx_fis_host == 0) {
        return -1;
    }
    d2h_fis = rx_fis_host + 0x40;
    for (i = 0; i < 20; i++) {
        d2h_fis[i] = 0;
    }
    d2h_fis[0] = 0x34; /* FIS type: Register - Device to Host */
    d2h_fis[2] = status_reg;
    d2h_fis[3] = error_reg;

    ahci->p_ci &= (uint32_t)~(1u << slot); /* this slot complete */
    /* Completion interrupt-status bit (PxIS.DHRS for D2H completions,
     * PxIS.PSS for PIO-in). A guest that polls waits on this directly;
     * one that took the interrupt-driven path (M4-6d2) enabled PxIE, so
     * also latch the port's bit in the global IS register -- its ISR
     * (Linux ahci_interrupt) reads IS first to learn which port fired.
     * The vCPU loop turns (GHC.IE && PxIS&PxIE) into a raised PIC IRQ
     * via hype_ahci_irq_pending(). */
    ahci->p_is |= pis_bit;
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
        guest_bytes = (const uint8_t *)(uintptr_t)ahci_dma_xlate(dma_map, real->vmcb->save.rip, 1u);
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

    reg = gpr_ptr(real, decoded.reg);
    if (reg == 0) {
        return -1;
    }

    if (decoded.is_write) {
        uint32_t value = hype_mmio_extract_write_value(*reg, decoded.size_bytes);
        if (g_ahci_trace) {
            hype_debug_print("ahci-trace: ABAR write off=0x%x val=0x%x\n", (unsigned int)offset,
                              (unsigned int)value);
        }
        if (hype_ahci_mmio_write(ahci, offset, decoded.size_bytes, value) != 0) {
            return -1;
        }
        if (offset == HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CI && ahci->p_ci != 0) {
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
        if (g_ahci_trace) {
            hype_debug_print("ahci-trace: ABAR read  off=0x%x val=0x%x\n", (unsigned int)offset,
                              (unsigned int)value);
        }
        *reg = hype_mmio_merge_read_value(*reg, value, decoded.size_bytes, decoded.zero_extend);
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
static void complete_ahci_command_slot0(hype_ahci_t *ahci, uint64_t rx_fis_phys, uint8_t status_reg,
                                         uint8_t error_reg) {
    uint8_t *d2h_fis = (uint8_t *)(uintptr_t)(rx_fis_phys + 0x40);
    unsigned i;

    ahci->p_tfd = (uint32_t)status_reg | ((uint32_t)error_reg << 8);

    for (i = 0; i < 20; i++) {
        d2h_fis[i] = 0;
    }
    d2h_fis[0] = 0x34; /* FIS type: Register - Device to Host */
    d2h_fis[2] = status_reg;
    d2h_fis[3] = error_reg;

    ahci->p_ci &= ~0x1u;
    /* PxIS.DHRS -- the D2H Register FIS interrupt bit a real driver
     * polls for a plain-ATA command's completion (same correction as
     * the ATAPI path; the M4-5/M5-2 cooperating test guests polled PxCI
     * and never depended on this bit). Latch the global IS port bit for
     * an interrupt-driven guest, same as the ATAPI path (M4-6d2). */
    ahci->p_is |= HYPE_AHCI_PIS_DHRS;
    if ((ahci->p_is & ahci->p_ie) != 0) {
        ahci->is |= HYPE_AHCI_IS_PORT0;
    }
}

/* M5-2's plain-ATA command dispatch, the H2D-FIS-command-byte-driven
 * counterpart to process_ahci_command_slot0()'s own ATAPI-only path.
 * Returns -1 for anything that isn't this handler's command (the
 * Command Header carries an ATAPI PACKET, or the H2D FIS isn't a valid
 * command FIS at all, or the command byte isn't one this project
 * models) so the caller can fall through to whichever other handler
 * actually owns it. */
static int process_ahci_ata_command_slot0(hype_ahci_t *ahci, hype_ata_disk_t *disk) {
    uint64_t cmd_list_phys = (uint64_t)ahci->p_clb | ((uint64_t)ahci->p_clbu << 32);
    uint64_t rx_fis_phys = (uint64_t)ahci->p_fb | ((uint64_t)ahci->p_fbu << 32);
    const uint8_t *cmd_hdr_bytes = (const uint8_t *)(uintptr_t)cmd_list_phys;
    hype_ahci_cmd_header_t hdr;
    const uint8_t *cmd_table_bytes;
    const uint8_t *prdt_bytes;
    hype_ahci_h2d_fis_t fis;
    uint8_t identify[HYPE_ATA_IDENTIFY_SIZE];
    const uint8_t *src = 0;
    uint8_t *dst_media = 0;
    uint32_t remaining;
    uint32_t prd_idx;
    uint8_t status_reg;
    uint8_t error_reg;
    int is_write_direction = 0;

    hype_ahci_decode_cmd_header(cmd_hdr_bytes, &hdr);
    if (hdr.is_atapi) {
        return -1; /* not this handler's command -- the ATAPI path owns it */
    }

    cmd_table_bytes = (const uint8_t *)(uintptr_t)hdr.cmd_table_phys;
    if (cmd_table_bytes[0] != 0x27u || (cmd_table_bytes[1] & 0x80u) == 0u) {
        return -1; /* not a valid H2D Register FIS carrying a command */
    }
    hype_ahci_decode_h2d_fis(cmd_table_bytes, &fis);

    status_reg = HYPE_ATA_STATUS_DRDY;
    error_reg = 0;
    remaining = 0;

    if (fis.command == HYPE_ATA_CMD_IDENTIFY_DEVICE) {
        hype_ata_disk_build_identify(disk, identify);
        src = identify;
        remaining = HYPE_ATA_IDENTIFY_SIZE;
    } else if (fis.command == HYPE_ATA_CMD_READ_DMA_EXT) {
        uint32_t sector_count = hype_ata_disk_resolve_sector_count(fis.count);
        if (hype_ata_disk_range_in_bounds(disk, fis.lba, sector_count)) {
            src = disk->media + fis.lba * HYPE_ATA_SECTOR_SIZE;
            remaining = sector_count * HYPE_ATA_SECTOR_SIZE;
        } else {
            status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_ERR);
            error_reg = 0x10u; /* IDNF -- ID Not Found, the real ATA convention for an out-of-range LBA */
        }
    } else if (fis.command == HYPE_ATA_CMD_WRITE_DMA_EXT) {
        uint32_t sector_count = hype_ata_disk_resolve_sector_count(fis.count);
        is_write_direction = 1;
        if (hype_ata_disk_range_in_bounds(disk, fis.lba, sector_count)) {
            dst_media = disk->media + fis.lba * HYPE_ATA_SECTOR_SIZE;
            remaining = sector_count * HYPE_ATA_SECTOR_SIZE;
        } else {
            status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_ERR);
            error_reg = 0x10u;
        }
    } else if (fis.command == HYPE_ATA_CMD_FLUSH_CACHE_EXT) {
        /* Nothing to stream -- an immediate, no-data completion. */
    } else {
        return -1; /* unrecognized command -- outside this project's own modeled ATA subset */
    }

    prdt_bytes = cmd_table_bytes + 0x80;
    prd_idx = 0;
    while (remaining > 0 && prd_idx < hdr.prdtl) {
        hype_ahci_prdt_entry_t prd;
        uint32_t chunk;
        uint32_t j;

        hype_ahci_decode_prdt_entry(prdt_bytes + (uint32_t)prd_idx * 16u, &prd);
        chunk = (prd.byte_count < remaining) ? prd.byte_count : remaining;

        if (is_write_direction) {
            const uint8_t *guest_src = (const uint8_t *)(uintptr_t)prd.data_phys;
            for (j = 0; j < chunk; j++) {
                dst_media[j] = guest_src[j];
            }
            dst_media += chunk;
        } else {
            uint8_t *guest_dst = (uint8_t *)(uintptr_t)prd.data_phys;
            for (j = 0; j < chunk; j++) {
                guest_dst[j] = src[j];
            }
            src += chunk;
        }
        remaining -= chunk;
        prd_idx++;
    }

    complete_ahci_command_slot0(ahci, rx_fis_phys, status_reg, error_reg);
    return 0;
}

int hype_svm_vcpu_handle_ahci_disk_npf(hype_vcpu_ctx_t *ctx, hype_ahci_t *ahci, hype_ata_disk_t *disk,
                                        uint64_t ahci_base_phys) {
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

    guest_bytes = (const uint8_t *)(uintptr_t)real->vmcb->save.rip;
    if (hype_mmio_decode(guest_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (decoded.is_write != npf.is_write) {
        return -1;
    }

    reg = gpr_ptr(real, decoded.reg);
    if (reg == 0) {
        return -1;
    }

    if (decoded.is_write) {
        uint32_t value = hype_mmio_extract_write_value(*reg, decoded.size_bytes);
        if (hype_ahci_mmio_write(ahci, offset, decoded.size_bytes, value) != 0) {
            return -1;
        }
        if (offset == HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CI && (ahci->p_ci & 0x1u) != 0) {
            if (process_ahci_ata_command_slot0(ahci, disk) != 0) {
                return -1;
            }
        }
    } else {
        uint32_t value = 0;
        if (hype_ahci_mmio_read(ahci, offset, decoded.size_bytes, &value) != 0) {
            return -1;
        }
        *reg = hype_mmio_merge_read_value(*reg, value, decoded.size_bytes, decoded.zero_extend);
    }

    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

int hype_svm_vcpu_handle_debug_port_ioio(hype_vcpu_ctx_t *ctx, uint16_t base_port, uint8_t *out_byte) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_ioio_t io;
    int is_write;

    hype_svm_decode_ioio_info1(real->vmcb->control.exitinfo1, &io);
    if (io.port != base_port) {
        return -1;
    }

    is_write = !io.is_in;
    if (io.is_in) {
        /* 0xE9 = the QEMU/bochs debug-port presence signature OVMF's
         * PlatformDebugLibIoPort checks before enabling the channel. */
        real->vmcb->save.rax = (real->vmcb->save.rax & ~0xFFULL) | 0xE9u;
    } else {
        *out_byte = (uint8_t)(real->vmcb->save.rax & 0xFFu);
    }

    real->vmcb->save.rip = real->vmcb->control.exitinfo2;
    return is_write ? 0 : 1;
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
        hype_guest_uart_write(uart, offset, (uint8_t)(real->vmcb->save.rax & 0xFFu));
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

    reg = gpr_ptr(real, decoded.reg);
    if (reg == 0) {
        return -1;
    }

    hype_pci_decode_ecam_offset(npf.guest_phys_addr - ecam_base_phys, &addr);

    if (decoded.is_write) {
        uint32_t value = hype_mmio_extract_write_value(*reg, decoded.size_bytes);
        hype_pci_config_write(pci, &addr, decoded.size_bytes, value);
    } else {
        uint32_t value = 0;
        hype_pci_config_read(pci, &addr, decoded.size_bytes, &value);
        *reg = hype_mmio_merge_read_value(*reg, value, decoded.size_bytes, decoded.zero_extend);
    }

    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

int hype_svm_vcpu_handle_bochs_vbe_npf(hype_vcpu_ctx_t *ctx, hype_bochs_vbe_t *dev,
                                        uint64_t mmio_base_phys) {
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

    guest_bytes = (const uint8_t *)(uintptr_t)real->vmcb->save.rip;
    if (hype_mmio_decode(guest_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (decoded.is_write != npf.is_write) {
        return -1;
    }
    if (decoded.size_bytes != 2u) {
        return -1; /* DISPI registers are architecturally 16-bit only */
    }

    reg = gpr_ptr(real, decoded.reg);
    if (reg == 0) {
        return -1;
    }

    if (offset < HYPE_BOCHS_VBE_DISPI_OFFSET || offset >= HYPE_BOCHS_VBE_DISPI_OFFSET + HYPE_BOCHS_VBE_DISPI_SIZE) {
        /* Reserved area of the MMIO BAR -- reads as 0, writes ignored,
         * same convention devices/ahci.h's own MMIO model uses. */
        if (!decoded.is_write) {
            *reg = hype_mmio_merge_read_value(*reg, 0, decoded.size_bytes, decoded.zero_extend);
        }
        real->vmcb->save.rip += decoded.instr_len;
        return 0;
    }

    if (decoded.is_write) {
        uint32_t value = hype_mmio_extract_write_value(*reg, decoded.size_bytes);
        if (hype_bochs_vbe_mmio_write(dev, offset - HYPE_BOCHS_VBE_DISPI_OFFSET, (uint16_t)value) != 0) {
            return -1;
        }
    } else {
        uint16_t value = 0;
        if (hype_bochs_vbe_mmio_read(dev, offset - HYPE_BOCHS_VBE_DISPI_OFFSET, &value) != 0) {
            return -1;
        }
        *reg = hype_mmio_merge_read_value(*reg, value, decoded.size_bytes, decoded.zero_extend);
    }

    real->vmcb->save.rip += decoded.instr_len;
    return 0;
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

    reg = gpr_ptr(real, decoded.reg);
    if (reg == 0) {
        return -1;
    }

    if (decoded.is_write) {
        uint32_t value = hype_mmio_extract_write_value(*reg, decoded.size_bytes);
        if (hype_guest_lapic_write(lapic, offset, decoded.size_bytes, value) != 0) {
            return -1;
        }
    } else {
        uint32_t value = 0;
        if (hype_guest_lapic_read(lapic, offset, decoded.size_bytes, &value) != 0) {
            return -1;
        }
        *reg = hype_mmio_merge_read_value(*reg, value, decoded.size_bytes, decoded.zero_extend);
    }

    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

/*
 * Walks every newly-submitted chain in the (single) virtqueue since
 * this device's own last_avail_idx bookkeeping, processing each as a
 * virtio_blk_req: exactly 3 descriptors (header, one data segment,
 * status), a single-segment simplification (see hype_svm_vcpu_handle_
 * virtio_blk_npf()'s own doc comment for why). Returns -1 if a chain
 * doesn't have that exact shape (malformed guest request); 0 otherwise.
 */
static int process_virtio_blk_queue(hype_virtio_blk_t *dev, uint8_t *backing, uint64_t backing_bytes) {
    const uint8_t *avail_base = (const uint8_t *)(uintptr_t)dev->queue_driver;
    const uint8_t *desc_base = (const uint8_t *)(uintptr_t)dev->queue_desc;
    uint8_t *used_base = (uint8_t *)(uintptr_t)dev->queue_device;
    uint16_t avail_idx = (uint16_t)(avail_base[2] | (avail_base[3] << 8));

    while (dev->last_avail_idx != avail_idx) {
        uint16_t ring_index = (uint16_t)(dev->last_avail_idx % dev->queue_size);
        uint16_t head_desc =
            (uint16_t)(avail_base[4 + 2 * ring_index] | (avail_base[4 + 2 * ring_index + 1] << 8));
        uint8_t raw_desc[16];
        hype_virtq_desc_t header_desc, data_desc, status_desc;
        const uint8_t *hdr;
        uint32_t req_type;
        uint64_t sector;
        uint64_t byte_offset;
        uint8_t status_value;
        uint32_t used_len;
        uint16_t used_idx;
        uint16_t used_ring_index;
        uint32_t elem_off;
        uint32_t j;

        for (j = 0; j < 16u; j++) {
            raw_desc[j] = desc_base[(uint32_t)head_desc * 16u + j];
        }
        hype_virtq_decode_desc(raw_desc, &header_desc);
        if ((header_desc.flags & HYPE_VIRTQ_DESC_F_NEXT) == 0) {
            return -1;
        }
        for (j = 0; j < 16u; j++) {
            raw_desc[j] = desc_base[(uint32_t)header_desc.next * 16u + j];
        }
        hype_virtq_decode_desc(raw_desc, &data_desc);
        if ((data_desc.flags & HYPE_VIRTQ_DESC_F_NEXT) == 0) {
            return -1;
        }
        for (j = 0; j < 16u; j++) {
            raw_desc[j] = desc_base[(uint32_t)data_desc.next * 16u + j];
        }
        hype_virtq_decode_desc(raw_desc, &status_desc);
        if ((status_desc.flags & HYPE_VIRTQ_DESC_F_NEXT) != 0) {
            return -1; /* status must be the chain's last descriptor */
        }

        hdr = (const uint8_t *)(uintptr_t)header_desc.addr;
        req_type = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) |
                   ((uint32_t)hdr[3] << 24);
        sector = (uint64_t)hdr[8] | ((uint64_t)hdr[9] << 8) | ((uint64_t)hdr[10] << 16) |
                 ((uint64_t)hdr[11] << 24) | ((uint64_t)hdr[12] << 32) | ((uint64_t)hdr[13] << 40) |
                 ((uint64_t)hdr[14] << 48) | ((uint64_t)hdr[15] << 56);
        byte_offset = sector * HYPE_VIRTIO_BLK_SECTOR_SIZE;

        if (byte_offset + data_desc.len > backing_bytes) {
            status_value = HYPE_VIRTIO_BLK_S_IOERR;
            used_len = 1;
        } else if (req_type == HYPE_VIRTIO_BLK_T_OUT) {
            const uint8_t *src = (const uint8_t *)(uintptr_t)data_desc.addr;
            for (j = 0; j < data_desc.len; j++) {
                backing[byte_offset + j] = src[j];
            }
            status_value = HYPE_VIRTIO_BLK_S_OK;
            used_len = 1;
        } else if (req_type == HYPE_VIRTIO_BLK_T_IN) {
            uint8_t *dst = (uint8_t *)(uintptr_t)data_desc.addr;
            for (j = 0; j < data_desc.len; j++) {
                dst[j] = backing[byte_offset + j];
            }
            status_value = HYPE_VIRTIO_BLK_S_OK;
            used_len = data_desc.len + 1u;
        } else {
            status_value = HYPE_VIRTIO_BLK_S_UNSUPP;
            used_len = 1;
        }

        *(uint8_t *)(uintptr_t)status_desc.addr = status_value;

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
    }

    return 0;
}

int hype_svm_vcpu_handle_virtio_blk_npf(hype_vcpu_ctx_t *ctx, hype_virtio_blk_t *dev, uint8_t *backing,
                                         uint64_t backing_bytes, uint64_t mmio_base_phys) {
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

    guest_bytes = (const uint8_t *)(uintptr_t)real->vmcb->save.rip;
    if (hype_mmio_decode(guest_bytes, HYPE_MMIO_MAX_INSTR_BYTES, &decoded) != 0) {
        return -1;
    }
    if (decoded.is_write != npf.is_write) {
        return -1;
    }

    reg = gpr_ptr(real, decoded.reg);
    if (reg == 0) {
        return -1;
    }

    if (offset >= HYPE_VIRTIO_BLK_BAR_COMMON_CFG_OFFSET &&
        offset < HYPE_VIRTIO_BLK_BAR_COMMON_CFG_OFFSET + HYPE_VIRTIO_COMMON_CFG_SIZE) {
        uint32_t region_offset = offset - HYPE_VIRTIO_BLK_BAR_COMMON_CFG_OFFSET;
        if (decoded.is_write) {
            uint32_t value = hype_mmio_extract_write_value(*reg, decoded.size_bytes);
            if (hype_virtio_blk_common_cfg_write(dev, region_offset, decoded.size_bytes, value) != 0) {
                return -1;
            }
        } else {
            uint32_t value = 0;
            if (hype_virtio_blk_common_cfg_read(dev, region_offset, decoded.size_bytes, &value) != 0) {
                return -1;
            }
            *reg = hype_mmio_merge_read_value(*reg, value, decoded.size_bytes, decoded.zero_extend);
        }
    } else if (offset >= HYPE_VIRTIO_BLK_BAR_NOTIFY_CFG_OFFSET &&
               offset < HYPE_VIRTIO_BLK_BAR_NOTIFY_CFG_OFFSET + 4u) {
        if (decoded.is_write) {
            if (hype_virtio_blk_is_queue_ready(dev)) {
                if (process_virtio_blk_queue(dev, backing, backing_bytes) != 0) {
                    return -1;
                }
            }
        } else {
            *reg = hype_mmio_merge_read_value(*reg, 0, decoded.size_bytes, decoded.zero_extend);
        }
    } else if (offset == HYPE_VIRTIO_BLK_BAR_ISR_CFG_OFFSET) {
        if (!decoded.is_write) {
            uint8_t value = hype_virtio_blk_isr_read(dev);
            *reg = hype_mmio_merge_read_value(*reg, value, decoded.size_bytes, decoded.zero_extend);
        }
    } else if (offset >= HYPE_VIRTIO_BLK_BAR_DEVICE_CFG_OFFSET &&
               offset < HYPE_VIRTIO_BLK_BAR_DEVICE_CFG_OFFSET + HYPE_VIRTIO_BLK_CFG_SIZE) {
        if (!decoded.is_write) {
            uint32_t value = 0;
            uint32_t region_offset = offset - HYPE_VIRTIO_BLK_BAR_DEVICE_CFG_OFFSET;
            if (hype_virtio_blk_device_cfg_read(dev, region_offset, decoded.size_bytes, &value) != 0) {
                return -1;
            }
            *reg = hype_mmio_merge_read_value(*reg, value, decoded.size_bytes, decoded.zero_extend);
        }
    } else {
        /* Reserved area of the MMIO BAR -- reads as 0, writes ignored. */
        if (!decoded.is_write) {
            *reg = hype_mmio_merge_read_value(*reg, 0, decoded.size_bytes, decoded.zero_extend);
        }
    }

    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}

int hype_svm_vcpu_handle_fw_cfg_ioio(hype_vcpu_ctx_t *ctx, hype_fw_cfg_t *fw) {
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
            return -1; /* no writable fw_cfg files in this project's scope */
        }
        real->vmcb->save.rax = (real->vmcb->save.rax & ~0xFFULL) | hype_fw_cfg_read_byte(fw);
    } else if (io.port == 0x514u) {
        if (io.is_in) {
            return -1;
        }
        hype_fw_cfg_dma_addr_high(fw, (uint32_t)(real->vmcb->save.rax & 0xFFFFFFFFu));
    } else if (io.port == 0x518u) {
        uint64_t access_phys;
        uint8_t raw[16];
        hype_fw_cfg_dma_op_t op;
        uint8_t *guest_data;
        uint8_t *control_bytes;
        uint32_t result;
        int i;

        if (io.is_in) {
            return -1;
        }

        access_phys = hype_fw_cfg_dma_addr_low(fw, (uint32_t)(real->vmcb->save.rax & 0xFFFFFFFFu));

        for (i = 0; i < 16; i++) {
            raw[i] = ((const uint8_t *)(uintptr_t)access_phys)[i];
        }
        hype_fw_cfg_dma_decode(raw, &op);

        guest_data = (uint8_t *)(uintptr_t)op.address;
        result = hype_fw_cfg_dma_execute(fw, &op, guest_data);

        control_bytes = (uint8_t *)(uintptr_t)access_phys;
        control_bytes[0] = (uint8_t)(result >> 24);
        control_bytes[1] = (uint8_t)(result >> 16);
        control_bytes[2] = (uint8_t)(result >> 8);
        control_bytes[3] = (uint8_t)result;
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
    clgi();
    vmload(vmcb_phys);
    vmrun_full(vmcb_phys);
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

    reg = gpr_ptr(real, decoded.reg);
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

    /* Same "next-RIP-for-free" convenience as HLT/IOIO, just sourced
     * from the decoder's own computed instruction length instead of an
     * EXITINFO2 the hardware doesn't provide for NPF (EXITINFO2 there
     * is the faulting *address*, already consumed above). */
    real->vmcb->save.rip += decoded.instr_len;
    return 0;
}
