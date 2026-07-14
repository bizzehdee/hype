#include "svm.h"

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

hype_vcpu_ctx_t *hype_svm_vcpu_create(uint64_t guest_rip, uint64_t guest_rsp, uint64_t ept_or_npt_root) {
    unsigned i;

    /* CPUMSR-2: this guest now sets HYPE_SVM_INTERCEPT_MSR_PROT
     * (hype_vmcb_build_realmode_guest()) -- same "the bit only enables
     * interception, the bitmap decides per-MSR" reasoning as
     * hype_svm_vcpu_create_long_mode()'s own g_iopm fill below. All-
     * zero (BSS default) would mean "intercept nothing," letting every
     * guest RDMSR/WRMSR reach real hardware directly despite the
     * intercept bit being set. */
    for (i = 0; i < sizeof(g_msrpm); i++) {
        g_msrpm[i] = 0xFFu;
    }

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
     * HYPE_SVM_INTERCEPT_MSR_PROT. */
    for (i = 0; i < sizeof(g_msrpm); i++) {
        g_msrpm[i] = 0xFFu;
    }

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

int hype_svm_vcpu_handle_msr(hype_vcpu_ctx_t *ctx) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    int is_write = (real->vmcb->control.exitinfo1 & 0x1ULL) != 0;
    uint32_t msr_number = (uint32_t)real->gprs[1]; /* RCX */
    hype_msr_action_t action = hype_msr_decide(msr_number, is_write);

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
static int process_ahci_command_slot0(hype_ahci_t *ahci, hype_atapi_t *atapi) {
    uint64_t cmd_list_phys = (uint64_t)ahci->p_clb | ((uint64_t)ahci->p_clbu << 32);
    uint64_t rx_fis_phys = (uint64_t)ahci->p_fb | ((uint64_t)ahci->p_fbu << 32);
    const uint8_t *cmd_hdr_bytes = (const uint8_t *)(uintptr_t)cmd_list_phys;
    hype_ahci_cmd_header_t hdr;
    const uint8_t *cmd_table_bytes;
    const uint8_t *prdt_bytes;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];
    hype_atapi_result_t result;
    const uint8_t *src;
    uint32_t remaining;
    uint32_t prd_idx;
    uint8_t status_reg;
    uint8_t error_reg;
    uint8_t *d2h_fis;
    unsigned i;

    hype_ahci_decode_cmd_header(cmd_hdr_bytes, &hdr);
    if (!hdr.is_atapi) {
        return -1;
    }

    cmd_table_bytes = (const uint8_t *)(uintptr_t)hdr.cmd_table_phys;
    if (cmd_table_bytes[0] != 0x27u || cmd_table_bytes[2] != 0xA0u) {
        return -1; /* not a Register H2D FIS carrying ATA_CMD_PACKET (0xA0) */
    }
    for (i = 0; i < HYPE_ATAPI_CDB_MAX; i++) {
        cdb[i] = cmd_table_bytes[0x40 + i];
    }

    hype_atapi_execute_cdb(atapi, cdb, &result);

    src = result.uses_media_data ? (atapi->media_data + result.media_offset) : result.synth_data;
    remaining = result.uses_media_data ? result.media_length : result.synth_length;

    prdt_bytes = cmd_table_bytes + 0x80;
    prd_idx = 0;
    while (remaining > 0 && prd_idx < hdr.prdtl) {
        hype_ahci_prdt_entry_t prd;
        uint32_t chunk;
        uint8_t *dst;
        uint32_t j;

        hype_ahci_decode_prdt_entry(prdt_bytes + (uint32_t)prd_idx * 16u, &prd);
        chunk = (prd.byte_count < remaining) ? prd.byte_count : remaining;
        dst = (uint8_t *)(uintptr_t)prd.data_phys;
        for (j = 0; j < chunk; j++) {
            dst[j] = src[j];
        }
        src += chunk;
        remaining -= chunk;
        prd_idx++;
    }

    /* ATA STATUS register: DRDY|DSC always, +ERR on CHECK_CONDITION.
     * ATAPI convention: a failed PACKET command's ERROR register
     * carries the SCSI sense key in its upper nibble. */
    status_reg = (result.status == HYPE_ATAPI_STATUS_GOOD) ? 0x50u : 0x51u;
    error_reg = (result.status == HYPE_ATAPI_STATUS_GOOD) ? 0u : (uint8_t)(atapi->sense_key << 4);
    ahci->p_tfd = (uint32_t)status_reg | ((uint32_t)error_reg << 8);

    d2h_fis = (uint8_t *)(uintptr_t)(rx_fis_phys + 0x40);
    for (i = 0; i < 20; i++) {
        d2h_fis[i] = 0;
    }
    d2h_fis[0] = 0x34; /* FIS type: Register - Device to Host */
    d2h_fis[2] = status_reg;
    d2h_fis[3] = error_reg;

    ahci->p_ci &= ~0x1u;     /* slot 0 complete */
    ahci->p_is |= (1u << 6); /* D2H Register FIS interrupt bit -- spec-completeness; no real
                              * interrupt delivery wired up in this milestone (polling guest
                              * driver, same as fw_cfg's own DMA test guest). */
    return 0;
}

int hype_svm_vcpu_handle_ahci_npf(hype_vcpu_ctx_t *ctx, hype_ahci_t *ahci, hype_atapi_t *atapi,
                                   uint64_t ahci_base_phys) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_npf_t npf;
    hype_mmio_decode_t decoded;
    uint64_t *reg;
    uint32_t offset;
    const uint8_t *guest_bytes;

    hype_svm_decode_npf_info(real->vmcb->control.exitinfo1, real->vmcb->control.exitinfo2, &npf);

    if (npf.guest_phys_addr < ahci_base_phys) {
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
            if (process_ahci_command_slot0(ahci, atapi) != 0) {
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

int hype_svm_vcpu_handle_pci_ecam_npf(hype_vcpu_ctx_t *ctx, hype_pci_t *pci, uint64_t ecam_base_phys) {
    struct hype_vcpu_ctx *real = (struct hype_vcpu_ctx *)ctx;
    hype_svm_npf_t npf;
    hype_mmio_decode_t decoded;
    hype_pci_ecam_addr_t addr;
    uint64_t *reg;
    const uint8_t *guest_bytes;

    hype_svm_decode_npf_info(real->vmcb->control.exitinfo1, real->vmcb->control.exitinfo2, &npf);

    /* Both bounds matter, not just the lower one: PCI-2 introduces a
     * second NPT-trapped region (a device's own dynamically-BAR-
     * programmed MMIO window) that could otherwise be mistaken for an
     * ECAM access if this only checked "at or past the base." */
    if (npf.guest_phys_addr < ecam_base_phys ||
        npf.guest_phys_addr >= ecam_base_phys + HYPE_PCI_ECAM_BUS0_SIZE) {
        return -1;
    }

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
    hype_debug_print("svm: about to CLGI/VMLOAD/VMRUN (vmcb_phys=0x%llx)...\n",
                      (unsigned long long)vmcb_phys);
    clgi();
    vmload(vmcb_phys);
    vmrun_full(vmcb_phys);
    hype_debug_print("svm: VMRUN returned -- about to VMSAVE/STGI...\n");
    vmsave(vmcb_phys);
    stgi();
    hype_debug_print("svm: STGI done, exitcode=0x%llx\n",
                      (unsigned long long)real->vmcb->control.exitcode);

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
