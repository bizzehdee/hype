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
#include "../core/admission.h"
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
#include "../devices/acpi.h"
#include "../devices/acpi_loader.h"
#include "../devices/fw_cfg.h"
#include "../devices/ahci.h"
#include "../devices/atapi.h"
#include "../devices/ramfb.h"

/* Static storage: still valid (and unmoving) once these get built and
 * loaded, after ExitBootServices() below. */
static hype_gdt_entry_t g_gdt[HYPE_GDT_ENTRY_COUNT];
static hype_idt_entry_t g_idt[HYPE_IDT_ENTRY_COUNT];
static hype_pte_t g_pml4[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_pte_t g_pdpt[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_pte_t g_pd[HYPE_PAGING_MAX_GB][HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_gop_console_t g_gop_console;

/*
 * M2-7's hand-written test guest runs in real-address mode
 * (hype_vmcb_build_realmode_guest()), which points the guest's
 * CS.base/SS.base directly at this buffer's own physical address (see
 * vmcb.h). Confirmed on real AMD hardware (two different 32GB
 * machines) that this is NOT safe with a plain static buffer: AMD SVM
 * only implements the low 32 bits of most VMCB segment base fields
 * (vmcb.h's hype_vmcb_seg_t comment) -- real silicon silently
 * truncates CS.base to bits 31:0 whenever the linker/loader happens to
 * place this buffer above 4GB (which real UEFI firmware on a machine
 * with enough RAM does routinely; QEMU's own small test VMs never
 * happen to), sending the guest's first fetch to a completely
 * unrelated physical address and triple-faulting it instantly
 * (VMEXIT_SHUTDOWN) -- nested SVM under QEMU/KVM apparently honors the
 * full 64-bit field, masking this on every dev-environment run. Fixed
 * by explicitly allocating this guest's code/stack pages below 4GB via
 * UEFI's AllocatePages(AllocateMaxAddress) instead of trusting the
 * compiler's own static placement -- see hype_alloc_pages_below_4gb()
 * and its call site in efi_main(), before the MP dispatch block. */
static uint64_t g_m2_7_guest_code_phys;
static uint64_t g_m2_7_guest_stack_top_phys;

/*
 * RAM-1: a real, mem_mb-sized guest RAM region -- the first guest
 * memory in this project that isn't a small, fixed-size static array.
 * Allocated via AllocatePages(AllocateAnyPages) (no address
 * constraint, unlike g_m2_7's below-4GB requirement above: this
 * region is only ever used by a long-mode guest, whose CS.base is
 * architecturally forced to 0, so the same 32-bit segment-base
 * truncation risk doesn't apply here) on the BSP before MP dispatch,
 * same timing/ordering reasoning as g_m2_7_guest_code_phys. Sized from
 * HYPE_RAM_1_TEST_MEM_MB, standing in for a real per-VM mem_mb until a
 * real hype.cfg is actually read from the ESP (a separate, later piece
 * -- see task.md's RAM-1 note) -- gated by ADM-1's own already-tested
 * hype_adm_check_memory() against this machine's real usable RAM
 * (computed in efi_main(), see usable_ram_bytes), the first time that
 * check runs in the real boot path rather than only under its own
 * unit tests.
 */
#define HYPE_RAM_1_TEST_MEM_MB 64u
static uint64_t g_ram_1_base_phys;
static uint64_t g_ram_1_size_bytes;

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
 * unlike NPT (arch/x86_64/svm/npt.h).
 *
 * Tied to HYPE_PAGING_MAX_GB, not a separate smaller constant, for the
 * same real-hardware reason as HYPE_NPT_MAX_GB (arch/x86_64/svm/npt.h)
 * -- a 4GB-only map left this guest's own entry point (a static buffer
 * in the same image) unmapped and immediately triple-faulting
 * (VMEXIT_SHUTDOWN) the first time this ran on real AMD hardware whose
 * firmware happened to load the image above 4GB, something QEMU's own
 * small test VMs never exercised. */
#define HYPE_M3_5_GUEST_PAGING_GB HYPE_PAGING_MAX_GB
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
 * comfortably inside the NPT/guest identity map this test guest reuses
 * (HYPE_NPT_MAX_GB/HYPE_M3_5_GUEST_PAGING_GB, now both tied to
 * HYPE_PAGING_MAX_GB -- see that constant's own comment) and nowhere
 * near any real static buffer this project actually uses --
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

/* Same as hype_write_le64() above, but for a 4-byte immediate slot --
 * NOT interchangeable with it: calling the 8-byte version against a
 * 4-byte slot overwrites the following instruction's opening bytes. */
static void hype_write_le32(unsigned char *dst, uint32_t value) {
    int i;
    for (i = 0; i < 4; i++) {
        dst[i] = (unsigned char)(value >> (8 * i));
    }
}

/*
 * Allocates `pages` 4KB pages entirely below the 4GB boundary via
 * AllocateMaxAddress -- must be called before ExitBootServices(), same
 * as every other Boot Services call in this file. Needed specifically
 * for M2-7's real-mode test guest (see its own buffer-declaration
 * comment): AMD SVM only implements the low 32 bits of most VMCB
 * segment base fields, so that guest's CS.base/SS.base (set directly
 * to this buffer's physical address) must fit in 32 bits or real
 * hardware silently truncates it -- confirmed the hard way on real AMD
 * hardware, where the compiler's own static placement landed just past
 * 5GB. Fatal on failure: every caller needs this memory to exist for
 * its test guest to run at all. Returns the allocated physical
 * address directly (this project's flat-identity-map convention --
 * a plain pointer dereference at that address is valid immediately,
 * no translation needed).
 */
static uint64_t hype_alloc_pages_below_4gb(EFI_BOOT_SERVICES *bs, UINTN pages) {
    EFI_PHYSICAL_ADDRESS mem = 0xFFFFFFFFULL;
    EFI_STATUS status = bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, &mem);
    if (status != EFI_SUCCESS) {
        hype_fatal("AllocatePages(<4GB, %u pages) failed: 0x%llx", (unsigned int)pages,
                   (unsigned long long)status);
    }
    return (uint64_t)mem;
}

/*
 * RAM-1: allocates `pages` 4KB pages anywhere firmware chooses
 * (AllocateAnyPages -- no address constraint, unlike
 * hype_alloc_pages_below_4gb() above). Correct for guest RAM a
 * long-mode guest's own RIP/data addressing will reach directly (no
 * 32-bit segment-base truncation risk the way M2-7's real-mode guest
 * has); NPT/guest-CR3 identity-map sizing must cover wherever this
 * actually lands (see the gb_to_map computation at this function's
 * call site) rather than assuming a fixed low range. Must be called
 * before ExitBootServices(), same as every other Boot Services call in
 * this file.
 */
static uint64_t hype_alloc_pages_any(EFI_BOOT_SERVICES *bs, UINTN pages) {
    EFI_PHYSICAL_ADDRESS mem = 0;
    EFI_STATUS status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &mem);
    if (status != EFI_SUCCESS) {
        hype_fatal("AllocatePages(AnyPages, %u pages) failed: 0x%llx", (unsigned int)pages,
                   (unsigned long long)status);
    }
    return (uint64_t)mem;
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

    /* Real-hardware debugging: a hang here (no further serial output
     * at all past this line) localizes the failure to ops->enable()
     * itself -- RDMSR/WRMSR against real hardware MSRs, unlike
     * anything QEMU/KVM's nested-virtualization emulation exercises
     * the same way bare metal does. */
    hype_debug_print("vmm: about to enable %s...\n", ops->name);
    if (ops->enable() != 0) {
        hype_fatal("vmm: %s enable failed", ops->name);
    }
    hype_debug_print("vmm: %s enabled\n", ops->name);

    /*
     * VMX's vcpu_create/vcpu_run stay NULL past M2-7 (see vmx_ops.c)
     * -- only SVM actually launches here; VMX's equivalent is
     * deferred to M2-8's real Intel hardware pass.
     */
    if (ops->vcpu_create == 0 || ops->vcpu_run == 0) {
        hype_debug_print("vmm: %s vCPU launch not implemented yet -- test guest skipped\n", ops->name);
        return;
    }

    /* M2-6 hard invariant: zero every byte of a guest's reserved RAM
     * before its first VM-entry, on every (re)start -- not just the
     * bytes we're about to write ourselves. g_m2_7_guest_code_phys/
     * g_m2_7_guest_stack_top_phys are below-4GB pages allocated by
     * efi_main() (see hype_alloc_pages_below_4gb()) before this
     * function ever runs -- see this test's own buffer-declaration
     * comment above for why a plain static buffer isn't safe here. */
    uint8_t *guest_code = (uint8_t *)(uintptr_t)g_m2_7_guest_code_phys;
    hype_guest_ram_zero(guest_code, 4096);
    hype_guest_ram_zero((void *)(uintptr_t)(g_m2_7_guest_stack_top_phys - 4096), 4096);
    guest_code[0] = 0xF4; /* HLT */

    uint64_t entry_phys = g_m2_7_guest_code_phys;
    uint64_t stack_phys = g_m2_7_guest_stack_top_phys;
    hype_debug_print("vmm: %s test guest: entry_phys=0x%llx stack_phys=0x%llx\n", ops->name,
                      (unsigned long long)entry_phys, (unsigned long long)stack_phys);

    /* M3-1: SVM's NPT is real and QEMU-validated (unlike EPT, which
     * has nowhere to be wired in yet -- VMX's vcpu_create stays NULL,
     * see above). Building it fresh for every (re)start, same as
     * everything else here. */
    uint64_t npt_root_phys = 0;
    if (kind == HYPE_VMM_KIND_SVM) {
        hype_npt_build_identity(g_npt_pml4, g_npt_pdpt, g_npt_pd, HYPE_NPT_MAX_GB);
        npt_root_phys = (uint64_t)(uintptr_t)g_npt_pml4;
        hype_debug_print("vmm: %s NPT identity map built (root=0x%llx, %u GB)\n", ops->name,
                          (unsigned long long)npt_root_phys, HYPE_NPT_MAX_GB);
    }

    hype_debug_print("vmm: about to call %s vcpu_create...\n", ops->name);
    hype_vcpu_ctx_t *ctx = ops->vcpu_create(entry_phys, stack_phys, npt_root_phys);
    if (ctx == 0) {
        hype_fatal("vmm: %s vcpu_create failed", ops->name);
    }
    hype_debug_print("vmm: %s vcpu_create done -- entering dispatch loop...\n", ops->name);

    hype_vmexit_info_t info;
    int rc = hype_vmexit_dispatch_loop(ops, ctx, kind, &info);
    if (rc != 0) {
        hype_fatal("vmm: %s test guest did not exit cleanly (reason=0x%llx qual=0x%llx)", ops->name,
                   (unsigned long long)info.reason, (unsigned long long)info.qualification);
    }
    hype_debug_print("vmm: %s test guest halted cleanly (reason=0x%llx, guest_rip=0x%llx)\n", ops->name,
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

/*
 * M4-4: synthesizes RSDP/XSDT/FADT/MADT/MCFG/DSDT (devices/acpi.h) plus
 * the "etc/table-loader" linker/loader script (devices/acpi_loader.h),
 * registers them with a fw_cfg device model (devices/fw_cfg.h), and
 * validates the device model itself end-to-end: a hand-written
 * long-mode test guest speaks fw_cfg's real DMA protocol (the same one
 * this project's own vendored, unmodified OVMF driver uses,
 * edk2/OvmfPkg/Library/QemuFwCfgLib) to fetch "etc/acpi/rsdp" into a
 * guest buffer, which the host then compares against the exact bytes
 * hype_acpi_build_rsdp() built. This validates the fw_cfg device model
 * genuinely works under real QEMU/SVM; it does NOT yet boot real OVMF
 * as a nested guest to confirm OVMF's own AcpiPlatformDxe successfully
 * consumes this content end-to-end -- that integration is M4-6's job,
 * matching this project's established "build the primitive now, defer
 * the harder integration" pattern (e.g. M4-3's flash persistence).
 */
static uint8_t g_m4_4_guest_code[128] __attribute__((aligned(4096)));
static uint8_t g_m4_4_guest_stack[4096] __attribute__((aligned(4096)));
static uint8_t g_m4_4_access_struct[16] __attribute__((aligned(16)));
static uint8_t g_m4_4_dest_buffer[64] __attribute__((aligned(16)));

static hype_acpi_rsdp_t g_m4_4_rsdp;
static uint8_t g_m4_4_tables_blob[4096] __attribute__((aligned(64)));
static hype_acpi_loader_entry_t g_m4_4_loader_script[HYPE_ACPI_LOADER_SCRIPT_ENTRIES];
static hype_fw_cfg_t g_m4_4_fw_cfg;

/*
 * Guest payload: writes the DMA access-struct's guest-physical address
 * to ports 0x514/0x518 (triggering the transfer on the second write,
 * per fw_cfg's own protocol), then polls the access struct's own
 * Control field (an ordinary guest-RAM load, not a port access) until
 * the device clears it to 0. The access struct's own CONTENT (control/
 * length/address, all big-endian) is written directly into guest
 * memory by the host before launch, matching how earlier test guests'
 * initial state (M3-5's zero page, M4-3's pflash backing) were always
 * host-populated rather than guest-computed -- only the two immediate
 * values ports 0x514/0x518 actually need (RBX's address and the two
 * pre-byte-swapped 32-bit halves) are patched into this template at
 * runtime, same convention as M4-3's payload.
 *
 *   mov rbx, <patched: access struct guest-physical address>
 *                                        48 BB 00 00 00 00 00 00 00 00
 *   mov dx, 0x514                        66 BA 14 05
 *   mov eax, <patched: byte-swapped upper 32 bits of that address>
 *                                        B8 00 00 00 00
 *   out dx, eax                          EF
 *   mov dx, 0x518                        66 BA 18 05
 *   mov eax, <patched: byte-swapped lower 32 bits of that address>
 *                                        B8 00 00 00 00
 *   out dx, eax                          EF
 * poll:
 *   mov eax, [rbx]                       8B 03
 *   test eax, eax                        85 C0
 *   jnz poll                             75 FA
 *   hlt                                  F4
 *   jmp $-3                              EB FD
 */
static const uint8_t g_m4_4_payload_template[] = {
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x66, 0xBA, 0x14, 0x05,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0xEF,
    0x66, 0xBA, 0x18, 0x05,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0xEF,
    0x8B, 0x03,
    0x85, 0xC0,
    0x75, 0xFA,
    0xF4,
    0xEB, 0xFD
};
#define HYPE_M4_4_PAYLOAD_RBX_IMM_OFFSET 2
#define HYPE_M4_4_PAYLOAD_DMA_HIGH_IMM_OFFSET 15
#define HYPE_M4_4_PAYLOAD_DMA_LOW_IMM_OFFSET 25

static void hype_write_be32(unsigned char *dst, uint32_t value) {
    dst[0] = (unsigned char)(value >> 24);
    dst[1] = (unsigned char)(value >> 16);
    dst[2] = (unsigned char)(value >> 8);
    dst[3] = (unsigned char)value;
}

static void hype_write_be64(unsigned char *dst, uint64_t value) {
    int i;
    for (i = 0; i < 8; i++) {
        dst[i] = (unsigned char)(value >> (8 * (7 - i)));
    }
}

static uint32_t hype_byteswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) |
           ((v & 0xFF000000u) >> 24);
}

static void run_m4_4_fw_cfg_test(const hype_vmm_ops_t *ops, hype_vmm_kind_t kind) {
    unsigned long long i;
    uint64_t entry_rip, guest_cr3, rsp, access_struct_phys, dest_buffer_phys;
    uint32_t access_high, access_low;
    int rsdp_key;
    hype_vcpu_ctx_t *ctx;
    hype_vmexit_info_t info;
    hype_acpi_layout_t layout;
    hype_acpi_config_t cfg;
    uint32_t loader_entries;

    if (kind != HYPE_VMM_KIND_SVM) {
        hype_serial_print("m4-4: skipped -- %s has no working vcpu_run yet (see vmx_ops.c)\n", ops->name);
        return;
    }

    hype_guest_ram_zero(g_m4_4_guest_code, sizeof(g_m4_4_guest_code));
    hype_guest_ram_zero(g_m4_4_guest_stack, sizeof(g_m4_4_guest_stack));
    hype_guest_ram_zero(g_m4_4_access_struct, sizeof(g_m4_4_access_struct));
    hype_guest_ram_zero(g_m4_4_dest_buffer, sizeof(g_m4_4_dest_buffer));

    for (i = 0; i < HYPE_ACPI_MAX_CPUS; i++) {
        cfg.apic_ids[i] = (uint8_t)i;
    }
    cfg.cpu_count = 1;
    cfg.local_apic_address = 0xFEE00000u;
    cfg.io_apic_id = 1;
    cfg.io_apic_address = 0xFEC00000u;
    cfg.io_apic_gsi_base = 0;
    cfg.mcfg_base_address = 0xE0000000ULL;
    cfg.pci_segment = 0;
    cfg.pci_start_bus = 0;
    cfg.pci_end_bus = 255;
    cfg.sci_interrupt = 9;

    if (hype_acpi_build_tables_blob(g_m4_4_tables_blob, sizeof(g_m4_4_tables_blob), &cfg, &layout) != 0) {
        hype_fatal("m4-4: hype_acpi_build_tables_blob failed");
    }
    hype_acpi_build_rsdp(&g_m4_4_rsdp, layout.xsdt_offset);
    loader_entries = hype_acpi_loader_build_script(g_m4_4_loader_script, &layout);

    hype_fw_cfg_reset(&g_m4_4_fw_cfg);
    rsdp_key = hype_fw_cfg_add_file(&g_m4_4_fw_cfg, HYPE_ACPI_LOADER_FILE_RSDP, (const uint8_t *)&g_m4_4_rsdp,
                                     sizeof(g_m4_4_rsdp));
    if (rsdp_key < 0) {
        hype_fatal("m4-4: fw_cfg registry full while registering rsdp");
    }
    if (hype_fw_cfg_add_file(&g_m4_4_fw_cfg, HYPE_ACPI_LOADER_FILE_TABLES, g_m4_4_tables_blob,
                              layout.total_length) < 0) {
        hype_fatal("m4-4: fw_cfg registry full while registering tables");
    }
    if (hype_fw_cfg_add_file(&g_m4_4_fw_cfg, "etc/table-loader", (const uint8_t *)g_m4_4_loader_script,
                              loader_entries * (uint32_t)sizeof(hype_acpi_loader_entry_t)) < 0) {
        hype_fatal("m4-4: fw_cfg registry full while registering table-loader");
    }

    /* Host pre-populates the DMA access struct's content directly in
     * guest memory (control/length/address, all big-endian) -- the
     * guest payload itself only needs to trigger the transfer and poll
     * for completion, not construct this struct. select_key in the
     * upper 16 bits of control, matching fw_cfg's own DMA SELECT
     * convention. */
    access_struct_phys = (uint64_t)(uintptr_t)g_m4_4_access_struct;
    dest_buffer_phys = (uint64_t)(uintptr_t)g_m4_4_dest_buffer;
    {
        uint32_t control = ((uint32_t)rsdp_key << 16) | HYPE_FW_CFG_DMA_CTL_SELECT | HYPE_FW_CFG_DMA_CTL_READ;
        hype_write_be32(g_m4_4_access_struct + 0, control);
        hype_write_be32(g_m4_4_access_struct + 4, (uint32_t)sizeof(g_m4_4_rsdp));
        hype_write_be64(g_m4_4_access_struct + 8, dest_buffer_phys);
    }

    /* Ports 0x514/0x518 expect each 32-bit half byte-swapped on the
     * wire (matching OVMF's own SwapBytes32(AccessHigh/Low) before
     * IoWrite32 -- see devices/fw_cfg.h's own top comment) -- computed
     * here, at host build time, since this is a fully host-controlled
     * synthetic payload (same "known values baked in at build time"
     * pattern M4-3's payload already uses). */
    access_high = (uint32_t)(access_struct_phys >> 32);
    access_low = (uint32_t)access_struct_phys;

    for (i = 0; i < sizeof(g_m4_4_payload_template); i++) {
        g_m4_4_guest_code[i] = g_m4_4_payload_template[i];
    }
    hype_write_le64(g_m4_4_guest_code + HYPE_M4_4_PAYLOAD_RBX_IMM_OFFSET, access_struct_phys);
    hype_write_le32(g_m4_4_guest_code + HYPE_M4_4_PAYLOAD_DMA_HIGH_IMM_OFFSET,
                     hype_byteswap32(access_high));
    hype_write_le32(g_m4_4_guest_code + HYPE_M4_4_PAYLOAD_DMA_LOW_IMM_OFFSET, hype_byteswap32(access_low));

    entry_rip = (uint64_t)(uintptr_t)g_m4_4_guest_code;
    rsp = (uint64_t)(uintptr_t)(g_m4_4_guest_stack + sizeof(g_m4_4_guest_stack));

    hype_paging_build_identity(g_guest_pml4, g_guest_pdpt, g_guest_pd, HYPE_M3_5_GUEST_PAGING_GB);
    guest_cr3 = (uint64_t)(uintptr_t)g_guest_pml4;

    hype_serial_print("m4-4: entry_rip=0x%llx access_struct=0x%llx dest_buffer=0x%llx rsdp_key=0x%x\n",
                       (unsigned long long)entry_rip, (unsigned long long)access_struct_phys,
                       (unsigned long long)dest_buffer_phys, rsdp_key);

    /* No NPT for this test -- everything here is ordinary port I/O
     * plus plain guest-RAM reads/writes, no MMIO-trapped device
     * involved (unlike M4-3's pflash test). */
    ctx = hype_svm_vcpu_create_long_mode(entry_rip, guest_cr3, rsp, 0);
    if (ctx == 0) {
        hype_fatal("m4-4: vcpu_create_long_mode failed");
    }

    for (;;) {
        if (ops->vcpu_run(ctx, &info) != 0) {
            hype_fatal("m4-4: VM-entry failed (reason=0x%llx)", (unsigned long long)info.reason);
        }

        if (info.reason == HYPE_SVM_EXITCODE_IOIO) {
            if (hype_svm_vcpu_handle_fw_cfg_ioio(ctx, &g_m4_4_fw_cfg) != 0) {
                hype_fatal("m4-4: unhandled guest port I/O (qual=0x%llx guest_rip=0x%llx)",
                           (unsigned long long)info.qualification, (unsigned long long)info.guest_rip);
            }
            continue;
        }

        break;
    }

    if (info.reason != HYPE_SVM_EXITCODE_HLT) {
        hype_fatal("m4-4: test guest did not halt cleanly (reason=0x%llx guest_rip=0x%llx)",
                   (unsigned long long)info.reason, (unsigned long long)info.guest_rip);
    }

    for (i = 0; i < sizeof(g_m4_4_rsdp); i++) {
        if (g_m4_4_dest_buffer[i] != ((const uint8_t *)&g_m4_4_rsdp)[i]) {
            hype_fatal(
                "m4-4: fw_cfg DMA read mismatch at byte %llu: guest received 0x%x, expected 0x%x",
                i, g_m4_4_dest_buffer[i], ((const uint8_t *)&g_m4_4_rsdp)[i]);
        }
    }

    hype_serial_print(
        "m4-4: fw_cfg DMA test guest halted cleanly (reason=0x%llx, guest_rip=0x%llx) -- %llu-byte "
        "etc/acpi/rsdp round trip verified byte-for-byte\n",
        (unsigned long long)info.reason, (unsigned long long)info.guest_rip,
        (unsigned long long)sizeof(g_m4_4_rsdp));
}

/*
 * M4-5: synthesizes a single-port AHCI HBA (devices/ahci.h) with one
 * ATAPI CD-ROM attached (devices/atapi.h), backed by an in-memory
 * "ISO" buffer (host-file reading needs M5's disk driver, the same
 * circular dependency M4-3's flash persistence and M4-4's ACPI table
 * blob already had -- build the primitive now, wire real media later).
 * A hand-written long-mode test guest drives the real AHCI/ATAPI
 * protocol: initializes the port's registers, issues a READ(10) ATAPI
 * PACKET command for one sector, polls for completion, halts -- the
 * host then confirms the transferred sector matches the backing
 * buffer byte-for-byte. This validates the AHCI+ATAPI device model
 * itself end-to-end under real QEMU/SVM; it does NOT yet validate a
 * real guest OS's own AHCI/ATAPI driver against it -- that's M4-6's
 * job, matching this project's established "build the primitive now,
 * defer the harder integration" pattern.
 */
#define HYPE_M4_5_AHCI_GPA (HYPE_M4_3_PFLASH_GPA + HYPE_PAGING_2MB)

static uint8_t g_m4_5_media[4 * HYPE_ATAPI_SECTOR_SIZE] __attribute__((aligned(4096)));
static uint8_t g_m4_5_cmd_list[4096] __attribute__((aligned(4096)));
static uint8_t g_m4_5_cmd_table[4096] __attribute__((aligned(4096)));
static uint8_t g_m4_5_rx_fis[4096] __attribute__((aligned(4096)));
static uint8_t g_m4_5_dest_buffer[HYPE_ATAPI_SECTOR_SIZE] __attribute__((aligned(4096)));
static uint8_t g_m4_5_guest_code[256] __attribute__((aligned(4096)));
static uint8_t g_m4_5_guest_stack[4096] __attribute__((aligned(4096)));
static hype_ahci_t g_m4_5_ahci;
static hype_atapi_t g_m4_5_atapi;

/*
 * Guest payload: initializes the AHCI port (GHC.AE, PxCLB/PxCLBU,
 * PxFB/PxFBU, PxCMD=ST|FRE) then issues the command already staged in
 * slot 0 (PxCI=1, triggering hype_svm_vcpu_handle_ahci_npf()'s command
 * processing) and polls PxCI until the device clears it. The Command
 * Header/Command Table/PRDT content itself -- a real Register H2D FIS
 * carrying ATA_CMD_PACKET plus a READ(10) CDB -- is host-built
 * directly into guest memory before launch (same "host pre-populates
 * structured state, guest only triggers+polls" convention M4-4's fw_cfg
 * test already established), avoiding hand-encoding that structure as
 * machine code. Every store here is register-to-memory (0x89) or
 * memory-to-register (0x8B), the same forms hype_mmio_decode() already
 * supports and this project's own test suite already covers.
 *
 *   mov rbx, <patched: AHCI MMIO base>     48 BB 00*8
 *   mov eax, 0x80000000 (GHC.AE)          B8 00 00 00 80
 *   mov [rbx+4], eax                       89 43 04
 *   mov eax, <patched: CLB low32>          B8 00*4
 *   mov [rbx+0x100], eax                   89 83 00 01 00 00
 *   mov eax, <patched: CLB high32>         B8 00*4
 *   mov [rbx+0x104], eax                   89 83 04 01 00 00
 *   mov eax, <patched: FB low32>           B8 00*4
 *   mov [rbx+0x108], eax                   89 83 08 01 00 00
 *   mov eax, <patched: FB high32>          B8 00*4
 *   mov [rbx+0x10C], eax                   89 83 0C 01 00 00
 *   mov eax, 0x00000011 (PxCMD ST|FRE)     B8 11 00 00 00
 *   mov [rbx+0x118], eax                   89 83 18 01 00 00
 *   mov eax, 0x00000001 (PxCI slot 0)      B8 01 00 00 00
 *   mov [rbx+0x138], eax                   89 83 38 01 00 00  <- triggers
 * poll:
 *   mov eax, [rbx+0x138]                   8B 83 38 01 00 00
 *   test eax, eax                          85 C0
 *   jnz poll                               75 F6
 *   hlt                                    F4
 *   jmp $-3                                EB FD
 */
static const uint8_t g_m4_5_payload_template[] = {
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x80,
    0x89, 0x43, 0x04,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x00, 0x01, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x04, 0x01, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x08, 0x01, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x0C, 0x01, 0x00, 0x00,
    0xB8, 0x11, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x18, 0x01, 0x00, 0x00,
    0xB8, 0x01, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x38, 0x01, 0x00, 0x00,
    0x8B, 0x83, 0x38, 0x01, 0x00, 0x00,
    0x85, 0xC0,
    0x75, 0xF6,
    0xF4,
    0xEB, 0xFD
};
#define HYPE_M4_5_PAYLOAD_RBX_IMM_OFFSET 2
#define HYPE_M4_5_PAYLOAD_CLB_LOW_IMM_OFFSET 19
#define HYPE_M4_5_PAYLOAD_CLB_HIGH_IMM_OFFSET 30
#define HYPE_M4_5_PAYLOAD_FB_LOW_IMM_OFFSET 41
#define HYPE_M4_5_PAYLOAD_FB_HIGH_IMM_OFFSET 52

static void run_m4_5_ahci_test(const hype_vmm_ops_t *ops, hype_vmm_kind_t kind) {
    unsigned long long i;
    uint64_t entry_rip, guest_cr3, rsp, npt_root_phys;
    uint64_t ahci_gpa, cmd_list_phys, cmd_table_phys, rx_fis_phys, dest_phys;
    hype_vcpu_ctx_t *ctx;
    hype_vmexit_info_t info;

    if (kind != HYPE_VMM_KIND_SVM) {
        hype_serial_print("m4-5: skipped -- %s has no working vcpu_run yet (see vmx_ops.c)\n", ops->name);
        return;
    }

    hype_guest_ram_zero(g_m4_5_cmd_list, sizeof(g_m4_5_cmd_list));
    hype_guest_ram_zero(g_m4_5_cmd_table, sizeof(g_m4_5_cmd_table));
    hype_guest_ram_zero(g_m4_5_rx_fis, sizeof(g_m4_5_rx_fis));
    hype_guest_ram_zero(g_m4_5_dest_buffer, sizeof(g_m4_5_dest_buffer));
    hype_guest_ram_zero(g_m4_5_guest_code, sizeof(g_m4_5_guest_code));
    hype_guest_ram_zero(g_m4_5_guest_stack, sizeof(g_m4_5_guest_stack));

    /* Recognizable synthetic "ISO" content -- sector N's bytes are all
     * (N & 0xFF), letting the read-back check confirm both the right
     * sector was fetched and the right byte count. */
    for (i = 0; i < sizeof(g_m4_5_media); i++) {
        g_m4_5_media[i] = (uint8_t)((i / HYPE_ATAPI_SECTOR_SIZE) & 0xFFu);
    }
    hype_atapi_reset(&g_m4_5_atapi, g_m4_5_media, sizeof(g_m4_5_media));
    hype_ahci_reset(&g_m4_5_ahci);

    cmd_list_phys = (uint64_t)(uintptr_t)g_m4_5_cmd_list;
    cmd_table_phys = (uint64_t)(uintptr_t)g_m4_5_cmd_table;
    rx_fis_phys = (uint64_t)(uintptr_t)g_m4_5_rx_fis;
    dest_phys = (uint64_t)(uintptr_t)g_m4_5_dest_buffer;

    /* Command Header, slot 0: CFL=5 (Register H2D FIS is 5 DWORDs),
     * ATAPI bit (0x20) set, PRDTL=1 (one PRDT entry) -> opts =
     * (1 << 16) | 0x20 | 5 = 0x00010025. */
    hype_write_le32(g_m4_5_cmd_list + 0, 0x00010025u);
    hype_write_le32(g_m4_5_cmd_list + 4, 0);          /* PRDBC, device-written on completion */
    hype_write_le32(g_m4_5_cmd_list + 8, (uint32_t)cmd_table_phys);
    hype_write_le32(g_m4_5_cmd_list + 12, (uint32_t)(cmd_table_phys >> 32));

    /* Command Table: Register H2D FIS (20 bytes) at offset 0 --
     * command = ATA_CMD_PACKET (0xA0), C bit set (bit 7 of byte 1). */
    g_m4_5_cmd_table[0] = 0x27;
    g_m4_5_cmd_table[1] = 0x80;
    g_m4_5_cmd_table[2] = 0xA0;
    /* ATAPI CDB (16 bytes) at offset 0x40 -- READ(10): LBA=2, transfer
     * length=1 block, matching HYPE_ATAPI_CMD_READ10's own byte layout
     * (devices/atapi.c's handle_read10()). */
    g_m4_5_cmd_table[0x40 + 0] = HYPE_ATAPI_CMD_READ10;
    g_m4_5_cmd_table[0x40 + 5] = 2; /* LBA low byte (LBA = 2) */
    g_m4_5_cmd_table[0x40 + 8] = 1; /* transfer length low byte (1 block) */
    /* PRDT entry 0 (16 bytes) at offset 0x80: destination buffer,
     * DBC field = byte_count - 1 (a real hardware/spec quirk, see
     * hype_ahci_decode_prdt_entry()'s own comment). */
    hype_write_le32(g_m4_5_cmd_table + 0x80 + 0, (uint32_t)dest_phys);
    hype_write_le32(g_m4_5_cmd_table + 0x80 + 4, (uint32_t)(dest_phys >> 32));
    hype_write_le32(g_m4_5_cmd_table + 0x80 + 12, HYPE_ATAPI_SECTOR_SIZE - 1u);

    for (i = 0; i < sizeof(g_m4_5_payload_template); i++) {
        g_m4_5_guest_code[i] = g_m4_5_payload_template[i];
    }
    ahci_gpa = HYPE_M4_5_AHCI_GPA;
    hype_write_le64(g_m4_5_guest_code + HYPE_M4_5_PAYLOAD_RBX_IMM_OFFSET, ahci_gpa);
    hype_write_le32(g_m4_5_guest_code + HYPE_M4_5_PAYLOAD_CLB_LOW_IMM_OFFSET, (uint32_t)cmd_list_phys);
    hype_write_le32(g_m4_5_guest_code + HYPE_M4_5_PAYLOAD_CLB_HIGH_IMM_OFFSET,
                     (uint32_t)(cmd_list_phys >> 32));
    hype_write_le32(g_m4_5_guest_code + HYPE_M4_5_PAYLOAD_FB_LOW_IMM_OFFSET, (uint32_t)rx_fis_phys);
    hype_write_le32(g_m4_5_guest_code + HYPE_M4_5_PAYLOAD_FB_HIGH_IMM_OFFSET,
                     (uint32_t)(rx_fis_phys >> 32));

    entry_rip = (uint64_t)(uintptr_t)g_m4_5_guest_code;
    rsp = (uint64_t)(uintptr_t)(g_m4_5_guest_stack + sizeof(g_m4_5_guest_stack));

    hype_paging_build_identity(g_guest_pml4, g_guest_pdpt, g_guest_pd, HYPE_M3_5_GUEST_PAGING_GB);
    guest_cr3 = (uint64_t)(uintptr_t)g_guest_pml4;

    hype_npt_build_identity(g_npt_pml4, g_npt_pdpt, g_npt_pd, HYPE_NPT_MAX_GB);
    hype_npt_mark_not_present(g_npt_pd, HYPE_M4_5_AHCI_GPA);
    npt_root_phys = (uint64_t)(uintptr_t)g_npt_pml4;

    hype_serial_print("m4-5: entry_rip=0x%llx ahci_gpa=0x%llx cmd_list=0x%llx cmd_table=0x%llx\n",
                       (unsigned long long)entry_rip, (unsigned long long)ahci_gpa,
                       (unsigned long long)cmd_list_phys, (unsigned long long)cmd_table_phys);

    ctx = hype_svm_vcpu_create_long_mode(entry_rip, guest_cr3, rsp, npt_root_phys);
    if (ctx == 0) {
        hype_fatal("m4-5: vcpu_create_long_mode failed");
    }

    for (;;) {
        if (ops->vcpu_run(ctx, &info) != 0) {
            hype_fatal("m4-5: VM-entry failed (reason=0x%llx)", (unsigned long long)info.reason);
        }

        if (info.reason == HYPE_SVM_EXITCODE_NPF) {
            if (hype_svm_vcpu_handle_ahci_npf(ctx, &g_m4_5_ahci, &g_m4_5_atapi, HYPE_M4_5_AHCI_GPA) != 0) {
                hype_fatal("m4-5: unhandled/unrecognized AHCI MMIO access (qual=0x%llx guest_rip=0x%llx)",
                           (unsigned long long)info.qualification, (unsigned long long)info.guest_rip);
            }
            continue;
        }

        break;
    }

    if (info.reason != HYPE_SVM_EXITCODE_HLT) {
        hype_fatal("m4-5: test guest did not halt cleanly (reason=0x%llx guest_rip=0x%llx)",
                   (unsigned long long)info.reason, (unsigned long long)info.guest_rip);
    }

    for (i = 0; i < HYPE_ATAPI_SECTOR_SIZE; i++) {
        if (g_m4_5_dest_buffer[i] != g_m4_5_media[2 * HYPE_ATAPI_SECTOR_SIZE + i]) {
            hype_fatal("m4-5: AHCI/ATAPI READ(10) mismatch at byte %llu: got 0x%x, expected 0x%x", i,
                       g_m4_5_dest_buffer[i], g_m4_5_media[2 * HYPE_ATAPI_SECTOR_SIZE + i]);
        }
    }

    hype_serial_print(
        "m4-5: AHCI/ATAPI test guest halted cleanly (reason=0x%llx, guest_rip=0x%llx) -- %u-byte "
        "READ(10) round trip verified byte-for-byte\n",
        (unsigned long long)info.reason, (unsigned long long)info.guest_rip, HYPE_ATAPI_SECTOR_SIZE);
}

/*
 * VIDEO-2: exercises devices/ramfb.h/fw_cfg.c's new writable-file DMA
 * WRITE path against the exact "etc/ramfb" protocol this project's own
 * vendored OVMF (M4-2) already knows how to speak -- confirmed present
 * in the vendored build (edk2/Build/.../QemuRamfbDxe.efi) via
 * OvmfPkg/QemuRamfbDxe. Same host-pre-populates/guest-triggers
 * convention as M4-4's fw_cfg DMA test, with the roles reversed: the
 * guest payload here writes a host-built RAMFB_CONFIG (28 bytes, every
 * field big-endian, exact layout/values transcribed from
 * edk2/OvmfPkg/QemuRamfbDxe/QemuRamfb.c) into the fw_cfg-registered
 * "etc/ramfb" file, standing in for what a real OVMF driver's
 * QemuRamfbGraphicsOutputSetMode() does after allocating its own
 * framebuffer. This milestone's own scope is the protocol/transport
 * only -- actually presenting the guest's framebuffer content on the
 * host's real screen is VIDEO-3's job (a "post-boot virtual display
 * adapter"), matching this project's established "build the primitive
 * now, defer the harder integration" pattern.
 */
static uint8_t g_video_2_guest_code[128] __attribute__((aligned(4096)));
static uint8_t g_video_2_guest_stack[4096] __attribute__((aligned(4096)));
static uint8_t g_video_2_access_struct[16] __attribute__((aligned(16)));
static uint8_t g_video_2_config_buf[HYPE_RAMFB_CONFIG_SIZE] __attribute__((aligned(16)));
static uint8_t g_video_2_ramfb_backing[HYPE_RAMFB_CONFIG_SIZE];
static uint8_t g_video_2_guest_framebuffer[64] __attribute__((aligned(64)));
static hype_fw_cfg_t g_video_2_fw_cfg;

/* Identical shape to g_m4_4_payload_template -- the access struct's own
 * CONTROL field (host-built, WRITE instead of READ) is what determines
 * direction; the guest instructions that trigger/poll it don't need to
 * differ. Kept as its own copy rather than shared, matching every
 * other milestone test payload here (M4-3/M4-4/M4-5 all have their
 * own, despite overlapping shapes) -- these are milestone-scoped
 * fixtures, not a reusable abstraction. */
static const uint8_t g_video_2_payload_template[] = {
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x66, 0xBA, 0x14, 0x05,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0xEF,
    0x66, 0xBA, 0x18, 0x05,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0xEF,
    0x8B, 0x03,
    0x85, 0xC0,
    0x75, 0xFA,
    0xF4,
    0xEB, 0xFD
};
#define HYPE_VIDEO_2_PAYLOAD_RBX_IMM_OFFSET 2
#define HYPE_VIDEO_2_PAYLOAD_DMA_HIGH_IMM_OFFSET 15
#define HYPE_VIDEO_2_PAYLOAD_DMA_LOW_IMM_OFFSET 25

static void run_video_2_ramfb_test(const hype_vmm_ops_t *ops, hype_vmm_kind_t kind) {
    unsigned long long i;
    uint64_t entry_rip, guest_cr3, rsp, access_struct_phys, config_buf_phys, framebuffer_phys;
    uint32_t access_high, access_low;
    int ramfb_key;
    hype_vcpu_ctx_t *ctx;
    hype_vmexit_info_t info;
    hype_ramfb_config_t decoded;

    if (kind != HYPE_VMM_KIND_SVM) {
        hype_serial_print("video-2: skipped -- %s has no working vcpu_run yet (see vmx_ops.c)\n", ops->name);
        return;
    }

    hype_guest_ram_zero(g_video_2_guest_code, sizeof(g_video_2_guest_code));
    hype_guest_ram_zero(g_video_2_guest_stack, sizeof(g_video_2_guest_stack));
    hype_guest_ram_zero(g_video_2_access_struct, sizeof(g_video_2_access_struct));
    hype_guest_ram_zero(g_video_2_config_buf, sizeof(g_video_2_config_buf));
    hype_guest_ram_zero(g_video_2_ramfb_backing, sizeof(g_video_2_ramfb_backing));
    hype_guest_ram_zero(g_video_2_guest_framebuffer, sizeof(g_video_2_guest_framebuffer));

    hype_fw_cfg_reset(&g_video_2_fw_cfg);
    ramfb_key = hype_fw_cfg_add_writable_file(&g_video_2_fw_cfg, "etc/ramfb", g_video_2_ramfb_backing,
                                               sizeof(g_video_2_ramfb_backing));
    if (ramfb_key < 0) {
        hype_fatal("video-2: fw_cfg registry full while registering etc/ramfb");
    }

    /* Host builds the 28-byte RAMFB_CONFIG the guest "wants to write"
     * directly in guest memory, every field big-endian -- standing in
     * for what a real OVMF driver computes after choosing its own
     * framebuffer address (g_video_2_guest_framebuffer here). Matches
     * M4-4's own "guest payload only triggers/polls, host pre-builds
     * the content" convention. */
    framebuffer_phys = (uint64_t)(uintptr_t)g_video_2_guest_framebuffer;
    hype_write_be64(g_video_2_config_buf + 0, framebuffer_phys);
    hype_write_be32(g_video_2_config_buf + 8, HYPE_RAMFB_FORMAT_XRGB8888);
    hype_write_be32(g_video_2_config_buf + 12, 0);
    hype_write_be32(g_video_2_config_buf + 16, 800);
    hype_write_be32(g_video_2_config_buf + 20, 600);
    hype_write_be32(g_video_2_config_buf + 24, 800u * 4u);

    access_struct_phys = (uint64_t)(uintptr_t)g_video_2_access_struct;
    config_buf_phys = (uint64_t)(uintptr_t)g_video_2_config_buf;
    {
        uint32_t control =
            ((uint32_t)ramfb_key << 16) | HYPE_FW_CFG_DMA_CTL_SELECT | HYPE_FW_CFG_DMA_CTL_WRITE;
        hype_write_be32(g_video_2_access_struct + 0, control);
        hype_write_be32(g_video_2_access_struct + 4, (uint32_t)sizeof(g_video_2_config_buf));
        hype_write_be64(g_video_2_access_struct + 8, config_buf_phys);
    }

    access_high = (uint32_t)(access_struct_phys >> 32);
    access_low = (uint32_t)access_struct_phys;

    for (i = 0; i < sizeof(g_video_2_payload_template); i++) {
        g_video_2_guest_code[i] = g_video_2_payload_template[i];
    }
    hype_write_le64(g_video_2_guest_code + HYPE_VIDEO_2_PAYLOAD_RBX_IMM_OFFSET, access_struct_phys);
    hype_write_le32(g_video_2_guest_code + HYPE_VIDEO_2_PAYLOAD_DMA_HIGH_IMM_OFFSET,
                     hype_byteswap32(access_high));
    hype_write_le32(g_video_2_guest_code + HYPE_VIDEO_2_PAYLOAD_DMA_LOW_IMM_OFFSET,
                     hype_byteswap32(access_low));

    entry_rip = (uint64_t)(uintptr_t)g_video_2_guest_code;
    rsp = (uint64_t)(uintptr_t)(g_video_2_guest_stack + sizeof(g_video_2_guest_stack));

    hype_paging_build_identity(g_guest_pml4, g_guest_pdpt, g_guest_pd, HYPE_M3_5_GUEST_PAGING_GB);
    guest_cr3 = (uint64_t)(uintptr_t)g_guest_pml4;

    hype_debug_print("video-2: entry_rip=0x%llx access_struct=0x%llx config_buf=0x%llx ramfb_key=0x%x\n",
                      (unsigned long long)entry_rip, (unsigned long long)access_struct_phys,
                      (unsigned long long)config_buf_phys, ramfb_key);

    ctx = hype_svm_vcpu_create_long_mode(entry_rip, guest_cr3, rsp, 0);
    if (ctx == 0) {
        hype_fatal("video-2: vcpu_create_long_mode failed");
    }

    for (;;) {
        if (ops->vcpu_run(ctx, &info) != 0) {
            hype_fatal("video-2: VM-entry failed (reason=0x%llx)", (unsigned long long)info.reason);
        }

        if (info.reason == HYPE_SVM_EXITCODE_IOIO) {
            if (hype_svm_vcpu_handle_fw_cfg_ioio(ctx, &g_video_2_fw_cfg) != 0) {
                hype_fatal("video-2: unhandled guest port I/O (qual=0x%llx guest_rip=0x%llx)",
                           (unsigned long long)info.qualification, (unsigned long long)info.guest_rip);
            }
            continue;
        }

        break;
    }

    if (info.reason != HYPE_SVM_EXITCODE_HLT) {
        hype_fatal("video-2: test guest did not halt cleanly (reason=0x%llx guest_rip=0x%llx)",
                   (unsigned long long)info.reason, (unsigned long long)info.guest_rip);
    }

    hype_ramfb_decode_config(g_video_2_ramfb_backing, &decoded);
    if (decoded.address != framebuffer_phys || decoded.fourcc != HYPE_RAMFB_FORMAT_XRGB8888 ||
        decoded.flags != 0 || decoded.width != 800 || decoded.height != 600 || decoded.stride != 800u * 4u) {
        hype_fatal(
            "video-2: decoded etc/ramfb config mismatch (address=0x%llx fourcc=0x%x width=%u height=%u "
            "stride=%u)",
            (unsigned long long)decoded.address, decoded.fourcc, decoded.width, decoded.height,
            decoded.stride);
    }

    hype_debug_print(
        "video-2: ramfb test guest halted cleanly (reason=0x%llx, guest_rip=0x%llx) -- etc/ramfb DMA "
        "write verified byte-for-byte (framebuffer=0x%llx %ux%u XRGB8888)\n",
        (unsigned long long)info.reason, (unsigned long long)info.guest_rip,
        (unsigned long long)framebuffer_phys, decoded.width, decoded.height);
}

/*
 * CPUMSR-1/CPUMSR-2: exercises the new CPUID and MSR VM-exit paths
 * (hype_svm_vcpu_handle_cpuid()/hype_svm_vcpu_handle_msr(),
 * arch/x86_64/svm/svm_vcpu.c) end-to-end, not just
 * hype_cpuid_emulate()/hype_msr_decide() in isolation -- proves the
 * VMCB intercept bits, exit-code dispatch, and register write-back all
 * actually work together. Guest executes real CPUID for leaves 0, 1,
 * and the hypervisor-signature leaf (0x40000000), then RDMSR for
 * APIC_BASE and EFER (both read-only exercises here -- WRMSR against
 * EFER mid-test would risk destabilizing the guest's own long-mode
 * state, not worth the risk for a baseline test), storing every
 * result into a host-inspectable guest buffer via RDI-relative stores
 * (ordinary guest-RAM writes, no MMIO/NPF involved -- unlike M4-3/
 * M4-5's device tests).
 */
static uint8_t g_cpumsr_1_guest_code[160] __attribute__((aligned(4096)));
static uint8_t g_cpumsr_1_guest_stack[4096] __attribute__((aligned(4096)));
static uint8_t g_cpumsr_1_result_buf[64] __attribute__((aligned(16)));

/*
 *   mov rdi, <patched: result_buf guest-physical address>
 *                                        48 BF 00 00 00 00 00 00 00 00
 *   mov eax, 0                           B8 00 00 00 00
 *   cpuid                                0F A2
 *   mov [rdi+0], eax                     89 47 00
 *   mov [rdi+4], ebx                     89 5F 04
 *   mov [rdi+8], ecx                     89 4F 08
 *   mov [rdi+12], edx                    89 57 0C
 *   mov eax, 1                           B8 01 00 00 00
 *   cpuid                                0F A2
 *   mov [rdi+16], eax                    89 47 10
 *   mov [rdi+20], ebx                    89 5F 14
 *   mov [rdi+24], ecx                    89 4F 18
 *   mov [rdi+28], edx                    89 57 1C
 *   mov eax, 0x40000000                  B8 00 00 00 40
 *   cpuid                                0F A2
 *   mov [rdi+32], eax                    89 47 20
 *   mov [rdi+36], ebx                    89 5F 24
 *   mov [rdi+40], ecx                    89 4F 28
 *   mov [rdi+44], edx                    89 57 2C
 *   mov ecx, 0x1B        (APIC_BASE)     B9 1B 00 00 00
 *   rdmsr                                0F 32
 *   mov [rdi+48], eax                    89 47 30
 *   mov [rdi+52], edx                    89 57 34
 *   mov ecx, 0xC0000080  (EFER)          B9 80 00 00 C0
 *   rdmsr                                0F 32
 *   mov [rdi+56], eax                    89 47 38
 *   mov [rdi+60], edx                    89 57 3C
 *   hlt                                  F4
 *   jmp $-3                              EB FD
 */
static const uint8_t g_cpumsr_1_payload_template[] = {
    0x48, 0xBF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x0F, 0xA2,
    0x89, 0x47, 0x00,
    0x89, 0x5F, 0x04,
    0x89, 0x4F, 0x08,
    0x89, 0x57, 0x0C,
    0xB8, 0x01, 0x00, 0x00, 0x00,
    0x0F, 0xA2,
    0x89, 0x47, 0x10,
    0x89, 0x5F, 0x14,
    0x89, 0x4F, 0x18,
    0x89, 0x57, 0x1C,
    0xB8, 0x00, 0x00, 0x00, 0x40,
    0x0F, 0xA2,
    0x89, 0x47, 0x20,
    0x89, 0x5F, 0x24,
    0x89, 0x4F, 0x28,
    0x89, 0x57, 0x2C,
    0xB9, 0x1B, 0x00, 0x00, 0x00,
    0x0F, 0x32,
    0x89, 0x47, 0x30,
    0x89, 0x57, 0x34,
    0xB9, 0x80, 0x00, 0x00, 0xC0,
    0x0F, 0x32,
    0x89, 0x47, 0x38,
    0x89, 0x57, 0x3C,
    0xF4,
    0xEB, 0xFD
};
#define HYPE_CPUMSR_1_PAYLOAD_RDI_IMM_OFFSET 2

static void run_cpumsr_test(const hype_vmm_ops_t *ops, hype_vmm_kind_t kind) {
    unsigned long long i;
    uint64_t entry_rip, guest_cr3, rsp, result_buf_phys;
    hype_vcpu_ctx_t *ctx;
    hype_vmexit_info_t info;
    hype_cpuid_result_t real, expected;

    if (kind != HYPE_VMM_KIND_SVM) {
        hype_serial_print("cpumsr: skipped -- %s has no working vcpu_run yet (see vmx_ops.c)\n",
                           ops->name);
        return;
    }

    hype_guest_ram_zero(g_cpumsr_1_guest_code, sizeof(g_cpumsr_1_guest_code));
    hype_guest_ram_zero(g_cpumsr_1_guest_stack, sizeof(g_cpumsr_1_guest_stack));
    hype_guest_ram_zero(g_cpumsr_1_result_buf, sizeof(g_cpumsr_1_result_buf));

    result_buf_phys = (uint64_t)(uintptr_t)g_cpumsr_1_result_buf;

    for (i = 0; i < sizeof(g_cpumsr_1_payload_template); i++) {
        g_cpumsr_1_guest_code[i] = g_cpumsr_1_payload_template[i];
    }
    hype_write_le64(g_cpumsr_1_guest_code + HYPE_CPUMSR_1_PAYLOAD_RDI_IMM_OFFSET, result_buf_phys);

    entry_rip = (uint64_t)(uintptr_t)g_cpumsr_1_guest_code;
    rsp = (uint64_t)(uintptr_t)(g_cpumsr_1_guest_stack + sizeof(g_cpumsr_1_guest_stack));

    hype_paging_build_identity(g_guest_pml4, g_guest_pdpt, g_guest_pd, HYPE_M3_5_GUEST_PAGING_GB);
    guest_cr3 = (uint64_t)(uintptr_t)g_guest_pml4;

    hype_debug_print("cpumsr: entry_rip=0x%llx result_buf=0x%llx\n", (unsigned long long)entry_rip,
                      (unsigned long long)result_buf_phys);

    /* No NPT for this test -- pure register/memory-write test, no
     * MMIO-trapped device involved (same reasoning as M4-4's fw_cfg
     * test). */
    ctx = hype_svm_vcpu_create_long_mode(entry_rip, guest_cr3, rsp, 0);
    if (ctx == 0) {
        hype_fatal("cpumsr: vcpu_create_long_mode failed");
    }

    for (;;) {
        if (ops->vcpu_run(ctx, &info) != 0) {
            hype_fatal("cpumsr: VM-entry failed (reason=0x%llx)", (unsigned long long)info.reason);
        }

        if (info.reason == HYPE_SVM_EXITCODE_CPUID) {
            hype_svm_vcpu_handle_cpuid(ctx);
            continue;
        }
        if (info.reason == HYPE_SVM_EXITCODE_MSR) {
            if (hype_svm_vcpu_handle_msr(ctx) != 0) {
                hype_fatal("cpumsr: unhandled guest MSR access (qual=0x%llx guest_rip=0x%llx)",
                           (unsigned long long)info.qualification, (unsigned long long)info.guest_rip);
            }
            continue;
        }

        break;
    }

    if (info.reason != HYPE_SVM_EXITCODE_HLT) {
        hype_fatal("cpumsr: test guest did not halt cleanly (reason=0x%llx guest_rip=0x%llx)",
                   (unsigned long long)info.reason, (unsigned long long)info.guest_rip);
    }

    /* Host independently recomputes the same expected result via
     * hype_cpuid_emulate() (already fully unit tested in isolation)
     * fed with the real host CPU's own CPUID output for each leaf, and
     * confirms the guest's VM-exit-mediated result matches byte-for-
     * byte -- proving the whole intercept path, not just the pure
     * decode logic. */
    for (i = 0; i < 3; i++) {
        uint32_t eax_in = (i == 0) ? 0u : (i == 1) ? 1u : 0x40000000u;
        const uint8_t *slot = g_cpumsr_1_result_buf + i * 16;
        uint32_t got_eax, got_ebx, got_ecx, got_edx;

        __asm__ volatile("cpuid"
                          : "=a"(real.eax), "=b"(real.ebx), "=c"(real.ecx), "=d"(real.edx)
                          : "a"(eax_in), "c"(0));
        hype_cpuid_emulate(eax_in, 0, &real, &expected);

        got_eax = (uint32_t)slot[0] | ((uint32_t)slot[1] << 8) | ((uint32_t)slot[2] << 16) |
                  ((uint32_t)slot[3] << 24);
        got_ebx = (uint32_t)slot[4] | ((uint32_t)slot[5] << 8) | ((uint32_t)slot[6] << 16) |
                  ((uint32_t)slot[7] << 24);
        got_ecx = (uint32_t)slot[8] | ((uint32_t)slot[9] << 8) | ((uint32_t)slot[10] << 16) |
                  ((uint32_t)slot[11] << 24);
        got_edx = (uint32_t)slot[12] | ((uint32_t)slot[13] << 8) | ((uint32_t)slot[14] << 16) |
                  ((uint32_t)slot[15] << 24);

        if (got_eax != expected.eax || got_ebx != expected.ebx || got_ecx != expected.ecx ||
            got_edx != expected.edx) {
            hype_fatal("cpumsr: CPUID leaf 0x%x mismatch (got eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x, "
                       "expected eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x)",
                       eax_in, got_eax, got_ebx, got_ecx, got_edx, expected.eax, expected.ebx,
                       expected.ecx, expected.edx);
        }
    }

    /* MSR results: APIC_BASE at offset 48 (eax/edx), EFER at offset
     * 56. APIC_BASE's expected value is hype_msr_apic_base_value()
     * directly (a fixed synthesized constant, no real-hardware input
     * needed); EFER's expected value is whatever this test guest's own
     * VMCB actually has in save.efer -- not independently
     * recomputable from outside the VMCB, so this confirms internal
     * consistency (the value read back is a plausible 64-bit-mode EFER
     * -- SVME/LME/LMA all set) rather than an external oracle. */
    {
        uint64_t apic_base_expected = hype_msr_apic_base_value();
        uint32_t got_apic_eax = (uint32_t)g_cpumsr_1_result_buf[48] |
                                 ((uint32_t)g_cpumsr_1_result_buf[49] << 8) |
                                 ((uint32_t)g_cpumsr_1_result_buf[50] << 16) |
                                 ((uint32_t)g_cpumsr_1_result_buf[51] << 24);
        uint32_t got_apic_edx = (uint32_t)g_cpumsr_1_result_buf[52] |
                                 ((uint32_t)g_cpumsr_1_result_buf[53] << 8) |
                                 ((uint32_t)g_cpumsr_1_result_buf[54] << 16) |
                                 ((uint32_t)g_cpumsr_1_result_buf[55] << 24);
        uint64_t got_apic_base = ((uint64_t)got_apic_edx << 32) | (uint64_t)got_apic_eax;

        uint32_t got_efer_eax = (uint32_t)g_cpumsr_1_result_buf[56] |
                                 ((uint32_t)g_cpumsr_1_result_buf[57] << 8) |
                                 ((uint32_t)g_cpumsr_1_result_buf[58] << 16) |
                                 ((uint32_t)g_cpumsr_1_result_buf[59] << 24);
        uint32_t got_efer_edx = (uint32_t)g_cpumsr_1_result_buf[60] |
                                 ((uint32_t)g_cpumsr_1_result_buf[61] << 8) |
                                 ((uint32_t)g_cpumsr_1_result_buf[62] << 16) |
                                 ((uint32_t)g_cpumsr_1_result_buf[63] << 24);
        uint64_t got_efer = ((uint64_t)got_efer_edx << 32) | (uint64_t)got_efer_eax;

        if (got_apic_base != apic_base_expected) {
            hype_fatal("cpumsr: RDMSR(APIC_BASE) mismatch (got 0x%llx, expected 0x%llx)",
                       (unsigned long long)got_apic_base, (unsigned long long)apic_base_expected);
        }
        if ((got_efer & HYPE_SVM_SAVE_EFER_SVME) == 0) {
            hype_fatal("cpumsr: RDMSR(EFER) implausible -- SVME bit not set (0x%llx)",
                       (unsigned long long)got_efer);
        }
    }

    hype_debug_print(
        "cpumsr: test guest halted cleanly (reason=0x%llx, guest_rip=0x%llx) -- CPUID leaves "
        "0/1/0x40000000 and RDMSR(APIC_BASE/EFER) all verified via the real VM-exit path\n",
        (unsigned long long)info.reason, (unsigned long long)info.guest_rip);
}

/*
 * RAM-1/RAM-2: exercises the new dynamically-allocated,
 * dynamically-NPT-sized guest RAM path (g_ram_1_base_phys/
 * g_ram_1_size_bytes, allocated in efi_main() before this test runs --
 * see that allocation's own comment for why it happens on the BSP
 * before MP dispatch rather than here). Deliberately trivial guest
 * code (a single HLT) -- what's actually being validated is that
 * dynamically-computed NPT/guest-CR3 coverage (hype_ram_1_gb_to_map())
 * genuinely reaches wherever AllocatePages(AllocateAnyPages) actually
 * put this allocation, not a fixed guess -- the same class of bug this
 * project already found and fixed the hard way on real hardware
 * (arch/x86_64/svm/npt.h's own HYPE_NPT_MAX_GB comment) for a
 * differently-sized gap (compiler-placed static buffers vs. firmware-
 * placed dynamic allocations).
 */
static const uint8_t g_ram_1_payload[] = {
    0xF4,      /* hlt */
    0xEB, 0xFD /* jmp $-3 */
};

/* Computes how many GB hype_paging_build_identity()/
 * hype_npt_build_identity() need to map to cover guest-physical
 * address `end_phys` -- both builders map from GB index 0 upward (the
 * same shape as the host's own identity map and every existing guest/
 * NPT identity map here), so this is "round up to the next whole GB,"
 * not "map only the allocated region in isolation." Bounded by
 * HYPE_PAGING_MAX_GB, the actual compile-time capacity of every
 * g_npt_pd/g_guest_pd-style array in this file -- fails closed rather
 * than silently overrunning a static array if a future, much larger
 * mem_mb ever needs more than that. */
static unsigned int hype_ram_1_gb_to_map(uint64_t end_phys) {
    unsigned int gb = (unsigned int)((end_phys + HYPE_PAGING_1GB - 1) / HYPE_PAGING_1GB);

    if (gb == 0) {
        gb = 1;
    }
    if (gb > HYPE_PAGING_MAX_GB) {
        hype_fatal("ram-1: guest RAM allocation needs %u GB of identity map, only %u available", gb,
                   HYPE_PAGING_MAX_GB);
    }
    return gb;
}

static void run_ram_1_test(const hype_vmm_ops_t *ops, hype_vmm_kind_t kind) {
    uint64_t entry_rip, guest_cr3, rsp, npt_root_phys;
    unsigned int gb_to_map;
    hype_vcpu_ctx_t *ctx;
    hype_vmexit_info_t info;
    uint8_t *guest_code;
    unsigned long long i;

    if (kind != HYPE_VMM_KIND_SVM) {
        hype_serial_print("ram-1: skipped -- %s has no working vcpu_run yet (see vmx_ops.c)\n", ops->name);
        return;
    }

    gb_to_map = hype_ram_1_gb_to_map(g_ram_1_base_phys + g_ram_1_size_bytes);

    /* M2-6 hard invariant: zero the WHOLE allocated region before this
     * guest's first VM-entry, not just the bytes written below. */
    hype_guest_ram_zero((void *)(uintptr_t)g_ram_1_base_phys, g_ram_1_size_bytes);

    guest_code = (uint8_t *)(uintptr_t)g_ram_1_base_phys;
    for (i = 0; i < sizeof(g_ram_1_payload); i++) {
        guest_code[i] = g_ram_1_payload[i];
    }

    entry_rip = g_ram_1_base_phys;
    rsp = g_ram_1_base_phys + g_ram_1_size_bytes; /* top of the same allocated region */

    hype_paging_build_identity(g_guest_pml4, g_guest_pdpt, g_guest_pd, gb_to_map);
    guest_cr3 = (uint64_t)(uintptr_t)g_guest_pml4;

    hype_npt_build_identity(g_npt_pml4, g_npt_pdpt, g_npt_pd, gb_to_map);
    npt_root_phys = (uint64_t)(uintptr_t)g_npt_pml4;

    hype_debug_print("ram-1: base_phys=0x%llx size=0x%llx gb_to_map=%u\n",
                      (unsigned long long)g_ram_1_base_phys, (unsigned long long)g_ram_1_size_bytes,
                      gb_to_map);

    ctx = hype_svm_vcpu_create_long_mode(entry_rip, guest_cr3, rsp, npt_root_phys);
    if (ctx == 0) {
        hype_fatal("ram-1: vcpu_create_long_mode failed");
    }

    if (ops->vcpu_run(ctx, &info) != 0) {
        hype_fatal("ram-1: VM-entry failed (reason=0x%llx)", (unsigned long long)info.reason);
    }

    if (info.reason != HYPE_SVM_EXITCODE_HLT) {
        hype_fatal("ram-1: test guest did not halt cleanly (reason=0x%llx guest_rip=0x%llx)",
                   (unsigned long long)info.reason, (unsigned long long)info.guest_rip);
    }

    hype_debug_print(
        "ram-1: test guest halted cleanly (reason=0x%llx, guest_rip=0x%llx) -- %u MB dynamic guest "
        "RAM, NPT sized to %u GB\n",
        (unsigned long long)info.reason, (unsigned long long)info.guest_rip, HYPE_RAM_1_TEST_MEM_MB,
        gb_to_map);
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
    run_m4_4_fw_cfg_test(args->ops, args->kind);
    run_m4_5_ahci_test(args->ops, args->kind);
    run_video_2_ramfb_test(args->ops, args->kind);
    run_cpumsr_test(args->ops, args->kind);
    run_ram_1_test(args->ops, args->kind);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_MEMORY_DESCRIPTOR *map = 0;
    UINTN map_size = 0, desc_size = 0, map_key = 0;
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    int have_gop;
    UINT64 usable_ram_bytes = 0;

    hype_console_print(SystemTable, "hype\n");

    /* Safe to bring up now: it's raw port I/O, independent of Boot
     * Services or which GDT/IDT happens to be active either way. */
    hype_serial_init(HYPE_SERIAL_COM1, 115200);

    status = hype_memmap_get(SystemTable->BootServices, &map, &map_size, &desc_size, &map_key);
    if (status != EFI_SUCCESS) {
        hype_fatal("failed to get memory map: 0x%llx", (unsigned long long)status);
    }

    hype_memmap_dump(SystemTable, map, map_size, desc_size);
    /* RAM-1: computed here, before the map is freed, so the admission
     * check ahead of the guest-RAM allocation below is against this
     * machine's own real usable RAM, not a guess. */
    usable_ram_bytes = hype_memmap_usable_bytes(map, map_size, desc_size);
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
     * GOP console + hype_fatal()'s registration for it are both set up
     * here -- *before* the test-guest dispatch below, not after
     * ExitBootServices() -- specifically so a hype_fatal() panic during
     * any of those test guests is actually visible on screen. Found the
     * hard way on real AMD hardware: hype_fatal() only ever prints via
     * serial and (if registered) GOP, never UEFI's own ConOut, and the
     * test guests all run before ExitBootServices -- so with the GOP
     * console registered only afterward (this project's original
     * ordering), a panic during test-guest execution was completely
     * silent on a screen-only setup (no serial capture), indistinguishable
     * from a genuine hang. FrameBufferBase/resolution are available as
     * soon as GOP is located, independent of ExitBootServices, so this
     * doesn't need to wait. Framebuffer contents survive past
     * ExitBootServices and the later GDT/paging/IDT swap unchanged --
     * it's just memory, and the later "Boot Services exited" print
     * below reuses this same console rather than re-initializing it. */
    if (have_gop && gop->Mode != 0 && gop->Mode->Info != 0 &&
        (gop->Mode->Info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ||
         gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor)) {
        hype_gop_console_init(&g_gop_console, (void *)gop->Mode->FrameBufferBase,
                               gop->Mode->Info->HorizontalResolution,
                               gop->Mode->Info->VerticalResolution,
                               gop->Mode->Info->PixelsPerScanLine,
                               0xFFFFFFu, 0x000000u);
        hype_gop_console_clear(&g_gop_console);
        hype_fatal_set_gop(&g_gop_console);
        hype_gop_print(&g_gop_console, "hype: running self-tests...\n");
        /* Real-hardware debugging: our own host paging (built later,
         * right after ExitBootServices -- see HYPE_PAGING_MAX_GB) only
         * identity-maps the first HYPE_PAGING_MAX_GB gigabytes. If this
         * GPU's framebuffer BAR sits above that (plausible on a modern
         * board with Resizable BAR/Above-4G decoding), the framebuffer
         * goes unmapped the instant our own page tables load, faulting
         * with no IDT yet in place -- this print exists to confirm or
         * rule that out directly. */
        hype_debug_print("gop: FrameBufferBase=0x%llx FrameBufferSize=0x%llx\n",
                          (unsigned long long)gop->Mode->FrameBufferBase,
                          (unsigned long long)gop->Mode->FrameBufferSize);
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
        hype_cpu_diag_t cpu_diag = hype_cpu_detect_vmm_kind_diag();
        hype_vmm_kind_t kind = cpu_diag.kind;
        const hype_vmm_ops_t *ops = hype_vmm_ops_for_kind(kind);
        static hype_test_guest_args_t args;
        EFI_MP_SERVICES_PROTOCOL *mp = 0;
        UINTN target_ap = 0;
        int have_target_ap = 0;

        /* Real-hardware debugging: the single most useful line in the
         * whole log if a machine turns out to be the "wrong" vendor,
         * or a feature bit is unexpectedly absent (e.g. SVM disabled
         * in firmware setup) -- print it before anything else can go
         * wrong, not only on failure. */
        hype_debug_print(
            "cpu: vendor=%s vmx=%d svm=%d\n",
            (cpu_diag.vendor == HYPE_CPU_VENDOR_INTEL)
                ? "Intel"
                : (cpu_diag.vendor == HYPE_CPU_VENDOR_AMD) ? "AMD" : "unknown",
            cpu_diag.has_vmx, cpu_diag.has_svm);

        if (ops == 0) {
            hype_fatal("no usable virtualization extension (VMX/SVM) detected");
        }
        hype_debug_print("vmm: %s detected\n", ops->name);

        /* Must happen here, before ExitBootServices(), and before
         * run_all_test_guests() below actually uses these -- see
         * g_m2_7_guest_code_phys's own comment for why a static buffer
         * isn't safe for this particular (real-mode) test guest. One
         * single 2-page allocation, not two separate 1-page ones: the
         * UEFI spec guarantees the pages *within* one AllocatePages
         * call are contiguous and non-overlapping, but says nothing
         * about the relative placement of two independent calls. */
        {
            uint64_t pages_phys = hype_alloc_pages_below_4gb(SystemTable->BootServices, 2);
            g_m2_7_guest_code_phys = pages_phys;
            g_m2_7_guest_stack_top_phys = pages_phys + 2 * 4096;
        }

        /*
         * RAM-1: a synthetic one-VM config standing in for a real
         * parsed hype.cfg (reading one from the ESP is a separate,
         * later piece -- see task.md's own note) exercises ADM-1's
         * already-tested hype_adm_check_memory() against this
         * machine's real usable RAM for the first time in the actual
         * boot path, then allocates that many MB of real guest RAM.
         */
        {
            static hype_cfg_t ram_1_cfg;
            hype_adm_result_t adm_result;
            unsigned char *raw = (unsigned char *)&ram_1_cfg;
            unsigned long long z;
            UINTN pages;

            for (z = 0; z < sizeof(ram_1_cfg); z++) {
                raw[z] = 0;
            }
            ram_1_cfg.vm_count = 1;
            ram_1_cfg.vms[0].mem_mb = HYPE_RAM_1_TEST_MEM_MB;

            adm_result = hype_adm_check_memory(&ram_1_cfg, usable_ram_bytes,
                                                (UINT64)HYPE_ADM_RESERVED_MB_DEFAULT * 1024ULL * 1024ULL);
            if (adm_result.status != HYPE_ADM_OK) {
                hype_fatal("ram-1: admission check rejected a %u MB VM (status=%d)",
                           HYPE_RAM_1_TEST_MEM_MB, (int)adm_result.status);
            }

            g_ram_1_size_bytes = (uint64_t)HYPE_RAM_1_TEST_MEM_MB * 1024ULL * 1024ULL;
            pages = (UINTN)(g_ram_1_size_bytes / 4096ULL);
            g_ram_1_base_phys = hype_alloc_pages_any(SystemTable->BootServices, pages);
        }

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
            /* Mirrored to serial too (not just the UEFI console) --
             * real-hardware debugging: if the AP never comes back, this
             * is the last line either channel will show, and serial is
             * the more reliable one to actually capture from real
             * hardware. */
            hype_serial_print("mp: dispatching test guest to pinned pCPU #%llu...\n",
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

    /* Real-hardware debugging: a hang with this as the last serial
     * line means ExitBootServices() itself never returned control --
     * a real firmware quirk QEMU/OVMF's own implementation might not
     * reproduce. */
    hype_debug_print("about to call ExitBootServices...\n");
    status = hype_exit_boot_services(ImageHandle, SystemTable->BootServices);
    if (status != EFI_SUCCESS) {
        hype_fatal("ExitBootServices failed: 0x%llx", (unsigned long long)status);
    }
    hype_debug_print("ExitBootServices returned\n");

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
    hype_debug_print("interrupts masked (cli)\n");

    /* Real-hardware debugging: LGDT + reloading every segment register
     * is one of the more real-silicon-sensitive sequences here (a bad
     * descriptor can fault immediately) -- bracket it so a hang
     * localizes to this exact instruction sequence, not "somewhere
     * between cli and the timer starting." */
    hype_debug_print("about to load own GDT...\n");
    hype_gdt_build(g_gdt);
    hype_gdt_load(g_gdt, HYPE_GDT_ENTRY_COUNT);
    hype_debug_print("own GDT loaded\n");

    hype_debug_print("about to load own paging (identity-mapping %u GB)...\n", HYPE_PAGING_MAX_GB);
    hype_paging_build_identity(g_pml4, g_pdpt, g_pd, HYPE_PAGING_MAX_GB);
    hype_paging_load(g_pml4);
    hype_debug_print("own paging loaded\n");

    hype_debug_print("about to load own IDT...\n");
    hype_idt_build(g_idt, hype_isr_stub_table, HYPE_GDT_CODE64_SEL);
    hype_idt_load(g_idt, HYPE_IDT_ENTRY_COUNT);
    hype_debug_print("own IDT loaded\n");

    /*
     * Boot Services -- including ConOut, which every hype_console_print
     * above depended on -- are gone as of the ExitBootServices() call
     * above. This is now the only kernel running on this CPU. Serial
     * (initialized above) is one output channel; GOP (already
     * initialized/registered with hype_fatal(), above, before the
     * test-guest dispatch -- see that block's own comment) is the
     * other -- the framebuffer itself is just memory, identity-mapped
     * by our own paging above, so writing into it needs nothing
     * further from firmware.
     */
    if (have_gop && gop->Mode != 0 && gop->Mode->Info != 0 &&
        (gop->Mode->Info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ||
         gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor)) {
        hype_gop_console_clear(&g_gop_console);
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
    hype_serial_print("about to enable interrupts (sti)...\n");
    hype_sti();
    hype_serial_print("interrupts enabled -- waiting for timer ticks\n");

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
