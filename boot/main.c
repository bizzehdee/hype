#include "../core/efi_types.h"
#include "../core/console.h"
#include "../core/fatal.h"
#include "../core/gop.h"
#include "../core/gop_text.h"
#include "../core/halt.h"
#include "../core/memmap.h"
#include "../core/serial.h"
#include "../core/guest_ram.h"
#include "../core/mp.h"
#include "../arch/x86_64/cpu/cpu_features.h"
#include "../arch/x86_64/cpu/gdt.h"
#include "../arch/x86_64/cpu/idt.h"
#include "../arch/x86_64/cpu/isr.h"
#include "../arch/x86_64/cpu/lapic.h"
#include "../arch/x86_64/cpu/paging.h"
#include "../arch/x86_64/cpu/pic.h"
#include "../arch/x86_64/cpu/pit.h"
#include "../arch/x86_64/cpu/timer.h"
#include "../arch/x86_64/cpu/vmexit.h"
#include "../arch/x86_64/cpu/vmm_select.h"
#include "../arch/x86_64/svm/npt.h"
#include "../arch/x86_64/svm/svm.h"
#include "../core/linux_boot.h"
#include "../devices/pic.h"
#include "../devices/pit.h"
#include "../devices/pflash.h"

/* Static storage: still valid (and unmoving) once these get built and
 * loaded, after ExitBootServices() below. */
