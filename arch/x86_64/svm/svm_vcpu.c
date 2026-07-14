#include "svm.h"

/*
 * Concrete per-vCPU context for the SVM backend (M2-7). Opaque outside
 * this file per vmm_ops.h's hype_vcpu_ctx_t contract -- the dispatch
 * loop and device model only ever see the pointer. Single static
 * instance: M2's scope is one vCPU (the hand-written M2-7 test guest);
 * real multi-vCPU allocation is M8's job.
 */
struct hype_vcpu_ctx {
    hype_vmcb_t *vmcb;
    /* Not a VMCB field -- VMRUN only loads/saves RAX/RSP/RIP/RFLAGS
     * from the VMCB; every other GPR (RSI included) just passes
     * through whatever the host had at VMRUN time. Loaded into the
     * real RSI register immediately before VMRUN (see vmrun() below),
     * so a guest can rely on it at entry the way the Linux boot
     * protocol's RSI=zero-page-address convention requires (M3-5). */
    uint64_t initial_rsi;
};

static hype_vmcb_t g_vmcb __attribute__((aligned(4096)));
static struct hype_vcpu_ctx g_ctx;

/* AMD SDM: 12KB I/O permission map, 8KB MSR permission map -- VMRUN
 * always consults both, for every guest, regardless of whether it
 * ever executes I/O or RDMSR/WRMSR/etc. All-zero (this array's default,
 * BSS-zeroed) means "intercept nothing," correct for the real-mode
 * guest (hype_svm_vcpu_create() below), which never sets
 * HYPE_SVM_INTERCEPT_IOIO_PROT and so never consults this bitmap at
 * all; hype_svm_vcpu_create_long_mode() (M3-5), which does set that
 * bit, fills every byte with 0xFF first (see its own comment) so
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
 * general-purpose register before the next #VMEXIT -- not just
 * RAX/RSI (the two given as explicit operands here), which VMCB
 * fields and the boot-protocol RSI convention respectively care
 * about. A plain input-only constraint ("a"(x), "S"(x)) does not by
 * itself tell the compiler those registers are clobbered by the
 * instruction -- without saying so, the compiler could keep some
 * *other* live C value (e.g. the caller's `real` pointer) in one of
 * the registers guest code actually stomps, silently corrupting it
 * once the guest runs. Read-write ("+a"/"+S") constraints bound to
 * throwaway locals mark RAX/RSI as genuinely clobbered without
 * conflicting with their use as inputs; every other GPR guest code
 * could reach is clobbered explicitly. Confirmed the hard way: this
 * exact gap caused `real->vmcb->control.exitcode` to read
 * plausible-looking garbage instead of a real SVM exit code, once the
 * guest actually ran code that touched RSI.
 */
static inline void vmrun(uint64_t vmcb_phys, uint64_t initial_rsi) {
    uint64_t clobbered_rax = vmcb_phys;
    uint64_t clobbered_rsi = initial_rsi;
    __asm__ volatile("vmrun %%rax"
                      : "+a"(clobbered_rax), "+S"(clobbered_rsi)
                      :
                      : "memory", "cc", "rbx", "rcx", "rdx", "rdi", "rbp", "r8", "r9", "r10", "r11",
                        "r12", "r13", "r14", "r15");
}

static inline void vmsave(uint64_t vmcb_phys) {
    __asm__ volatile("vmsave %%rax" : : "a"(vmcb_phys) : "memory");
}

hype_vcpu_ctx_t *hype_svm_vcpu_create(uint64_t guest_rip, uint64_t guest_rsp, uint64_t ept_or_npt_root) {
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
    g_ctx.initial_rsi = 0;
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

    hype_vmcb_build_long_mode_guest(&g_vmcb, entry_rip, guest_cr3, rsp, (uint64_t)(uintptr_t)g_iopm,
                                     (uint64_t)(uintptr_t)g_msrpm);

    if (npt_root != 0) {
        hype_vmcb_enable_nested_paging(&g_vmcb, npt_root);
    }

    g_ctx.vmcb = &g_vmcb;
    g_ctx.initial_rsi = 0;
    return &g_ctx;
}

void hype_svm_vcpu_set_rsi(hype_vcpu_ctx_t *ctx, uint64_t rsi) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    real->initial_rsi = rsi;
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

void hype_svm_vcpu_enable_apic_accel_ops(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_vcpu_enable_apic_accel(real->vmcb);
}

int hype_svm_vcpu_run(hype_vcpu_ctx_t *ctx, hype_vmexit_info_t *info) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    uint64_t vmcb_phys = (uint64_t)(uintptr_t)real->vmcb;

    clgi();
    vmload(vmcb_phys);
    vmrun(vmcb_phys, real->initial_rsi);
    vmsave(vmcb_phys);
    stgi();

    info->reason = real->vmcb->control.exitcode;
    info->qualification = real->vmcb->control.exitinfo1;
    info->guest_rip = real->vmcb->save.rip;

    return (info->reason == HYPE_SVM_EXITCODE_INVALID) ? -1 : 0;
}
