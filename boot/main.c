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
     * run_test_guest() does depends on our own GDT/IDT/paging/timer
     * (see its own comment), so there's no reason it needs to run
     * after ExitBootServices at all. No extra pCPU (or no MP services
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
            /* WaitEvent=0/NULL => blocking: waits for run_test_guest()
             * to return, which it always does on success. On a fatal
             * path inside it (hype_fatal() -> hype_halt_forever(),
             * never returns), this call -- and so the whole boot --
             * blocks forever on that core too; the diagnostic message
             * fatal() already printed to serial is what actually
             * matters for debugging a genuinely unrecoverable
             * condition, so this is an accepted tradeoff, not a gap. */
            status = mp->StartupThisAP(mp, run_test_guest, target_ap, 0, 0, &args, &finished);
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
            run_test_guest(&args);
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