static hype_gdt_entry_t g_gdt[HYPE_GDT_ENTRY_COUNT];
static hype_idt_entry_t g_idt[HYPE_IDT_ENTRY_COUNT];
static hype_pte_t g_pml4[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_pte_t g_pdpt[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_pte_t g_pd[HYPE_PAGING_MAX_GB][HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_gop_console_t g_gop_console;

/* M2-7: the hand-written test guest's code+stack pages -- wherever
 * the linker/loader actually placed them; hype_svm_vcpu_create()
 * points the guest's CS.base/SS.base directly at their address (see
 * vmcb.h), so no particular alignment or address range is required. */
static uint8_t g_m2_7_guest_code[4096] __attribute__((aligned(4096)));
static uint8_t g_m2_7_guest_stack[4096] __attribute__((aligned(4096)));

/* M3-1: NPT identity map for the same test guest, built fresh on
 * every (re)start like everything else here. */
static hype_pte_t g_npt_pml4[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_pte_t g_npt_pdpt[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_pte_t g_npt_pd[HYPE_NPT_MAX_GB][HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));

/* M3-5: guest identity page tables (the GUEST's own CR3) for the
 * long-mode Linux boot-protocol test guest -- distinct from both the
 * host's own paging (g_pml4 etc.) and NPT (g_npt_pml4 etc.): the
 * Linux boot protocol requires paging already enabled at 64-bit
 * entry, with the kernel/zero-page/stack range identity-mapped.
 * Reuses hype_paging_build_identity() directly (arch/x86_64/cpu/
 * paging.h) -- a ring-0-only guest CR3 needs no User/Supervisor bit,
 * unlike NPT (arch/x86_64/svm/npt.h). */
#define HYPE_M3_5_GUEST_PAGING_GB 4
static hype_pte_t g_guest_pml4[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_pte_t g_guest_pdpt[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_pte_t g_guest_pd[HYPE_M3_5_GUEST_PAGING_GB][HYPE_PAGING_ENTRIES_PER_TABLE]
    __attribute__((aligned(4096)));

/*
 * M3-5: a synthetic, hand-built "bzImage" -- a real setup_header
 * (parsed through core/linux_boot.h's shim, not bypassed) followed by
 * a tiny hand-written 64-bit payload standing in for a real kernel's
 * entry code. Same rigor/reasoning as M2-7's single-HLT-byte guest:
 * full control over the outcome, proving the new plumbing (guest
 * paging, long-mode VMCB, IOIO intercept -> device stubs) actually
 * works, before attempting a real, unpredictable kernel.
 */
static uint8_t g_m3_5_bzimage[4096] __attribute__((aligned(4096)));
static uint8_t g_m3_5_guest_stack[8192] __attribute__((aligned(4096)));
static hype_linux_boot_params_t g_m3_5_zero_page __attribute__((aligned(4096)));
static hype_pic_emu_t g_m3_5_pic;
static hype_pit_emu_t g_m3_5_pit;

/*
 * The hand-written payload standing in for a kernel's 64-bit entry
 * point: masks all IRQs on the emulated PIC (a guest OUT, exercising
 * devices/pic.h's write dispatch), latches and reads back PIT channel
 * 0 (a guest IN, exercising devices/pit.h's read dispatch), then
 * halts. Deliberately does NOT touch the serial port -- this project's
 * own guest-isolation invariant (AGENTS.md) means every port a guest
 * touches gets intercepted, serial included, and there is no emulated
 * serial device yet (M3-4's device list is PIC/IOAPIC/PIT/HPET only,
 * not serial) -- confirmed the hard way when an earlier version of
 * this payload's serial writes correctly triggered "unhandled port"
 * once IOIO interception actually started working. Verified
 * byte-for-byte against well-established, unambiguous x86_64 opcodes
 * (register-implicit MOV/IN/OUT/HLT forms -- no ModRM/SIB byte
 * complexity anywhere in this sequence).
 */
static const uint8_t g_m3_5_payload[] = {
    0xB0, 0xFF,             /* mov al, 0xff */
    0xE6, 0x21,             /* out 0x21, al -- PIC: mask all IRQs */
    0xB0, 0x00,             /* mov al, 0x00 */
    0xE6, 0x43,             /* out 0x43, al -- PIT: latch channel 0 */
    0xE4, 0x40,             /* in al, 0x40  -- PIT: read latched lobyte */
    0xE4, 0x40,             /* in al, 0x40  -- PIT: read latched hibyte */
    0xF4,                   /* hlt */
    0xEB, 0xFD              /* jmp $-3 (back to hlt, belt-and-braces) */
};

/*
 * M4-3: dedicated backing store + device state for the emulated CFI
 * flash (devices/pflash.h). Small on purpose -- this test only
 * exercises WRITE_BYTE and an array READ, not a full varstore image;
 * real persistence to a host file is explicitly deferred (M5's disk
 * driver doesn't exist yet -- see task.md's M4-3 scope note).
 */
static uint8_t g_m4_3_pflash_backing[4096] __attribute__((aligned(4096)));
static hype_pflash_t g_m4_3_pflash;
static uint8_t g_m4_3_guest_code[4096] __attribute__((aligned(4096)));
static uint8_t g_m4_3_guest_stack[4096] __attribute__((aligned(4096)));

/* Guest-physical address the emulated flash is mapped at: 3GB,
 * comfortably inside the 4GB NPT/guest identity map this test guest
 * reuses (HYPE_NPT_MAX_GB/HYPE_M3_5_GUEST_PAGING_GB, both 4) and
 * nowhere near any real static buffer this project actually uses --
 * marking its covering 2MB NPT entry not-present
 * (hype_npt_mark_not_present()) can't collide with anything real, and
 * since that marking is what makes the guest's access fault into
 * hype_svm_vcpu_handle_npf() in the first place (entirely
 * software-emulated from there -- the underlying "physical" address is
 * never actually touched), it does not matter whether QEMU's
 * configured RAM even reaches 3GB. */
#define HYPE_M4_3_PFLASH_GPA (3ULL * HYPE_PAGING_1GB)

/*
 * M4-3's hand-written MMIO test payload: issues a real CFI WRITE_BYTE
 * command (0x10) and data byte through genuine memory-mapped stores at
 * HYPE_M4_3_PFLASH_GPA, reads the byte back through a genuine
 * memory-mapped load (exercising the read-side NPF/decode/dispatch
 * path, not just the write side), then re-issues WRITE_BYTE at a
 * second offset (+0x100) with the exact value just read back -- so the
 * host can confirm BOTH directions worked from the pflash backing
 * array alone: backing[0] == 0xAB proves the write path;
 * backing[0x100] == 0xAB proves the read genuinely delivered 0xAB into
 * CL (not some stale/garbage value) and that value made it back out
 * through a second write. Every instruction here is one of the exact
 * forms hype_mmio_decode() supports (already unit-tested in isolation,
 * core/tests/test_mmio_decode.c) -- verified byte-for-byte against the
 * AMD64 opcode tables, same rigor as every other hand-written test
 * payload in this file. The two 8-byte immediate fields (zeroed here)
 * are patched at runtime (see HYPE_M4_3_PAYLOAD_*_IMM_OFFSET below) --
 * simpler and less error-prone than hand-transcribing
 * HYPE_M4_3_PFLASH_GPA's little-endian encoding into a byte literal.
 *
 *   mov rbx, <patched>   48 BB 00 00 00 00 00 00 00 00
 *   mov rdx, <patched>   48 BA 00 00 00 00 00 00 00 00
 *   mov al, 0x10         B0 10
 *   mov [rbx], al        88 03   (issue WRITE_BYTE at offset 0)
 *   mov al, 0xAB         B0 AB
 *   mov [rbx], al        88 03   (write data byte 0xAB at offset 0)
 *   mov cl, [rbx]        8A 0B   (read it back into CL)
 *   mov al, 0x10         B0 10
 *   mov [rdx], al        88 02   (issue WRITE_BYTE at offset 0x100)
 *   mov [rdx], cl        88 0A   (write CL's value at offset 0x100)
 *   hlt                  F4
 *   jmp $-3              EB FD  (belt-and-braces, matching M3-5's payload)
 */
static const uint8_t g_m4_3_payload_template[] = {
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x48, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB0, 0x10,
    0x88, 0x03,
    0xB0, 0xAB,
    0x88, 0x03,
    0x8A, 0x0B,
    0xB0, 0x10,
    0x88, 0x02,
    0x88, 0x0A,
    0xF4,
    0xEB, 0xFD
};
#define HYPE_M4_3_PAYLOAD_RBX_IMM_OFFSET 2
#define HYPE_M4_3_PAYLOAD_RDX_IMM_OFFSET 12

/* Writes `value` little-endian into dst[0..7] -- avoids an unaligned
 * uint64_t* store (dst is not necessarily 8-byte aligned within the
 * guest code buffer), matching this file's existing byte-at-a-time
 * conventions elsewhere. */
static void hype_write_le64(unsigned char *dst, uint64_t value) {
    int i;
    for (i = 0; i < 8; i++) {
        dst[i] = (unsigned char)(value >> (8 * i));
    }
}

typedef struct {
    const hype_vmm_ops_t *ops;
    hype_vmm_kind_t kind;
} hype_test_guest_args_t;

/*
 * M2-7/M3-1's test-guest launch, factored out so it can run either
 * inline on the BSP (no extra pCPU available) or dispatched onto a
 * pinned AP (M3-2, see efi_main). Nothing here depends on our own
 * GDT/IDT/paging being active -- RDMSR/WRMSR, VMLOAD/VMRUN/VMSAVE/
 * CLGI/STGI, and struct-filling are all self-contained under whatever
 * valid environment is currently active, which matters because the
 * AP dispatch below runs this *before* ExitBootServices (see the
 * comment at the StartupThisAP call for why).
 */
static void EFIAPI run_test_guest(void *arg) {
    hype_test_guest_args_t *args = (hype_test_guest_args_t *)arg;
    const hype_vmm_ops_t *ops = args->ops;
    hype_vmm_kind_t kind = args->kind;

    if (ops->enable() != 0) {
        hype_fatal("vmm: %s enable failed", ops->name);
    }
    hype_serial_print("vmm: %s enabled\n", ops->name);

    /*
     * VMX's vcpu_create/vcpu_run stay NULL past M2-7 (see vmx_ops.c)
     * -- only SVM actually launches here; VMX's equivalent is
     * deferred to M2-8's real Intel hardware pass.
     */
    if (ops->vcpu_create == 0 || ops->vcpu_run == 0) {
        hype_serial_print("vmm: %s vCPU launch not implemented yet -- test guest skipped\n", ops->name);
        return;
    }

    /* M2-6 hard invariant: zero every byte of a guest's reserved RAM
     * before its first VM-entry, on every (re)start -- not just the
     * bytes we're about to write ourselves. */
    hype_guest_ram_zero(g_m2_7_guest_code, sizeof(g_m2_7_guest_code));
    hype_guest_ram_zero(g_m2_7_guest_stack, sizeof(g_m2_7_guest_stack));
    g_m2_7_guest_code[0] = 0xF4; /* HLT */

    uint64_t entry_phys = (uint64_t)(uintptr_t)g_m2_7_guest_code;
    uint64_t stack_phys = (uint64_t)(uintptr_t)(g_m2_7_guest_stack + sizeof(g_m2_7_guest_stack));
    hype_serial_print("vmm: %s test guest: entry_phys=0x%llx stack_phys=0x%llx\n", ops->name,
                       (unsigned long long)entry_phys, (unsigned long long)stack_phys);

    /* M3-1: SVM's NPT is real and QEMU-validated (unlike EPT, which
     * has nowhere to be wired in yet -- VMX's vcpu_create stays NULL,
     * see above). Building it fresh for every (re)start, same as
     * everything else here. */
    uint64_t npt_root_phys = 0;
    if (kind == HYPE_VMM_KIND_SVM) {
        hype_npt_build_identity(g_npt_pml4, g_npt_pdpt, g_npt_pd, HYPE_NPT_MAX_GB);
        npt_root_phys = (uint64_t)(uintptr_t)g_npt_pml4;
        hype_serial_print("vmm: %s NPT identity map built (root=0x%llx, %u GB)\n", ops->name,
                           (unsigned long long)npt_root_phys, HYPE_NPT_MAX_GB);
    }

    hype_vcpu_ctx_t *ctx = ops->vcpu_create(entry_phys, stack_phys, npt_root_phys);
    if (ctx == 0) {
        hype_fatal("vmm: %s vcpu_create failed", ops->name);
    }

    hype_vmexit_info_t info;
    int rc = hype_vmexit_dispatch_loop(ops, ctx, kind, &info);
    if (rc != 0) {
        hype_fatal("vmm: %s test guest did not exit cleanly (reason=0x%llx qual=0x%llx)", ops->name,
                   (unsigned long long)info.reason, (unsigned long long)info.qualification);
    }
    hype_serial_print("vmm: %s test guest halted cleanly (reason=0x%llx, guest_rip=0x%llx)\n", ops->name,
                       (unsigned long long)info.reason, (unsigned long long)info.guest_rip);
}

/*
 * M3-5: builds the synthetic bzImage (real setup_header validated
 * through core/linux_boot.h's shim, not bypassed), builds guest
 * identity page tables, launches the long-mode test guest, and runs a
 * real VM-exit loop that keeps resuming the guest across IOIO exits
 * (routed to devices/pic.h and devices/pit.h) until it halts. SVM-only
 * -- VMX's vcpu_create/vcpu_run stay NULL past M2-7 (vmx_ops.c), same
 * as the M2-7/M3-1/M3-2 test guest above.
 */
static void run_m3_5_linux_shim_test(const hype_vmm_ops_t *ops, hype_vmm_kind_t kind) {
    hype_linux_setup_header_t hdr;
    unsigned char *hdr_bytes = (unsigned char *)&hdr;
    unsigned long long i;
    unsigned char *img;
    hype_linux_setup_header_t *img_hdr;
    uint32_t payload_offset;
    unsigned char *payload_at;
    uint64_t payload_load_address, entry_rip, guest_cr3, rsp, rsi;
    hype_vcpu_ctx_t *ctx;
    hype_vmexit_info_t info;

    if (kind != HYPE_VMM_KIND_SVM) {
        hype_serial_print("m3-5: skipped -- %s has no working vcpu_run yet (see vmx_ops.c)\n", ops->name);
        return;
    }

    /* M2-6 hard invariant: zero every byte of this guest's reserved
     * RAM before its first VM-entry, on every (re)start. */
    hype_guest_ram_zero(g_m3_5_bzimage, sizeof(g_m3_5_bzimage));
    hype_guest_ram_zero(g_m3_5_guest_stack, sizeof(g_m3_5_guest_stack));
    hype_guest_ram_zero(&g_m3_5_zero_page, sizeof(g_m3_5_zero_page));

    for (i = 0; i < sizeof(hdr); i++) {
        hdr_bytes[i] = 0;
    }
    hdr.setup_sects = 4; /* real-mode/setup region = (4+1)*512 = 2560 bytes */
    hdr.boot_flag = HYPE_LINUX_BOOT_FLAG;
    hdr.header = HYPE_LINUX_HDR_MAGIC;
    hdr.version = 0x020Fu;
    hdr.xloadflags = HYPE_LINUX_XLF_KERNEL_64;

    if (!hype_linux_header_is_valid(&hdr)) {
        hype_fatal("m3-5: synthetic setup header failed its own validity check");
    }

    /* Write the header into the synthetic bzImage buffer at its real
     * file offset -- exactly where a real loader would find it, not a
     * shortcut around the shim being tested. */
    img = g_m3_5_bzimage;
    img_hdr = (hype_linux_setup_header_t *)(img + HYPE_LINUX_SETUP_HEADER_OFFSET);
    *img_hdr = hdr;

    payload_offset = hype_linux_payload_file_offset(&hdr);
    payload_at = img + payload_offset;
    payload_load_address = (uint64_t)(uintptr_t)payload_at;
    entry_rip = hype_linux_64bit_entry(payload_load_address);

    /* The 64-bit entry point is payload_load_address + 0x200, not
     * payload_load_address itself (hype_linux_64bit_entry()) -- write
     * the hand-written test payload AT the entry point, not at the
     * start of the payload region a few hundred bytes before it. */
    for (i = 0; i < sizeof(g_m3_5_payload); i++) {
        ((unsigned char *)(uintptr_t)entry_rip)[i] = g_m3_5_payload[i];
    }

    hype_pic_emu_reset(&g_m3_5_pic);
    hype_pit_emu_reset(&g_m3_5_pit);

    hype_paging_build_identity(g_guest_pml4, g_guest_pdpt, g_guest_pd, HYPE_M3_5_GUEST_PAGING_GB);
    guest_cr3 = (uint64_t)(uintptr_t)g_guest_pml4;

    rsp = (uint64_t)(uintptr_t)(g_m3_5_guest_stack + sizeof(g_m3_5_guest_stack));
    rsi = (uint64_t)(uintptr_t)&g_m3_5_zero_page;

    hype_serial_print("m3-5: entry_rip=0x%llx guest_cr3=0x%llx zero_page=0x%llx\n",
                       (unsigned long long)entry_rip, (unsigned long long)guest_cr3,
                       (unsigned long long)rsi);

    /* No NPT for this first pass (0) -- see task.md's M3-5 scope note
     * on why full AVIC interrupt-delivery validation is deferred. */
    ctx = hype_svm_vcpu_create_long_mode(entry_rip, guest_cr3, rsp, 0);
    if (ctx == 0) {
        hype_fatal("m3-5: vcpu_create_long_mode failed");
    }
    hype_svm_vcpu_set_rsi(ctx, rsi);

    for (;;) {
        if (ops->vcpu_run(ctx, &info) != 0) {
            hype_fatal("m3-5: VM-entry failed (reason=0x%llx)", (unsigned long long)info.reason);
        }

        if (info.reason == HYPE_SVM_EXITCODE_IOIO) {
            if (hype_svm_vcpu_handle_ioio(ctx, &g_m3_5_pic, &g_m3_5_pit) != 0) {
                hype_fatal("m3-5: unhandled guest port I/O (qual=0x%llx)",
                           (unsigned long long)info.qualification);
            }
            continue;
        }

        break;
    }

    if (info.reason != HYPE_SVM_EXITCODE_HLT) {
        hype_fatal("m3-5: test guest did not halt cleanly (reason=0x%llx guest_rip=0x%llx "
                   "expected_entry=0x%llx qual=0x%llx)",
                   (unsigned long long)info.reason, (unsigned long long)info.guest_rip,
                   (unsigned long long)entry_rip, (unsigned long long)info.qualification);
    }

    hype_serial_print(
        "m3-5: Linux boot-protocol shim test guest halted cleanly (reason=0x%llx, guest_rip=0x%llx, "
        "PIC master IMR=0x%x, PIT ch0 latch_pending=%d)\n",
        (unsigned long long)info.reason, (unsigned long long)info.guest_rip, g_m3_5_pic.master.imr,
        g_m3_5_pit.channels[0].latch_pending);
}

/*
 * M4-3: builds a minimal 64-bit long-mode guest (NOT a Linux
 * boot-protocol shim like M3-5 -- hype_svm_vcpu_create_long_mode()
 * itself has no such requirement, it just needs an entry RIP/RSP/CR3,
 * so this test skips core/linux_boot.h entirely), this time with
 * nested paging genuinely enabled (M3-5 passed npt_root=0 -- "no NPT
 * for this first pass"), and with the emulated flash's covering NPT
 * entry marked not-present so the guest's own memory-mapped accesses
 * to it take a real NPF, decoded and dispatched by
 * hype_svm_vcpu_handle_npf() to devices/pflash.h. SVM-only, same
 * reasoning as run_m3_5_linux_shim_test() above (VMX's vcpu_run stays
 * NULL past M2-7).
 */
static void run_m4_3_pflash_mmio_test(const hype_vmm_ops_t *ops, hype_vmm_kind_t kind) {
    unsigned long long i;
    uint64_t entry_rip, guest_cr3, rsp, npt_root_phys;
    hype_vcpu_ctx_t *ctx;
    hype_vmexit_info_t info;

    if (kind != HYPE_VMM_KIND_SVM) {
        hype_serial_print("m4-3: skipped -- %s has no working vcpu_run yet (see vmx_ops.c)\n", ops->name);
        return;
    }

    /* M2-6 hard invariant: zero every byte of this guest's reserved
     * RAM before its first VM-entry, on every (re)start. NOT applied to
     * g_m4_3_pflash_backing -- that is the guest's *persistent*
     * variable store (devices/pflash.h's own hype_pflash_reset() doc
     * comment), which by definition must survive a restart; this test
     * starts it from a known all-zero state itself instead, since
     * there is nothing to persist across yet (M5's disk driver). */
    hype_guest_ram_zero(g_m4_3_guest_code, sizeof(g_m4_3_guest_code));
    hype_guest_ram_zero(g_m4_3_guest_stack, sizeof(g_m4_3_guest_stack));
    for (i = 0; i < sizeof(g_m4_3_pflash_backing); i++) {
        g_m4_3_pflash_backing[i] = 0;
    }
    hype_pflash_reset(&g_m4_3_pflash, g_m4_3_pflash_backing, sizeof(g_m4_3_pflash_backing));

    for (i = 0; i < sizeof(g_m4_3_payload_template); i++) {
        g_m4_3_guest_code[i] = g_m4_3_payload_template[i];
    }
    hype_write_le64(g_m4_3_guest_code + HYPE_M4_3_PAYLOAD_RBX_IMM_OFFSET, HYPE_M4_3_PFLASH_GPA);
    hype_write_le64(g_m4_3_guest_code + HYPE_M4_3_PAYLOAD_RDX_IMM_OFFSET, HYPE_M4_3_PFLASH_GPA + 0x100);

    entry_rip = (uint64_t)(uintptr_t)g_m4_3_guest_code;
    rsp = (uint64_t)(uintptr_t)(g_m4_3_guest_stack + sizeof(g_m4_3_guest_stack));

    /* Rebuilt fresh here (same "fresh on every (re)start" convention as
     * every other identity map in this file) -- reusing the same
     * static tables run_m3_5_linux_shim_test() already used above is
     * safe since that guest has already finished running by the time
     * this one starts (see run_all_test_guests()), never concurrently. */
    hype_paging_build_identity(g_guest_pml4, g_guest_pdpt, g_guest_pd, HYPE_M3_5_GUEST_PAGING_GB);
    guest_cr3 = (uint64_t)(uintptr_t)g_guest_pml4;

    hype_npt_build_identity(g_npt_pml4, g_npt_pdpt, g_npt_pd, HYPE_NPT_MAX_GB);
    hype_npt_mark_not_present(g_npt_pd, HYPE_M4_3_PFLASH_GPA);
    npt_root_phys = (uint64_t)(uintptr_t)g_npt_pml4;

    hype_serial_print("m4-3: entry_rip=0x%llx guest_cr3=0x%llx npt_root=0x%llx pflash_gpa=0x%llx\n",
                       (unsigned long long)entry_rip, (unsigned long long)guest_cr3,
                       (unsigned long long)npt_root_phys, (unsigned long long)HYPE_M4_3_PFLASH_GPA);

    ctx = hype_svm_vcpu_create_long_mode(entry_rip, guest_cr3, rsp, npt_root_phys);
    if (ctx == 0) {
        hype_fatal("m4-3: vcpu_create_long_mode failed");
    }

    for (;;) {
        if (ops->vcpu_run(ctx, &info) != 0) {
            hype_fatal("m4-3: VM-entry failed (reason=0x%llx)", (unsigned long long)info.reason);
        }

        if (info.reason == HYPE_SVM_EXITCODE_NPF) {
            if (hype_svm_vcpu_handle_npf(ctx, &g_m4_3_pflash, HYPE_M4_3_PFLASH_GPA) != 0) {
                hype_fatal("m4-3: unhandled/unrecognized MMIO access (qual=0x%llx guest_rip=0x%llx)",
                           (unsigned long long)info.qualification, (unsigned long long)info.guest_rip);
            }
            continue;
        }

        break;
    }

    if (info.reason != HYPE_SVM_EXITCODE_HLT) {
        hype_fatal("m4-3: test guest did not halt cleanly (reason=0x%llx guest_rip=0x%llx)",
                   (unsigned long long)info.reason, (unsigned long long)info.guest_rip);
    }

    if (g_m4_3_pflash_backing[0] != 0xABu) {
        hype_fatal("m4-3: pflash write path failed: backing[0]=0x%x, expected 0xab",
                   g_m4_3_pflash_backing[0]);
    }
    if (g_m4_3_pflash_backing[0x100] != 0xABu) {
        hype_fatal(
            "m4-3: pflash read path failed: backing[0x100]=0x%x, expected 0xab (the guest's own "
            "memory-mapped read must not have returned 0xab)",
            g_m4_3_pflash_backing[0x100]);
    }

    hype_serial_print(
        "m4-3: pflash MMIO test guest halted cleanly (reason=0x%llx, guest_rip=0x%llx, backing[0]=0x%x "
        "backing[0x100]=0x%x -- write and read-back round trip both verified)\n",
        (unsigned long long)info.reason, (unsigned long long)info.guest_rip, g_m4_3_pflash_backing[0],
        g_m4_3_pflash_backing[0x100]);
}

/* Runs every test guest in sequence -- what actually gets dispatched
 * (inline on the BSP, or onto a pinned AP; see efi_main) so each new
 * milestone's test guest still exercises real 1:1 vCPU-to-pCPU
 * pinning (M3-2) rather than only the first one ever tested. */
static void EFIAPI run_all_test_guests(void *arg) {
    hype_test_guest_args_t *args = (hype_test_guest_args_t *)arg;
    run_test_guest(arg);
    run_m3_5_linux_shim_test(args->ops, args->kind);
    run_m4_3_pflash_mmio_test(args->ops, args->kind);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_MEMORY_DESCRIPTOR *map = 0;
    UINTN map_size = 0, desc_size = 0, map_key = 0;
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    int have_gop;

    hype_console_print(SystemTable, "hype\n");

    /* Safe to bring up now: it's raw port I/O, independent of Boot
     * Services or which GDT/IDT happens to be active either way. */
    hype_serial_init(HYPE_SERIAL_COM1, 115200);

    status = hype_memmap_get(SystemTable->BootServices, &map, &map_size, &desc_size, &map_key);
    if (status != EFI_SUCCESS) {
        hype_fatal("failed to get memory map: 0x%llx", (unsigned long long)status);
    }

    hype_memmap_dump(SystemTable, map, map_size, desc_size);
    SystemTable->BootServices->FreePool(map);

    /* LocateProtocol is a Boot Services call like the memory map fetch
     * above -- must happen before ExitBootServices(). A GOP-less system
     * isn't fatal: serial remains available regardless, so just note it
     * and move on rather than hype_panic(). */
    status = hype_gop_locate(SystemTable->BootServices, &gop);
    have_gop = (status == EFI_SUCCESS);
    if (!have_gop) {
        hype_console_print(SystemTable, "no GOP found: 0x%llx\n", (unsigned long long)status);
    }

    /*
     * M3-2: 1:1 vCPU-to-pCPU pinning. The test guest launches *here*,
     * before ExitBootServices, dispatched onto a pinned AP via a
     * blocking StartupThisAP call when one is available -- not after,
     * parked in a loop waiting for later work. An AP that firmware
     * dispatched via MP services is not guaranteed to survive
     * ExitBootServices: firmware is free to reclaim/reset APs as part
     * of that transition (confirmed the hard way -- an earlier design
     * that parked an AP here and signaled it to do real work only
     * after ExitBootServices/our own GDT+paging swap reliably hung,
     * with a golden-signal test confirming the AP simply stopped
     * responding to shared memory writes once ExitBootServices had
     * run, even with no work involved beyond the bare go/done flags).
     * Running here, synchronously, sidesteps that entirely -- nothing
     * run_all_test_guests() does depends on our own GDT/IDT/paging/
     * timer (see run_test_guest()'s own comment), so there's no reason
     * it needs to run after ExitBootServices at all. No extra pCPU (or
     * no MP services
     * at all) isn't fatal -- the test guest just runs on the BSP
     * instead, right here, same as M2-7/M3-1's original behavior.
     */
    {
        hype_vmm_kind_t kind = hype_cpu_detect_vmm_kind();
        const hype_vmm_ops_t *ops = hype_vmm_ops_for_kind(kind);
        static hype_test_guest_args_t args;
        EFI_MP_SERVICES_PROTOCOL *mp = 0;
        UINTN target_ap = 0;
        int have_target_ap = 0;

        if (ops == 0) {
            hype_fatal("no usable virtualization extension (VMX/SVM) detected");
        }
        hype_serial_print("vmm: %s detected\n", ops->name);

        args.ops = ops;
        args.kind = kind;

        status = hype_mp_locate(SystemTable->BootServices, &mp);
        if (status == EFI_SUCCESS) {
            status = hype_mp_pick_target_ap(mp, &target_ap);
            have_target_ap = (status == EFI_SUCCESS);
        }

        if (have_target_ap) {
            BOOLEAN finished = 0;

            hype_console_print(SystemTable, "mp: dispatching test guest to pinned pCPU #%llu\n",
                                (unsigned long long)target_ap);
            /* WaitEvent=0/NULL => blocking: waits for
             * run_all_test_guests() to return, which it always does on
             * success. On a fatal path inside it (hype_fatal() ->
             * hype_halt_forever(), never returns), this call -- and so
             * the whole boot -- blocks forever on that core too; the
             * diagnostic message fatal() already printed to serial is
             * what actually matters for debugging a genuinely
             * unrecoverable condition, so this is an accepted
             * tradeoff, not a gap. */
            status = mp->StartupThisAP(mp, run_all_test_guests, target_ap, 0, 0, &args, &finished);
            if (status != EFI_SUCCESS) {
                hype_fatal("mp: StartupThisAP on pCPU #%llu failed: 0x%llx",
                           (unsigned long long)target_ap, (unsigned long long)status);
            }
            hype_console_print(SystemTable, "mp: pinned pCPU #%llu finished\n",
                                (unsigned long long)target_ap);
        } else {
            hype_console_print(SystemTable,
                                "mp: no extra pCPU available (0x%llx) -- test guest running on the BSP\n",
                                (unsigned long long)status);
            run_all_test_guests(&args);
        }
    }

    status = hype_exit_boot_services(ImageHandle, SystemTable->BootServices);
    if (status != EFI_SUCCESS) {
        hype_fatal("ExitBootServices failed: 0x%llx", (unsigned long long)status);
    }

    /*
     * GDT, paging, and IDT are all built and loaded here, together,
     * after ExitBootServices() -- not earlier, and not GDT/paging early
     * with IDT deferred (an ordering this project tried first, and
     * which is exactly what caused the bug below). Two things are both
     * true and both matter:
     *
     * 1. UEFI's Boot Services calls (ConOut, GetMemoryMap, ...) can
     *    re-enable interrupts as a documented side effect of internally
     *    raising/restoring TPL, outside our control -- so no `cli`
     *    before this point ever durably holds while more Boot Services
     *    calls are still to come.
     * 2. Firmware's IDT entries reference firmware's OWN GDT selectors
     *    (e.g. its timer ISR's CS). Swapping in our own (much smaller)
     *    GDT while firmware's IDT is still the one loaded means any
     *    interrupt landing in that gap makes the CPU fault trying to
     *    load a now out-of-bounds selector, before our own handler even
     *    exists to catch it -- confirmed via QEMU's `-d int` trace: a
     *    normally-handled timer IRQ under firmware's IDT, immediately
     *    followed by a #GP whose error code matched firmware's own CS
     *    selector, cascading into a double then triple fault (a full
     *    VM reset, not even a message from our own panic handler).
     *
     * Doing both swaps back-to-back, under `cli`, right after Boot
     * Services are already gone for good, closes both gaps at once:
     * nothing can flip interrupts back on (nothing calls firmware code
     * again), and GDT+IDT are never inconsistent with each other while
     * an interrupt could actually fire.
     */
    hype_cli();

    hype_gdt_build(g_gdt);
    hype_gdt_load(g_gdt, HYPE_GDT_ENTRY_COUNT);
    hype_serial_print("own GDT loaded\n");

    hype_paging_build_identity(g_pml4, g_pdpt, g_pd, HYPE_PAGING_MAX_GB);
    hype_paging_load(g_pml4);
    hype_serial_print("own paging loaded\n");

    hype_idt_build(g_idt, hype_isr_stub_table, HYPE_GDT_CODE64_SEL);
    hype_idt_load(g_idt, HYPE_IDT_ENTRY_COUNT);
    hype_serial_print("own IDT loaded\n");

    /*
     * Boot Services -- including ConOut, which every hype_console_print
     * above depended on -- are gone as of the ExitBootServices() call
     * above. This is now the only kernel running on this CPU. Serial
     * (initialized above) is one output channel; GOP (if found earlier,
     * while Boot Services still worked) is the other -- the framebuffer
     * itself is just memory, identity-mapped by our own paging above,
     * so writing into it needs nothing further from firmware. Only
     * handle the two 32bpp linear pixel formats GOP defines; anything
     * else (PixelBltOnly, PixelBitMask) isn't a linear framebuffer we
     * can just write into, so skip it rather than misinterpret it.
     */
    if (have_gop && gop->Mode != 0 && gop->Mode->Info != 0 &&
        (gop->Mode->Info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ||
         gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor)) {
        hype_gop_console_init(&g_gop_console, (void *)gop->Mode->FrameBufferBase,
                               gop->Mode->Info->HorizontalResolution,
                               gop->Mode->Info->VerticalResolution,
                               gop->Mode->Info->PixelsPerScanLine,
                               0xFFFFFFu, 0x000000u);
        /* From here on, any fatal fault (arch/x86_64/cpu/isr_decode.c)
         * prints to this console too, not just serial. */
        hype_fatal_set_gop(&g_gop_console);
        hype_gop_print(&g_gop_console, "hype: Boot Services exited, hypervisor now running\n");
    }

    hype_serial_print("hype: Boot Services exited, hypervisor now running\n");

    /*
     * M1-8: bring up the host's own timer tick. Ordering matters again:
     * mask the stray LAPIC timer firmware left armed and firing at
     * vector 32 (observed while validating M1-5 -- see lapic.h) and
     * every legacy PIC line (pic.c's remap masks all 16 before we
     * unmask just the one we handle) *before* registering our handler
     * and unmasking IRQ0, so nothing can fire on a vector we haven't
     * wired up yet. Only `sti` once all of that is in place --
     * interrupts have been durably masked since right after
     * ExitBootServices() (idt_load.c) specifically so nothing could
     * fire before we were ready for it. This is that "ready."
     */
    hype_lapic_mask_timer((volatile uint32_t *)HYPE_LAPIC_DEFAULT_BASE);
    hype_pic_remap_and_mask_all(HYPE_TIMER_VECTOR);
    hype_isr_register(HYPE_TIMER_VECTOR, hype_timer_isr);
    hype_pic_unmask_irq(HYPE_TIMER_IRQ);
    hype_pit_init(1000);
    hype_sti();

    {
        uint64_t target = hype_timer_get_ticks() + 1000; /* ~1s at 1000Hz */
        while (hype_timer_get_ticks() < target) {
            hype_wait_for_interrupt();
        }
    }
    hype_serial_print("timer: %llu ticks (PIT @ 1000Hz)\n",
                       (unsigned long long)hype_timer_get_ticks());

    hype_halt_forever();
}
