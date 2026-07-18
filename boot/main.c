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
#include "../core/file_io.h"
#include "../core/logbuf.h"
#include "../arch/x86_64/cpu/cpu_features.h"
#include "../arch/x86_64/cpu/gdt.h"
#include "../arch/x86_64/cpu/idt.h"
#include "../arch/x86_64/cpu/isr.h"
#include "../arch/x86_64/cpu/lapic.h"
#include "../arch/x86_64/cpu/paging.h"
#include "../arch/x86_64/cpu/pic.h"
#include "../arch/x86_64/cpu/pit.h"
#include "../arch/x86_64/cpu/timer.h"
#include "../arch/x86_64/cpu/ps2_host.h"
#include "../arch/x86_64/cpu/leader_chord.h"
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
#include "../devices/fb_blit.h"
#include "../devices/e820.h"
#include "../devices/vt_filter.h"

/* Static storage: still valid (and unmoving) once these get built and
 * loaded, after ExitBootServices() below. */
static hype_gdt_entry_t g_gdt[HYPE_GDT_ENTRY_COUNT];
static hype_idt_entry_t g_idt[HYPE_IDT_ENTRY_COUNT];
static hype_pte_t g_pml4[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_pte_t g_pdpt[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_pte_t g_pd[HYPE_PAGING_MAX_GB][HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
/* Extra PD tables for mapping a GOP framebuffer BAR that firmware placed
 * in high MMIO above the low identity map (e.g. 256GB on Intel client
 * parts). Two tables cover a framebuffer that straddles a 1GB boundary. */
static hype_pte_t g_fb_pd[2][HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
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

/*
 * FW-1: this project's own vendored guest firmware (M4-2), read from
 * the same ESP hype.efi was itself booted from (core/file_io.h).
 *
 * Found the hard way, on the very first attempt: OVMF_CODE.fd/
 * OVMF_VARS.fd's own internal addressing assumes the classic x86/QEMU
 * convention of being mapped ending exactly at guest-physical 4GB
 * (0xFFFFFFF0, the reset vector) -- but that is NOT safe/available
 * *host*-physical memory to allocate real content into. Whether
 * running nested under another hypervisor (this project's own dev
 * environment, where that exact host-physical range turned out to be
 * the L0 host's own real OVMF flash -- writing into it corrupted the
 * host firmware and crashed it with a #PF) or on real hardware (the
 * literal top-of-4GB range is the motherboard's own real BIOS/UEFI
 * flash chip, not general-purpose RAM either), that address is real,
 * fixed hardware, not something this project can repurpose.
 *
 * Fixed with NPT remapping (hype_npt_map_range(), arch/x86_64/svm/
 * npt.h): the combined CODE+VARS content is loaded into one ordinary,
 * freely-allocated (AllocateAnyPages) host-physical buffer -- wherever
 * that actually is -- and NPT translates guest-physical
 * [0x100000000 - (code_size+vars_size), 0x100000000) to THAT host
 * address instead of identity-mapping it. The guest still sees its
 * own reset vector at the architecturally-correct 0xFFFFFFF0; only the
 * underlying host-physical backing differs from a plain identity map.
 * VARS.fd is placed first in the combined buffer, immediately followed
 * by CODE.fd, matching their real relative guest-physical layout
 * (VARS immediately below CODE).
 *
 * Scope note (deliberate, for this first real-OVMF launch attempt):
 * the whole combined region is ordinary present, executable guest
 * memory -- NOT wiring VARS through M4-3's pflash MMIO-trap model yet.
 * Real CODE+VARS placement (VARS immediately below CODE, neither file
 * a round 2MB multiple) means the two regions don't align to this
 * project's own NPT granularity (2MB PS=1 leaf entries only, no 4KB PT
 * level exists in this paging model) -- VARS's own not-present region
 * would inevitably bleed into CODE's own first ~1.4MB, breaking its
 * execute-in-place fetches. Reconciling that is real, separate design
 * work; deferred until this first attempt confirms whether the reset-
 * vector-launch mechanism itself even works and shows what real
 * OVMF's SEC phase actually touches first -- SEC-phase code runs
 * strictly XIP from ROM for quite a while before DXE-phase variable
 * services would ever touch VARS, so this is unlikely to matter yet.
 *
 * Loaded once, on the BSP in efi_main() before MP dispatch (same
 * ordering/reasoning as g_m2_7_guest_code_phys/g_ram_1_base_phys
 * above -- Boot Services calls from a non-BSP AP context is untested
 * territory this project has deliberately avoided all session).
 */
static uint64_t g_fw_1_combined_host_phys;
static uint64_t g_fw_1_combined_size;
/*
 * FW-1a: the guest's own low RAM, backed by a real, freely-allocated
 * host buffer and NPT-mapped at guest-physical [0, size) -- NOT the flat
 * identity map used before, which handed the guest the host's real
 * sub-4GB MMIO hole (reads all-1s) wherever OVMF placed its DXE stack,
 * so OVMF read a garbage pointer off its own stack and jumped to -1 (see
 * task.md's FW section). Size is fixed well below the Q35 32-bit MMIO
 * hole base (PcdPciExpressBaseAddress = 0xE0000000, which OVMF asserts
 * low RAM stays under) and far above OVMF's ~26MB fixed early footprint
 * (SEC page tables/temp RAM + PEIFV/DXEFV shadows + decompression
 * scratch, up to 0x1A10000). 1 GiB is a safe contiguous allocation and
 * ample for reaching the OVMF shell; bump it (and it alone) when a later
 * milestone needs a bigger guest. Must stay 2MB-aligned (NPT granularity)
 * and <= 0xE0000000. */
#define HYPE_FW_1_GUEST_RAM_BYTES (1024ULL * 1024ULL * 1024ULL) /* 1 GiB */
static uint64_t g_fw_1_ram_host_phys;
/* One usable e820 entry (20 bytes) is all FW-1 registers today; sized
 * for a handful in case a reserved region is ever added. */
static uint8_t g_fw_1_e820_blob[HYPE_E820_ENTRY_SIZE * 8];
/* ISO-1's own loaded test.iso buffer, kept around for ISO-2 to back
 * the AHCI/ATAPI model with real data instead of re-reading the file a
 * second time. */
static uint64_t g_iso_host_phys;
static uint64_t g_iso_size;
/* Set once in efi_main() from the real UEFI memory map -- FW-1's own
 * CMOS model reports this (not a guessed/fixed value) as the guest's
 * memory size, matching how a real VM's memory-size discovery ought to
 * reflect what's actually available. */
static uint64_t g_usable_ram_bytes;
static uint64_t g_fw_1_code_size;
static uint64_t g_fw_1_vars_size;
static uint8_t g_fw_1_guest_stack[65536] __attribute__((aligned(4096)));
static hype_pic_emu_t g_fw_1_pic;
static hype_pit_emu_t g_fw_1_pit;
static hype_pci_t g_fw_1_pci;
static hype_cmos_t g_fw_1_cmos;
static hype_guest_lapic_t g_fw_1_lapic; /* FW-1b: guest Local APIC (0xFEE00000) */
static hype_guest_uart_t g_fw_1_uart;  /* FW-1e: guest 16550 UART (COM1 0x3F8) */
static hype_guest_uart_t g_fw_1_uart2; /* FW-1e: guest 16550 UART (COM2 0x2F8) -- OVMF probes/uses both */
#define HYPE_SERIAL_COM2 0x2F8u
static hype_ps2_kbd_t g_fw_1_ps2;     /* FW-1f: guest PS/2 keyboard (0x60/0x64) -- OVMF's actual ConIn */
static hype_ps2_mouse_t g_fw_1_mouse; /* required by the shared PS/2 IOIO handler signature */
#define HYPE_FW_1_KEY_ENTER_MAKE 0x1Cu /* Set-1 make code for Enter */
#define HYPE_FW_1_DEBUG_PORT 0x402u    /* FW-1g: OVMF SEC/PEI PlatformDebugLibIoPort */
static hype_fw_cfg_t g_fw_1_fw_cfg;
static hype_acpi_rsdp_t g_fw_1_rsdp;
static uint8_t g_fw_1_tables_blob[4096] __attribute__((aligned(64)));
static hype_acpi_loader_entry_t g_fw_1_loader_script[HYPE_ACPI_LOADER_SCRIPT_ENTRIES];
/* FW-1h: a real AHCI/ATAPI CD-ROM controller so OVMF's BDS finds a
 * bootable optical drive (ISO-1's loaded \iso\test.iso). Device model
 * (M4-5), PCI discovery (PCI-2), and the real-ISO backing (ISO-2) all
 * already exist and are individually tested; FW-1h only integrates them
 * into the real-OVMF guest. The ATAPI model is backed by ISO-1's own
 * loaded buffer (g_iso_host_phys/g_iso_size), same as run_iso_2_test. */
static hype_ahci_t g_fw_1_ahci;
static hype_atapi_t g_fw_1_atapi;
/* VALID-1/VALID-3: FW-1's guest-physical -> host layout (RAM + flash),
 * used to bounds-check every guest-supplied AHCI DMA address. */
static hype_gpa_map_t g_fw_1_dma_map;
/* M4-6b1: measured real host TSC frequency (Hz), calibrated once in
 * efi_main via a Boot-Services Stall. The FW-1 loop converts real TSC
 * deltas into guest PIT/LAPIC-timer ticks at the 8254's 1.193182 MHz
 * so the guest's TSC/timer calibration lands at the true CPU frequency
 * instead of a nonsense value. 0 until calibrated (loop then falls back
 * to a single tick per exit). */
static uint64_t g_fw_1_host_tsc_hz;
/* M4-6d4: VMRUN-vs-loop-body wall-clock split (TSC), to localise the
 * real-HW per-exit cost. Accumulated in the FW-1 loop, dumped in EXHIST. */
static uint64_t g_fw_1_vmrun_tsc;
static uint64_t g_fw_1_body_tsc;
static uint64_t g_fw_1_prev_post_tsc;
/* M4-6d4 measurement: wall-clock (TSC) during which the timer IRQ0 was
 * pending+unmasked, an ISR was in service (so the strict "both ISRs clear"
 * delivery gate blocked it), AND the guest could accept it (IF=1, no
 * shadow) -- i.e. exactly the time a fair priority-preemption scheme would
 * RECLAIM. Compared against total wall-clock this says how much of the
 * soft-lockup slowness the tick fix can actually recover. */
static uint64_t g_fw_1_irq0_recoverable_tsc;
/* Broader companion: total wall-clock a timer IRQ0 edge is pending+unmasked
 * but NOT yet delivered, for ANY reason (in-service gate, IF=0, shadow).
 * This is the upper bound on tick lateness -- how much faster delivery
 * could reclaim in total. Comparing the two: recoverable≈pending means the
 * in-service gate is the whole story (priority fix recovers it all);
 * recoverable<<pending means most lateness is the guest itself masking
 * interrupts (IF=0), which no delivery change can fix. */
static uint64_t g_fw_1_irq0_pending_tsc;
static uint64_t g_fw_1_stall_prev_tsc;
/* M4-6d4: longest single VMRUN (guest ran this long before voluntarily
 * exiting) + count of VMRUNs over 100ms. A big max = the guest runs long
 * non-intercepting stretches we can't preempt -> a host preemption timer
 * (INTERCEPT_INTR + physical periodic timer) is the fix. */
static uint64_t g_fw_1_vmrun_max_tsc;
static unsigned long long g_fw_1_vmrun_over100ms;
#define HYPE_PIT_HZ 1193182ULL
/* Bus 0 slot for the AHCI function -- free (MCH is dev 0, ICH9 LPC is
 * dev 31). OVMF's PciBusDxe enumerates it, sizes BAR5, assigns it a
 * guest-physical address in the 32-bit PCI MMIO aperture, and enables
 * Memory Space -- exactly the PCI-2 discovery path. */
#define HYPE_FW_1_PCI_DEV_AHCI 2u

/* Intel Q35 MCH (the real Q35 chipset's host bridge) vendor/device ID
 * -- transcribed from this project's own vendored edk2 submodule,
 * edk2/OvmfPkg/Include/IndustryStandard/Q35MchIch9.h
 * (INTEL_Q35_MCH_DEVICE_ID). Confirmed via source-level investigation
 * (edk2/OvmfPkg/PlatformPei/Platform.c) that OVMF reads this exact
 * device ID via the legacy CF8/CFC ports to decide it's running on a
 * genuine PC-compatible platform; without a real host bridge here, an
 * unhandled read absorbs as all-1s (0xFFFF), which OVMF's own platform
 * detection treats as the QEMU "microvm" machine-type sentinel instead
 * -- sending it down a completely different, unsupported init path. */
#define HYPE_FW_1_PCI_VENDOR_ID_INTEL 0x8086u
#define HYPE_FW_1_PCI_DEVICE_ID_Q35_MCH 0x29C0u

/* ICH9 LPC bridge (the Q35 chipset's south bridge, bus 0/device 31/
 * function 0 -- Q35MchIch9.h's own POWER_MGMT_REGISTER_Q35() macro,
 * "B/D/F/Type: 0/0x1f/0/PCI"), a real Intel device ID (ICH9 LPC
 * Interface Controller). Confirmed via source-level investigation
 * (edk2/OvmfPkg/Library/PlatformInitLib/Platform.c's
 * PciAndThenOr32(Pmba, ...) programming ICH9_PMBASE, config offset
 * 0x40) that PlatformPei writes the ACPI PM Timer's I/O port base
 * here -- without this device registered, that write was silently
 * dropped (hype_pci_config_write()'s own "write to an absent device is
 * dropped" convention), so AcpiTimerLib's later PciRead32(Pmba) saw
 * the absent-device all-1s default instead: 0xFFFFFFFF & ~PMBA_RTE(1)
 * = 0xFFFFFFFE, + ACPI_TIMER_OFFSET(8), truncated to UINT32 =
 * 0x00000006 -- an exact match for the observed port-0x6 infinite poll
 * loop (AcpiTimerLib's own GetPerformanceCounter()). */
#define HYPE_FW_1_PCI_DEV_ICH9_LPC 31u
#define HYPE_FW_1_PCI_DEVICE_ID_ICH9_LPC 0x2918u

/* FW-1c: guest-physical base of the PCI MMCONFIG (ECAM) window. Must
 * match the base FW-1's ACPI MCFG table advertises (cfg.mcfg_base_
 * address below) AND OVMF's Q35 PcdPciExpressBaseAddress default, since
 * OVMF derives its ECAM accesses from that PCD, not from MCFG. Left
 * not-present by FW-1a's NPT map, so ECAM MMIO traps here as an NPF and
 * is serviced by PCI-1's own config-space model. */
#define HYPE_FW_1_ECAM_GPA 0xE0000000ULL

/* FW-1d: OVMF never executes HLT during DXE/BDS init -- its idle wait
 * (CpuSleep) HLTs only once everything is up. Empirically it reaches
 * that idle HLT after ~3600 VM-exits; treat a HLT past this many
 * *productive* (non-HLT) exits as "firmware booted" and finish the
 * bring-up test cleanly. Comfortably below the observed count and far
 * above any early boot activity (which does not HLT). */
#define HYPE_FW_1_BOOTED_EXITS 1500ULL
/* Runaway guard: last-resort ceiling on total VM-exits. Raised for the
 * M4-6 OS-boot case -- a real kernel booting under nested emulation does
 * far more than the ~5M an OVMF bring-up needed (initcalls, the libata
 * probe, unpacking + reading the rootfs at scale all run for tens of
 * millions of exits). The PRIMARY stop for a booting OS is the
 * wall-clock idle detector (HYPE_FW_1_IDLE_GIVEUP_SECONDS) below and, in
 * QEMU, the `make run` timeout; this ceiling only catches a true runaway
 * on real hardware where neither of those applies. */
#define HYPE_FW_1_MAX_EXITS 200000000ULL
/* M4-6d2b: a booting OS legitimately idle-HLTs for real-time stretches
 * (e.g. libata's COMRESET + ATA reset delay, waiting on the clockevent)
 * -- that is NOT a hang, unlike OVMF which only HLTs once booted. So the
 * "guest is quiescent, stop" decision is made on WALL-CLOCK time since
 * the last productive (non-HLT) exit, not on an exit count: the HLT/VMRUN
 * idle spin rate is far too high and variable to threshold by count. If
 * the guest does zero productive work for this many seconds it has either
 * reached a stable idle (a login/shell prompt) or genuinely hung -- stop
 * and report either way. Comfortably longer than any single in-boot wait,
 * bounded so a run doesn't burn minutes idling at the end. */
#define HYPE_FW_1_IDLE_GIVEUP_SECONDS 10ULL
/* FW-1f: evidence-based reaction detection for an injected keystroke.
 * "Reacted" = OVMF leaves its idle wait and does at least this many
 * PRODUCTIVE (non-HLT) exits after the key -- entering the Boot Manager
 * Menu is thousands of exits of work. (We can't key off *console* chars:
 * the menu is a full-screen TUI that's almost all VT escapes, which our
 * filter strips -- rendering it is the TERM milestone.) If instead the
 * guest stays idle for HYPE_FW_1_KEY_WAIT_EXITS total VM-exits after the
 * key, it didn't consume it on a ConIn source we feed -- don't claim it
 * advanced. */
#define HYPE_FW_1_KEY_REACTION_CHARS 40ULL  /* new console chars after the key => menu rendered => reacted */
#define HYPE_FW_1_KEY_REARM_INTERVAL 256ULL /* re-arm the scancode every N exits (leaves OBF-clear windows) */
#define HYPE_FW_1_KEY_WAIT_EXITS 20000ULL   /* give up waiting for a reaction after this many exits post-key */
/* FW-1g: inject a key once the guest has done this many *consecutive*
 * empty keyboard status polls -- an interactive prompt busy-waiting for
 * input (e.g. the UEFI Shell's "press any key to continue" countdown
 * polls port 0x64 thousands of times). Keyboard init interleaves data
 * reads/command writes, which reset the run, so a run this long is
 * unambiguously an idle input-wait, not init. */
#define HYPE_FW_1_KBD_POLL_INJECT_RUN 512ULL

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

/* Same as hype_write_le32() above, but for a 2-byte field -- used for
 * virtq_desc's own 16-bit flags/next fields (M5-1). */
static void hype_write_le16(unsigned char *dst, uint16_t value) {
    dst[0] = (unsigned char)(value & 0xFFu);
    dst[1] = (unsigned char)((value >> 8) & 0xFFu);
}

/* M4-6b1: the real host TSC, for driving the guest timebase from real
 * elapsed time rather than one tick per VM-exit. */
static inline uint64_t hype_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* Reads back a 4-byte little-endian value a test guest wrote directly
 * into its own (unintercepted, ordinarily-mapped) memory -- e.g.
 * VIDEO-3's framebuffer pixels, which take no VM-exit to write, so
 * there is no device-model read path to go through for verification. */
static uint32_t hype_read_le32(const unsigned char *src) {
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
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

/*
 * Like hype_alloc_pages_any(), but guarantees the returned host-physical
 * address is 2MB-aligned. hype_npt_map_range() (arch/x86_64/svm/npt.h)
 * encodes its host_phys_base into PS=1 (2MB) PDEs, which require the
 * address's low 21 bits to be zero -- UEFI's AllocatePages only ever
 * guarantees 4KB alignment, so a plain hype_alloc_pages_any() call here
 * silently produces a PDE with garbage in its reserved low bits, which
 * hardware treats as a page-level protection violation (an NPF on the
 * very first access, not a not-present fault) rather than anything
 * obviously "unaligned". Over-allocates by up to one extra 2MB granule
 * and returns the aligned address within it; the leading unaligned pages
 * are simply left allocated and unused (never freed -- this hypervisor
 * never exits).
 */
static uint64_t hype_alloc_pages_any_2mb_aligned(EFI_BOOT_SERVICES *bs, uint64_t size) {
    UINTN pages = (UINTN)((size + HYPE_PAGING_2MB) / 4096ULL);
    uint64_t raw = hype_alloc_pages_any(bs, pages);
    return (raw + HYPE_PAGING_2MB - 1) & ~(HYPE_PAGING_2MB - 1);
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

#define HYPE_ISO_2_AHCI_GPA (HYPE_M4_5_AHCI_GPA + HYPE_PAGING_2MB)
#define HYPE_ISO_2_PVD_LBA 16 /* ISO9660 Primary Volume Descriptor: always the 17th 2048-byte sector */

static uint8_t g_iso_2_cmd_list[4096] __attribute__((aligned(4096)));
static uint8_t g_iso_2_cmd_table[4096] __attribute__((aligned(4096)));
static uint8_t g_iso_2_rx_fis[4096] __attribute__((aligned(4096)));
static uint8_t g_iso_2_dest_buffer[HYPE_ATAPI_SECTOR_SIZE] __attribute__((aligned(4096)));
static uint8_t g_iso_2_guest_code[256] __attribute__((aligned(4096)));
static uint8_t g_iso_2_guest_stack[4096] __attribute__((aligned(4096)));
static hype_ahci_t g_iso_2_ahci;
static hype_atapi_t g_iso_2_atapi;

/*
 * ISO-2: backs M4-5's own AHCI/ATAPI in-memory model with ISO-1's real
 * loaded \iso\test.iso buffer (g_iso_host_phys/g_iso_size) instead of a
 * synthetic pattern -- otherwise an exact copy of M4-5's own test
 * guest (same payload template, same fixed-address convention; PCI
 * discovery is PCI-2's own separate concern, not needed here). Reads
 * LBA 16, the ISO9660 Primary Volume Descriptor sector (always the
 * 17th 2048-byte sector, ECMA-119 SS8.4), and verifies both a
 * byte-for-byte match against the real file content at that same
 * offset *and* the "CD001" identifier at the sector's own byte offset
 * 1 -- the same signature ISO-1 already verified via direct UEFI file
 * I/O, now confirmed reachable through the emulated AHCI/ATAPI
 * hardware path instead.
 */
static void run_iso_2_test(const hype_vmm_ops_t *ops, hype_vmm_kind_t kind) {
    unsigned long long i;
    uint64_t entry_rip, guest_cr3, rsp, npt_root_phys;
    uint64_t cmd_list_phys, cmd_table_phys, rx_fis_phys, dest_phys;
    const uint8_t *iso_bytes;
    hype_vcpu_ctx_t *ctx;
    hype_vmexit_info_t info;

    if (kind != HYPE_VMM_KIND_SVM) {
        hype_serial_print("iso-2: skipped -- %s has no working vcpu_run yet (see vmx_ops.c)\n", ops->name);
        return;
    }

    hype_guest_ram_zero(g_iso_2_cmd_list, sizeof(g_iso_2_cmd_list));
    hype_guest_ram_zero(g_iso_2_cmd_table, sizeof(g_iso_2_cmd_table));
    hype_guest_ram_zero(g_iso_2_rx_fis, sizeof(g_iso_2_rx_fis));
    hype_guest_ram_zero(g_iso_2_dest_buffer, sizeof(g_iso_2_dest_buffer));
    hype_guest_ram_zero(g_iso_2_guest_code, sizeof(g_iso_2_guest_code));
    hype_guest_ram_zero(g_iso_2_guest_stack, sizeof(g_iso_2_guest_stack));

    hype_atapi_reset(&g_iso_2_atapi, (uint8_t *)(uintptr_t)g_iso_host_phys, (uint32_t)g_iso_size);
    hype_ahci_reset(&g_iso_2_ahci);

    cmd_list_phys = (uint64_t)(uintptr_t)g_iso_2_cmd_list;
    cmd_table_phys = (uint64_t)(uintptr_t)g_iso_2_cmd_table;
    rx_fis_phys = (uint64_t)(uintptr_t)g_iso_2_rx_fis;
    dest_phys = (uint64_t)(uintptr_t)g_iso_2_dest_buffer;

    hype_write_le32(g_iso_2_cmd_list + 0, 0x00010025u);
    hype_write_le32(g_iso_2_cmd_list + 4, 0);
    hype_write_le32(g_iso_2_cmd_list + 8, (uint32_t)cmd_table_phys);
    hype_write_le32(g_iso_2_cmd_list + 12, (uint32_t)(cmd_table_phys >> 32));

    g_iso_2_cmd_table[0] = 0x27;
    g_iso_2_cmd_table[1] = 0x80;
    g_iso_2_cmd_table[2] = 0xA0;
    g_iso_2_cmd_table[0x40 + 0] = HYPE_ATAPI_CMD_READ10;
    g_iso_2_cmd_table[0x40 + 5] = HYPE_ISO_2_PVD_LBA; /* LBA low byte */
    g_iso_2_cmd_table[0x40 + 8] = 1;                  /* transfer length: 1 block */
    hype_write_le32(g_iso_2_cmd_table + 0x80 + 0, (uint32_t)dest_phys);
    hype_write_le32(g_iso_2_cmd_table + 0x80 + 4, (uint32_t)(dest_phys >> 32));
    hype_write_le32(g_iso_2_cmd_table + 0x80 + 12, HYPE_ATAPI_SECTOR_SIZE - 1u);

    for (i = 0; i < sizeof(g_m4_5_payload_template); i++) {
        g_iso_2_guest_code[i] = g_m4_5_payload_template[i];
    }
    hype_write_le64(g_iso_2_guest_code + HYPE_M4_5_PAYLOAD_RBX_IMM_OFFSET, HYPE_ISO_2_AHCI_GPA);
    hype_write_le32(g_iso_2_guest_code + HYPE_M4_5_PAYLOAD_CLB_LOW_IMM_OFFSET, (uint32_t)cmd_list_phys);
    hype_write_le32(g_iso_2_guest_code + HYPE_M4_5_PAYLOAD_CLB_HIGH_IMM_OFFSET,
                     (uint32_t)(cmd_list_phys >> 32));
    hype_write_le32(g_iso_2_guest_code + HYPE_M4_5_PAYLOAD_FB_LOW_IMM_OFFSET, (uint32_t)rx_fis_phys);
    hype_write_le32(g_iso_2_guest_code + HYPE_M4_5_PAYLOAD_FB_HIGH_IMM_OFFSET,
                     (uint32_t)(rx_fis_phys >> 32));

    entry_rip = (uint64_t)(uintptr_t)g_iso_2_guest_code;
    rsp = (uint64_t)(uintptr_t)(g_iso_2_guest_stack + sizeof(g_iso_2_guest_stack));

    hype_paging_build_identity(g_guest_pml4, g_guest_pdpt, g_guest_pd, HYPE_M3_5_GUEST_PAGING_GB);
    guest_cr3 = (uint64_t)(uintptr_t)g_guest_pml4;

    hype_npt_build_identity(g_npt_pml4, g_npt_pdpt, g_npt_pd, HYPE_NPT_MAX_GB);
    hype_npt_mark_not_present(g_npt_pd, HYPE_ISO_2_AHCI_GPA);
    npt_root_phys = (uint64_t)(uintptr_t)g_npt_pml4;

    hype_debug_print("iso-2: entry_rip=0x%llx ahci_gpa=0x%llx reading real ISO LBA %u\n",
                      (unsigned long long)entry_rip, (unsigned long long)HYPE_ISO_2_AHCI_GPA,
                      HYPE_ISO_2_PVD_LBA);

    ctx = hype_svm_vcpu_create_long_mode(entry_rip, guest_cr3, rsp, npt_root_phys);
    if (ctx == 0) {
        hype_fatal("iso-2: vcpu_create_long_mode failed");
    }

    for (;;) {
        if (ops->vcpu_run(ctx, &info) != 0) {
            hype_fatal("iso-2: VM-entry failed (reason=0x%llx)", (unsigned long long)info.reason);
        }

        if (info.reason == HYPE_SVM_EXITCODE_NPF) {
            if (hype_svm_vcpu_handle_ahci_npf(ctx, &g_iso_2_ahci, &g_iso_2_atapi, HYPE_ISO_2_AHCI_GPA) !=
                0) {
                hype_fatal("iso-2: unhandled/unrecognized AHCI MMIO access (qual=0x%llx guest_rip=0x%llx)",
                           (unsigned long long)info.qualification, (unsigned long long)info.guest_rip);
            }
            continue;
        }

        break;
    }

    if (info.reason != HYPE_SVM_EXITCODE_HLT) {
        hype_fatal("iso-2: test guest did not halt cleanly (reason=0x%llx guest_rip=0x%llx)",
                   (unsigned long long)info.reason, (unsigned long long)info.guest_rip);
    }

    iso_bytes = (const uint8_t *)(uintptr_t)g_iso_host_phys;
    for (i = 0; i < HYPE_ATAPI_SECTOR_SIZE; i++) {
        uint64_t file_offset = (uint64_t)HYPE_ISO_2_PVD_LBA * HYPE_ATAPI_SECTOR_SIZE + i;
        if (g_iso_2_dest_buffer[i] != iso_bytes[file_offset]) {
            hype_fatal("iso-2: AHCI/ATAPI READ(10) mismatch at byte %llu: got 0x%x, expected 0x%x", i,
                       g_iso_2_dest_buffer[i], iso_bytes[file_offset]);
        }
    }
    if (g_iso_2_dest_buffer[1] != 'C' || g_iso_2_dest_buffer[2] != 'D' || g_iso_2_dest_buffer[3] != '0' ||
        g_iso_2_dest_buffer[4] != '0' || g_iso_2_dest_buffer[5] != '1') {
        hype_fatal("iso-2: \"CD001\" identifier missing from the AHCI/ATAPI-read sector");
    }

    hype_debug_print(
        "iso-2: AHCI/ATAPI test guest halted cleanly (reason=0x%llx, guest_rip=0x%llx) -- real ISO LBA "
        "%u read byte-for-byte via emulated hardware, \"CD001\" identifier verified\n",
        (unsigned long long)info.reason, (unsigned long long)info.guest_rip, HYPE_ISO_2_PVD_LBA);
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

#define HYPE_INT_TEST_VECTOR 0x31u

/*
 * A real guest GDT (3 entries: null, flat 64-bit code at selector
 * 0x08, flat data at selector 0x10) -- access/flags bytes match
 * vmcb.c's own LONGMODE_CODE_ACCESS/FLAGS and LONGMODE_DATA_ACCESS/
 * FLAGS constants exactly, so a hardware-driven CS reload during
 * interrupt delivery ends up with the same effective attributes
 * hype_vmcb_build_long_mode_guest() already gave CS directly.
 * hype_vmcb_build_long_mode_guest()'s own default GDTR/IDTR (base=0,
 * limit=0xFFFF) has no real table behind it -- fine for every prior
 * long-mode test guest, none of which ever took a real hardware-
 * validated segment/gate reload, but genuine interrupt delivery does
 * exactly that (see hype_svm_vcpu_set_gdt()'s own comment).
 */
static uint8_t g_int_gdt[24] __attribute__((aligned(16))) = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* null descriptor */
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x9B, 0xAF, 0x00, /* 0x08: flat code */
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x93, 0xCF, 0x00, /* 0x10: flat data */
};

/* A real guest IDT (256 entries, 16 bytes each) -- zero-filled at
 * reset (an absent/not-present gate for every vector but the one this
 * test actually uses), with HYPE_INT_TEST_VECTOR's own entry populated
 * at runtime once the ISR's address is known (run_int_test() itself). */
static uint8_t g_int_idt[4096] __attribute__((aligned(4096)));

static uint8_t g_int_guest_stack[4096] __attribute__((aligned(4096)));
static uint8_t g_int_marker[4096] __attribute__((aligned(4096)));

/*
 * Guest payload: loads the marker's guest-physical address, enables
 * interrupts, then busy-polls the marker until the (host-triggered)
 * interrupt handler sets it, matching the real "STI; idle-wait" pattern
 * every real OS's own interrupt-driven idle loop uses. The ISR itself
 * sits at a fixed offset in the same buffer (HYPE_INT_PAYLOAD_ISR_OFFSET)
 * -- every store here is register-to-memory (0x89/0xC6) or memory-to-
 * register (0x8B), the same forms hype_mmio_decode() already supports,
 * though nothing here actually traps (this is ordinary guest RAM, not
 * MMIO) -- RBX is never touched between the initial load and the ISR
 * running, so it's still valid whenever the interrupt actually lands.
 *
 *   mov rbx, <patched: marker guest-physical address>
 *                                        48 BB 00*8
 *   sti                                  FB
 * poll:
 *   cmp byte [rbx], 0                    80 3B 00
 *   je poll                              74 FB
 *   hlt                                  F4
 *   jmp $-3                              EB FD
 *   ... padding to isr's own fixed offset ...
 * isr:
 *   mov byte [rbx], 1                    C6 03 01
 *   iretq                                48 CF
 */
static const uint8_t g_int_payload_template[] = {
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFB,
    0x80, 0x3B, 0x00,
    0x74, 0xFB,
    0xF4,
    0xEB, 0xFD,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0xC6, 0x03, 0x01,
    0x48, 0xCF,
};
#define HYPE_INT_PAYLOAD_RBX_IMM_OFFSET 2
#define HYPE_INT_PAYLOAD_ISR_OFFSET 32
static uint8_t g_int_guest_code[sizeof(g_int_payload_template)] __attribute__((aligned(4096)));

/*
 * INT-1/INT-2: proves real guest-interrupt injection end-to-end -- not
 * just the pure encode/decode logic (already unit tested), but an
 * actual VMRUN delivering a synthesized vector into a real guest ISR.
 * hype_svm_vcpu_request_interrupt() is called *before* the first
 * VMRUN, while RFLAGS.IF is still 0 (hype_vmcb_build_long_mode_guest()'s
 * own default) -- this deliberately exercises INT-2's deferred path
 * first (arms an interrupt-window request, since the guest can't
 * accept yet), then INT-1's direct EVENTINJ path once the guest's own
 * STI (a couple of instructions into its first VMRUN) opens that
 * window and fires EXITCODE_VINTR -- both mechanisms, in the one
 * natural sequence a real device's "raise this IRQ whenever the guest
 * happens to be ready" call already needs.
 */
static void run_int_test(const hype_vmm_ops_t *ops, hype_vmm_kind_t kind) {
    unsigned long long i;
    uint64_t entry_rip, guest_cr3, rsp, marker_phys, isr_phys, gdt_phys, idt_phys;
    hype_vcpu_ctx_t *ctx;
    hype_vmexit_info_t info;

    if (kind != HYPE_VMM_KIND_SVM) {
        hype_serial_print("int: skipped -- %s has no working vcpu_run yet (see vmx_ops.c)\n", ops->name);
        return;
    }

    hype_guest_ram_zero(g_int_idt, sizeof(g_int_idt));
    hype_guest_ram_zero(g_int_guest_code, sizeof(g_int_guest_code));
    hype_guest_ram_zero(g_int_guest_stack, sizeof(g_int_guest_stack));
    hype_guest_ram_zero(g_int_marker, sizeof(g_int_marker));

    for (i = 0; i < sizeof(g_int_payload_template); i++) {
        g_int_guest_code[i] = g_int_payload_template[i];
    }
    marker_phys = (uint64_t)(uintptr_t)g_int_marker;
    hype_write_le64(g_int_guest_code + HYPE_INT_PAYLOAD_RBX_IMM_OFFSET, marker_phys);

    isr_phys = (uint64_t)(uintptr_t)(g_int_guest_code + HYPE_INT_PAYLOAD_ISR_OFFSET);

    /* IDT entry HYPE_INT_TEST_VECTOR: a 64-bit interrupt gate (type
     * 0xE) pointing at the ISR, selector 0x08 (g_int_gdt's own flat
     * code descriptor). */
    {
        uint8_t *gate = g_int_idt + (unsigned)HYPE_INT_TEST_VECTOR * 16;
        gate[0] = (uint8_t)(isr_phys & 0xFFu);
        gate[1] = (uint8_t)((isr_phys >> 8) & 0xFFu);
        gate[2] = 0x08; /* selector low byte */
        gate[3] = 0x00; /* selector high byte */
        gate[4] = 0x00; /* IST -- use the current stack, no IST switch */
        gate[5] = 0x8E; /* P=1, DPL=00, type=0xE (64-bit interrupt gate) */
        gate[6] = (uint8_t)((isr_phys >> 16) & 0xFFu);
        gate[7] = (uint8_t)((isr_phys >> 24) & 0xFFu);
        hype_write_le32(gate + 8, (uint32_t)(isr_phys >> 32));
        hype_write_le32(gate + 12, 0);
    }

    entry_rip = (uint64_t)(uintptr_t)g_int_guest_code;
    rsp = (uint64_t)(uintptr_t)(g_int_guest_stack + sizeof(g_int_guest_stack));
    gdt_phys = (uint64_t)(uintptr_t)g_int_gdt;
    idt_phys = (uint64_t)(uintptr_t)g_int_idt;

    hype_paging_build_identity(g_guest_pml4, g_guest_pdpt, g_guest_pd, HYPE_M3_5_GUEST_PAGING_GB);
    guest_cr3 = (uint64_t)(uintptr_t)g_guest_pml4;

    hype_debug_print("int: entry_rip=0x%llx isr_phys=0x%llx marker_phys=0x%llx\n",
                      (unsigned long long)entry_rip, (unsigned long long)isr_phys,
                      (unsigned long long)marker_phys);

    /* No NPT for this test -- pure register/memory + real IDT/GDT
     * descriptor-table content, no MMIO-trapped device involved (same
     * reasoning as CPUMSR's own test). */
    ctx = hype_svm_vcpu_create_long_mode(entry_rip, guest_cr3, rsp, 0);
    if (ctx == 0) {
        hype_fatal("int: vcpu_create_long_mode failed");
    }

    hype_svm_vcpu_set_gdt(ctx, gdt_phys, (uint16_t)(sizeof(g_int_gdt) - 1));
    hype_svm_vcpu_set_idt(ctx, idt_phys, (uint16_t)(sizeof(g_int_idt) - 1));
    hype_svm_vcpu_set_cs_ss_selectors(ctx, 0x08u, 0x10u); /* g_int_gdt's own code/data selectors */
    hype_svm_vcpu_request_interrupt(ctx, HYPE_INT_TEST_VECTOR);

    for (;;) {
        if (ops->vcpu_run(ctx, &info) != 0) {
            hype_fatal("int: VM-entry failed (reason=0x%llx)", (unsigned long long)info.reason);
        }

        if (info.reason == HYPE_SVM_EXITCODE_VINTR) {
            hype_svm_vcpu_handle_vintr_window(ctx);
            continue;
        }

        break;
    }

    if (info.reason != HYPE_SVM_EXITCODE_HLT) {
        hype_fatal("int: test guest did not halt cleanly (reason=0x%llx guest_rip=0x%llx)",
                   (unsigned long long)info.reason, (unsigned long long)info.guest_rip);
    }
    if (g_int_marker[0] != 1) {
        hype_fatal("int: interrupt handler never ran (marker=0x%x)", g_int_marker[0]);
    }

    hype_debug_print(
        "int: test guest halted cleanly (reason=0x%llx, guest_rip=0x%llx) -- vector 0x%x delivered "
        "via an armed VINTR window (INT-2) then direct EVENTINJ (INT-1), ISR ran and set the marker\n",
        (unsigned long long)info.reason, (unsigned long long)info.guest_rip, HYPE_INT_TEST_VECTOR);
}

/* Same flat code/data GDT content as g_int_gdt (INT-1/INT-2) -- a
 * fresh, dedicated copy rather than sharing that buffer, matching this
 * project's own "each test owns its buffers" convention. */
static uint8_t g_input_1_gdt[24] __attribute__((aligned(16))) = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x9B, 0xAF, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x93, 0xCF, 0x00,
};
static uint8_t g_input_1_idt[4096] __attribute__((aligned(4096)));
static uint8_t g_input_1_guest_stack[4096] __attribute__((aligned(4096)));
static uint8_t g_input_1_marker[4096] __attribute__((aligned(4096)));
static uint8_t g_input_1_scancode_result[4096] __attribute__((aligned(4096)));
static hype_pic_emu_t g_input_1_pic;
static hype_pit_emu_t g_input_1_pit; /* unused by this test's own payload -- required by
                                      * hype_svm_vcpu_handle_ioio()'s existing signature */
static hype_ps2_kbd_t g_input_1_ps2;

/*
 * Guest payload -- a real OS-shaped keyboard init sequence, not just a
 * synthetic test harness: programs the master 8259 (ICW1-4, matching
 * the real BIOS-default convention of remapping IRQ0-7 to vectors
 * 0x20-0x27), unmasks only IRQ1 (OCW1), enables interrupts, then
 * busy-polls a marker exactly like a real idle loop waiting for a key.
 * The ISR reads the delivered scancode from the PS/2 data port, sends
 * a non-specific EOI, and sets the marker -- proving the full
 * device-to-guest path (devices/ps2_keyboard.h -> devices/pic.h's own
 * acknowledge -> INT-1/INT-2's injection) works end-to-end, not just
 * the injection mechanism alone (already proven by run_int_test()).
 *
 *   mov rbx, <patched: marker guest-physical address>    48 BB 00*8
 *   mov rcx, <patched: scancode-result guest-physical>    48 B9 00*8
 *   mov al, 0x11 (ICW1)                 B0 11
 *   mov dx, 0x20                        66 BA 20 00
 *   out dx, al                          EE
 *   mov al, 0x20 (ICW2 -- vector offset) B0 20
 *   mov dx, 0x21                        66 BA 21 00
 *   out dx, al                          EE
 *   mov al, 0x04 (ICW3)                 B0 04
 *   out dx, al                          EE
 *   mov al, 0x01 (ICW4)                 B0 01
 *   out dx, al                          EE
 *   mov al, 0xFD (OCW1 -- mask all but IRQ1) B0 FD
 *   out dx, al                          EE
 *   sti                                 FB
 * poll:
 *   cmp byte [rbx], 0                   80 3B 00
 *   je poll                             74 FB
 *   hlt                                 F4
 *   jmp $-3                             EB FD
 *   ... padding to isr's own fixed offset ...
 * isr:
 *   mov dx, 0x60                        66 BA 60 00
 *   in al, dx                           EC
 *   mov [rcx], al                       88 01
 *   mov dx, 0x20                        66 BA 20 00
 *   mov al, 0x20 (OCW2 non-specific EOI) B0 20
 *   out dx, al                          EE
 *   mov byte [rbx], 1                   C6 03 01
 *   iretq                               48 CF
 */
static const uint8_t g_input_1_payload_template[] = {
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x48, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB0, 0x11,
    0x66, 0xBA, 0x20, 0x00,
    0xEE,
    0xB0, 0x20,
    0x66, 0xBA, 0x21, 0x00,
    0xEE,
    0xB0, 0x04,
    0xEE,
    0xB0, 0x01,
    0xEE,
    0xB0, 0xFD,
    0xEE,
    0xFB,
    0x80, 0x3B, 0x00,
    0x74, 0xFB,
    0xF4,
    0xEB, 0xFD,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0x66, 0xBA, 0x60, 0x00,
    0xEC,
    0x88, 0x01,
    0x66, 0xBA, 0x20, 0x00,
    0xB0, 0x20,
    0xEE,
    0xC6, 0x03, 0x01,
    0x48, 0xCF,
};
#define HYPE_INPUT_1_PAYLOAD_RBX_IMM_OFFSET 2
#define HYPE_INPUT_1_PAYLOAD_RCX_IMM_OFFSET 12
#define HYPE_INPUT_1_PAYLOAD_ISR_OFFSET 64
#define HYPE_INPUT_1_TEST_SCANCODE 0x1Eu /* Set 1 make code for 'A' */
static uint8_t g_input_1_guest_code[sizeof(g_input_1_payload_template)] __attribute__((aligned(4096)));

/*
 * INPUT-1: guest-facing PS/2 keyboard device (plan.md §6c) -- proves
 * the full device-to-guest delivery path, not just the injection
 * mechanism run_int_test() already proved. Same "request the interrupt
 * before the first VMRUN" pattern: the key press is enqueued and IRQ1
 * raised while RFLAGS.IF is still 0, so the guest's own PIC
 * initialization + STI (not the host) is what actually opens the
 * delivery window -- exactly the real sequence a genuine keypress
 * arriving before an OS finishes booting would follow.
 */
static void run_input_1_test(const hype_vmm_ops_t *ops, hype_vmm_kind_t kind) {
    unsigned long long i;
    uint64_t entry_rip, guest_cr3, rsp, marker_phys, scancode_result_phys, isr_phys, gdt_phys, idt_phys;
    hype_vcpu_ctx_t *ctx;
    hype_vmexit_info_t info;

    if (kind != HYPE_VMM_KIND_SVM) {
        hype_serial_print("input-1: skipped -- %s has no working vcpu_run yet (see vmx_ops.c)\n",
                           ops->name);
        return;
    }

    hype_guest_ram_zero(g_input_1_idt, sizeof(g_input_1_idt));
    hype_guest_ram_zero(g_input_1_guest_code, sizeof(g_input_1_guest_code));
    hype_guest_ram_zero(g_input_1_guest_stack, sizeof(g_input_1_guest_stack));
    hype_guest_ram_zero(g_input_1_marker, sizeof(g_input_1_marker));
    hype_guest_ram_zero(g_input_1_scancode_result, sizeof(g_input_1_scancode_result));
    hype_pic_emu_reset(&g_input_1_pic);
    hype_pit_emu_reset(&g_input_1_pit);
    hype_ps2_kbd_reset(&g_input_1_ps2);

    for (i = 0; i < sizeof(g_input_1_payload_template); i++) {
        g_input_1_guest_code[i] = g_input_1_payload_template[i];
    }
    marker_phys = (uint64_t)(uintptr_t)g_input_1_marker;
    scancode_result_phys = (uint64_t)(uintptr_t)g_input_1_scancode_result;
    hype_write_le64(g_input_1_guest_code + HYPE_INPUT_1_PAYLOAD_RBX_IMM_OFFSET, marker_phys);
    hype_write_le64(g_input_1_guest_code + HYPE_INPUT_1_PAYLOAD_RCX_IMM_OFFSET, scancode_result_phys);

    isr_phys = (uint64_t)(uintptr_t)(g_input_1_guest_code + HYPE_INPUT_1_PAYLOAD_ISR_OFFSET);

    /* IDT entry 0x21 (this guest's own ICW2 offset 0x20 + IRQ1): a
     * 64-bit interrupt gate, same construction as run_int_test()'s own
     * IDT entry. */
    {
        uint8_t *gate = g_input_1_idt + 0x21u * 16;
        gate[0] = (uint8_t)(isr_phys & 0xFFu);
        gate[1] = (uint8_t)((isr_phys >> 8) & 0xFFu);
        gate[2] = 0x08;
        gate[3] = 0x00;
        gate[4] = 0x00;
        gate[5] = 0x8E;
        gate[6] = (uint8_t)((isr_phys >> 16) & 0xFFu);
        gate[7] = (uint8_t)((isr_phys >> 24) & 0xFFu);
        hype_write_le32(gate + 8, (uint32_t)(isr_phys >> 32));
        hype_write_le32(gate + 12, 0);
    }

    entry_rip = (uint64_t)(uintptr_t)g_input_1_guest_code;
    rsp = (uint64_t)(uintptr_t)(g_input_1_guest_stack + sizeof(g_input_1_guest_stack));
    gdt_phys = (uint64_t)(uintptr_t)g_input_1_gdt;
    idt_phys = (uint64_t)(uintptr_t)g_input_1_idt;

    hype_paging_build_identity(g_guest_pml4, g_guest_pdpt, g_guest_pd, HYPE_M3_5_GUEST_PAGING_GB);
    guest_cr3 = (uint64_t)(uintptr_t)g_guest_pml4;

    hype_debug_print("input-1: entry_rip=0x%llx isr_phys=0x%llx\n", (unsigned long long)entry_rip,
                      (unsigned long long)isr_phys);

    ctx = hype_svm_vcpu_create_long_mode(entry_rip, guest_cr3, rsp, 0);
    if (ctx == 0) {
        hype_fatal("input-1: vcpu_create_long_mode failed");
    }

    hype_svm_vcpu_set_gdt(ctx, gdt_phys, (uint16_t)(sizeof(g_input_1_gdt) - 1));
    hype_svm_vcpu_set_idt(ctx, idt_phys, (uint16_t)(sizeof(g_input_1_idt) - 1));
    hype_svm_vcpu_set_cs_ss_selectors(ctx, 0x08u, 0x10u);

    /* The key press itself: enqueue the scancode in the PS/2 device,
     * then, once the guest's own PIC initialization has genuinely
     * finished, raise IRQ1 through it -- this project's own real
     * device model, not a shortcut straight to EVENTINJ. Deliberately
     * NOT raised before the first VMRUN: this guest's very first
     * instructions are its own ICW1-4 sequence, and a real 8259's own
     * ICW1 unconditionally clears IRR (a fresh initialization discards
     * any previously pending state, matching real hardware) -- raising
     * IRQ1 any earlier would just have it wiped out again immediately.
     * A real keypress that happens to arrive before an OS has even
     * initialized its own PIC is genuinely lost on real hardware too;
     * this test instead waits for initialization to finish (tracked
     * below via the PIC's own init_state reaching 0, "normal
     * operation") before the key press happens, matching a realistic
     * timing. */
    hype_ps2_kbd_enqueue_scancode(&g_input_1_ps2, HYPE_INPUT_1_TEST_SCANCODE);

    {
        int irq1_delivered = 0;

        for (;;) {
            if (ops->vcpu_run(ctx, &info) != 0) {
                hype_fatal("input-1: VM-entry failed (reason=0x%llx)", (unsigned long long)info.reason);
            }

            if (info.reason == HYPE_SVM_EXITCODE_VINTR) {
                hype_svm_vcpu_handle_vintr_window(ctx);
                continue;
            }

            if (info.reason == HYPE_SVM_EXITCODE_IOIO) {
                if (hype_svm_vcpu_handle_ioio(ctx, &g_input_1_pic, &g_input_1_pit) == 0) {
                    if (!irq1_delivered && g_input_1_pic.master.init_state == 0) {
                        hype_svm_vcpu_deliver_pic_irq(ctx, &g_input_1_pic.master, 1);
                        irq1_delivered = 1;
                    }
                    continue;
                }
                if (hype_svm_vcpu_handle_ps2_kbd_ioio(ctx, &g_input_1_ps2) == 0) {
                    continue;
                }
                hype_fatal("input-1: unhandled IOIO access (qual=0x%llx guest_rip=0x%llx)",
                           (unsigned long long)info.qualification, (unsigned long long)info.guest_rip);
            }

            break;
        }
    }

    if (info.reason != HYPE_SVM_EXITCODE_HLT) {
        hype_fatal("input-1: test guest did not halt cleanly (reason=0x%llx guest_rip=0x%llx)",
                   (unsigned long long)info.reason, (unsigned long long)info.guest_rip);
    }
    if (g_input_1_marker[0] != 1) {
        hype_fatal("input-1: interrupt handler never ran (marker=0x%x)", g_input_1_marker[0]);
    }
    if (g_input_1_scancode_result[0] != HYPE_INPUT_1_TEST_SCANCODE) {
        hype_fatal("input-1: ISR read the wrong scancode (got 0x%x, expected 0x%x)",
                   g_input_1_scancode_result[0], HYPE_INPUT_1_TEST_SCANCODE);
    }

    hype_debug_print(
        "input-1: test guest halted cleanly (reason=0x%llx, guest_rip=0x%llx) -- scancode 0x%x "
        "delivered via PS/2 -> PIC (vector 0x21) -> INT-1/INT-2, ISR read it back correctly\n",
        (unsigned long long)info.reason, (unsigned long long)info.guest_rip,
        (unsigned int)HYPE_INPUT_1_TEST_SCANCODE);
}

static uint8_t g_input_2_gdt[24] __attribute__((aligned(16))) = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x9B, 0xAF, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x93, 0xCF, 0x00,
};
static uint8_t g_input_2_idt[4096] __attribute__((aligned(4096)));
static uint8_t g_input_2_guest_stack[4096] __attribute__((aligned(4096)));
static uint8_t g_input_2_marker[4096] __attribute__((aligned(4096)));
static uint8_t g_input_2_result[4096] __attribute__((aligned(4096))); /* [0]=status [1]=dx [2]=dy */
static hype_pic_emu_t g_input_2_pic;
static hype_pit_emu_t g_input_2_pit; /* unused by this test's own payload -- required by
                                      * hype_svm_vcpu_handle_ioio()'s existing signature */
static hype_ps2_kbd_t g_input_2_kbd;
static hype_ps2_mouse_t g_input_2_mouse;

#define HYPE_INPUT_2_TEST_STATUS 0x08u /* left button up, no sign/overflow bits */
#define HYPE_INPUT_2_TEST_DX 0x05u
#define HYPE_INPUT_2_TEST_DY 0xFBu /* -5, two's complement, in the packet's own signed-byte convention */
#define HYPE_INPUT_2_MOUSE_VECTOR 0x2Cu /* slave offset 0x28 + IRQ4 (IRQ12 overall) */

/*
 * Guest payload -- a real OS-shaped mouse-enable sequence: programs
 * BOTH the master 8259 (unmasking only IRQ2, the slave's own cascade
 * line -- required for ANY slave-originated IRQ, including the
 * mouse's IRQ12, to ever reach the CPU at all, matching real hardware)
 * and the slave 8259 (unmasking only IRQ4, IRQ12 overall), enables the
 * i8042's own auxiliary port (0xA8), then speaks directly to the
 * mouse device through the 0xD4 write-to-aux prefix to enable data
 * reporting (0xF4) -- reading back its ACK before proceeding, exactly
 * as a real driver must (leaving it unread would have the ISR
 * misinterpret it as the first byte of a movement packet). The ISR
 * reads the delivered 3-byte movement packet from port 0x60, sends EOI
 * to *both* the slave (ending this specific IRQ) and the master
 * (ending the cascade's own in-service state) -- a real driver detail
 * this project's own PIC model doesn't enforce, but every real OS
 * observes for any slave-originated interrupt.
 *
 *   mov rbx, <patched: marker guest-physical address>     48 BB 00*8
 *   mov rcx, <patched: result-buffer guest-physical>       48 B9 00*8
 *   mov al, 0x11 (master ICW1)          B0 11
 *   mov dx, 0x20                        66 BA 20 00
 *   out dx, al                          EE
 *   mov al, 0x20 (master ICW2)          B0 20
 *   mov dx, 0x21                        66 BA 21 00
 *   out dx, al                          EE
 *   mov al, 0x04 (master ICW3)          B0 04
 *   out dx, al                          EE
 *   mov al, 0x01 (master ICW4)          B0 01
 *   out dx, al                          EE
 *   mov al, 0xFB (master OCW1 -- unmask only IRQ2) B0 FB
 *   out dx, al                          EE
 *   mov al, 0x11 (slave ICW1)           B0 11
 *   mov dx, 0xA0                        66 BA A0 00
 *   out dx, al                          EE
 *   mov al, 0x28 (slave ICW2)           B0 28
 *   mov dx, 0xA1                        66 BA A1 00
 *   out dx, al                          EE
 *   mov al, 0x02 (slave ICW3)           B0 02
 *   out dx, al                          EE
 *   mov al, 0x01 (slave ICW4)           B0 01
 *   out dx, al                          EE
 *   mov al, 0xEF (slave OCW1 -- unmask only IRQ4) B0 EF
 *   out dx, al                          EE
 *   mov al, 0xA8 (ENABLE_AUX_PORT)      B0 A8
 *   mov dx, 0x64                        66 BA 64 00
 *   out dx, al                          EE
 *   mov al, 0xD4 (WRITE_TO_AUX)         B0 D4
 *   out dx, al                          EE
 *   mov al, 0xF4 (mouse: enable reporting) B0 F4
 *   mov dx, 0x60                        66 BA 60 00
 *   out dx, al                          EE
 *   in al, dx (discard the mouse's own ACK) EC
 *   sti                                 FB
 * poll:
 *   cmp byte [rbx], 0                   80 3B 00
 *   je poll                             74 FB
 *   hlt                                 F4
 *   jmp $-3                             EB FD
 *   ... padding to isr's own fixed offset ...
 * isr:
 *   mov dx, 0x60                        66 BA 60 00
 *   in al, dx                           EC
 *   mov [rcx], al                       88 01
 *   in al, dx                           EC
 *   mov [rcx+1], al                     88 41 01
 *   in al, dx                           EC
 *   mov [rcx+2], al                     88 41 02
 *   mov al, 0x20 (OCW2 non-specific EOI) B0 20
 *   mov dx, 0xA0 (slave first)          66 BA A0 00
 *   out dx, al                          EE
 *   mov dx, 0x20 (then master)          66 BA 20 00
 *   out dx, al                          EE
 *   mov byte [rbx], 1                   C6 03 01
 *   iretq                               48 CF
 */
static const uint8_t g_input_2_payload_template[] = {
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x48, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB0, 0x11,
    0x66, 0xBA, 0x20, 0x00,
    0xEE,
    0xB0, 0x20,
    0x66, 0xBA, 0x21, 0x00,
    0xEE,
    0xB0, 0x04,
    0xEE,
    0xB0, 0x01,
    0xEE,
    0xB0, 0xFB,
    0xEE,
    0xB0, 0x11,
    0x66, 0xBA, 0xA0, 0x00,
    0xEE,
    0xB0, 0x28,
    0x66, 0xBA, 0xA1, 0x00,
    0xEE,
    0xB0, 0x02,
    0xEE,
    0xB0, 0x01,
    0xEE,
    0xB0, 0xEF,
    0xEE,
    0xB0, 0xA8,
    0x66, 0xBA, 0x64, 0x00,
    0xEE,
    0xB0, 0xD4,
    0xEE,
    0xB0, 0xF4,
    0x66, 0xBA, 0x60, 0x00,
    0xEE,
    0xEC,
    0xFB,
    0x80, 0x3B, 0x00,
    0x74, 0xFB,
    0xF4,
    0xEB, 0xFD,
    0x90, 0x90, 0x90,
    0x66, 0xBA, 0x60, 0x00,
    0xEC,
    0x88, 0x01,
    0xEC,
    0x88, 0x41, 0x01,
    0xEC,
    0x88, 0x41, 0x02,
    0xB0, 0x20,
    0x66, 0xBA, 0xA0, 0x00,
    0xEE,
    0x66, 0xBA, 0x20, 0x00,
    0xEE,
    0xC6, 0x03, 0x01,
    0x48, 0xCF,
};
#define HYPE_INPUT_2_PAYLOAD_RBX_IMM_OFFSET 2
#define HYPE_INPUT_2_PAYLOAD_RCX_IMM_OFFSET 12
#define HYPE_INPUT_2_PAYLOAD_ISR_OFFSET 96
static uint8_t g_input_2_guest_code[sizeof(g_input_2_payload_template)] __attribute__((aligned(4096)));

/*
 * INPUT-2: guest-facing PS/2 mouse device (plan.md §6c). Same overall
 * shape as run_input_1_test(), one level deeper: the "device event"
 * (a mouse movement) can only be injected once the guest has itself
 * finished BOTH PIC channels' own initialization *and* explicitly
 * enabled mouse data reporting (hype_ps2_mouse_t's own reporting_enabled
 * flag, set the moment the guest's 0xF4 command is written -- mirroring
 * INPUT-1's own "wait for the guest's own init_state to reach 0" gate,
 * extended with this mouse-specific readiness condition since a real
 * mouse never streams movement before being told to).
 */
static void run_input_2_test(const hype_vmm_ops_t *ops, hype_vmm_kind_t kind) {
    unsigned long long i;
    uint64_t entry_rip, guest_cr3, rsp, marker_phys, result_phys, isr_phys, gdt_phys, idt_phys;
    hype_vcpu_ctx_t *ctx;
    hype_vmexit_info_t info;

    if (kind != HYPE_VMM_KIND_SVM) {
        hype_serial_print("input-2: skipped -- %s has no working vcpu_run yet (see vmx_ops.c)\n",
                           ops->name);
        return;
    }

    hype_guest_ram_zero(g_input_2_idt, sizeof(g_input_2_idt));
    hype_guest_ram_zero(g_input_2_guest_code, sizeof(g_input_2_guest_code));
    hype_guest_ram_zero(g_input_2_guest_stack, sizeof(g_input_2_guest_stack));
    hype_guest_ram_zero(g_input_2_marker, sizeof(g_input_2_marker));
    hype_guest_ram_zero(g_input_2_result, sizeof(g_input_2_result));
    hype_pic_emu_reset(&g_input_2_pic);
    hype_pit_emu_reset(&g_input_2_pit);
    hype_ps2_kbd_reset(&g_input_2_kbd);
    hype_ps2_mouse_reset(&g_input_2_mouse);

    for (i = 0; i < sizeof(g_input_2_payload_template); i++) {
        g_input_2_guest_code[i] = g_input_2_payload_template[i];
    }
    marker_phys = (uint64_t)(uintptr_t)g_input_2_marker;
    result_phys = (uint64_t)(uintptr_t)g_input_2_result;
    hype_write_le64(g_input_2_guest_code + HYPE_INPUT_2_PAYLOAD_RBX_IMM_OFFSET, marker_phys);
    hype_write_le64(g_input_2_guest_code + HYPE_INPUT_2_PAYLOAD_RCX_IMM_OFFSET, result_phys);

    isr_phys = (uint64_t)(uintptr_t)(g_input_2_guest_code + HYPE_INPUT_2_PAYLOAD_ISR_OFFSET);

    {
        uint8_t *gate = g_input_2_idt + (unsigned)HYPE_INPUT_2_MOUSE_VECTOR * 16;
        gate[0] = (uint8_t)(isr_phys & 0xFFu);
        gate[1] = (uint8_t)((isr_phys >> 8) & 0xFFu);
        gate[2] = 0x08;
        gate[3] = 0x00;
        gate[4] = 0x00;
        gate[5] = 0x8E;
        gate[6] = (uint8_t)((isr_phys >> 16) & 0xFFu);
        gate[7] = (uint8_t)((isr_phys >> 24) & 0xFFu);
        hype_write_le32(gate + 8, (uint32_t)(isr_phys >> 32));
        hype_write_le32(gate + 12, 0);
    }

    entry_rip = (uint64_t)(uintptr_t)g_input_2_guest_code;
    rsp = (uint64_t)(uintptr_t)(g_input_2_guest_stack + sizeof(g_input_2_guest_stack));
    gdt_phys = (uint64_t)(uintptr_t)g_input_2_gdt;
    idt_phys = (uint64_t)(uintptr_t)g_input_2_idt;

    hype_paging_build_identity(g_guest_pml4, g_guest_pdpt, g_guest_pd, HYPE_M3_5_GUEST_PAGING_GB);
    guest_cr3 = (uint64_t)(uintptr_t)g_guest_pml4;

    hype_debug_print("input-2: entry_rip=0x%llx isr_phys=0x%llx\n", (unsigned long long)entry_rip,
                      (unsigned long long)isr_phys);

    ctx = hype_svm_vcpu_create_long_mode(entry_rip, guest_cr3, rsp, 0);
    if (ctx == 0) {
        hype_fatal("input-2: vcpu_create_long_mode failed");
    }

    hype_svm_vcpu_set_gdt(ctx, gdt_phys, (uint16_t)(sizeof(g_input_2_gdt) - 1));
    hype_svm_vcpu_set_idt(ctx, idt_phys, (uint16_t)(sizeof(g_input_2_idt) - 1));
    hype_svm_vcpu_set_cs_ss_selectors(ctx, 0x08u, 0x10u);

    {
        int event_delivered = 0;

        for (;;) {
            if (ops->vcpu_run(ctx, &info) != 0) {
                hype_fatal("input-2: VM-entry failed (reason=0x%llx)", (unsigned long long)info.reason);
            }

            if (info.reason == HYPE_SVM_EXITCODE_VINTR) {
                hype_svm_vcpu_handle_vintr_window(ctx);
                continue;
            }

            if (info.reason == HYPE_SVM_EXITCODE_IOIO) {
                if (hype_svm_vcpu_handle_ioio(ctx, &g_input_2_pic, &g_input_2_pit) == 0) {
                    continue;
                }
                if (hype_svm_vcpu_handle_ps2_ioio(ctx, &g_input_2_kbd, &g_input_2_mouse, 0) == 0) {
                    /* Mirrors run_input_1_test()'s own "wait for the
                     * guest to genuinely be ready" gate, extended with
                     * the mouse-specific readiness condition: reporting
                     * must be enabled (the guest's own 0xF4, just
                     * handled above if this is that exact write) AND
                     * the slave PIC's own init must be complete (IRQ4
                     * unmasked). */
                    if (!event_delivered && g_input_2_mouse.reporting_enabled &&
                        g_input_2_pic.slave.init_state == 0) {
                        hype_ps2_mouse_enqueue_movement(&g_input_2_mouse, HYPE_INPUT_2_TEST_STATUS,
                                                         HYPE_INPUT_2_TEST_DX, HYPE_INPUT_2_TEST_DY);
                        hype_svm_vcpu_deliver_pic_irq(ctx, &g_input_2_pic.slave, 4);
                        event_delivered = 1;
                    }
                    continue;
                }
                hype_fatal("input-2: unhandled IOIO access (qual=0x%llx guest_rip=0x%llx)",
                           (unsigned long long)info.qualification, (unsigned long long)info.guest_rip);
            }

            break;
        }
    }

    if (info.reason != HYPE_SVM_EXITCODE_HLT) {
        hype_fatal("input-2: test guest did not halt cleanly (reason=0x%llx guest_rip=0x%llx)",
                   (unsigned long long)info.reason, (unsigned long long)info.guest_rip);
    }
    if (g_input_2_marker[0] != 1) {
        hype_fatal("input-2: interrupt handler never ran (marker=0x%x)", g_input_2_marker[0]);
    }
    if (g_input_2_result[0] != HYPE_INPUT_2_TEST_STATUS || g_input_2_result[1] != HYPE_INPUT_2_TEST_DX ||
        g_input_2_result[2] != HYPE_INPUT_2_TEST_DY) {
        hype_fatal("input-2: ISR read the wrong packet (got 0x%x/0x%x/0x%x, expected 0x%x/0x%x/0x%x)",
                   g_input_2_result[0], g_input_2_result[1], g_input_2_result[2], HYPE_INPUT_2_TEST_STATUS,
                   HYPE_INPUT_2_TEST_DX, HYPE_INPUT_2_TEST_DY);
    }

    hype_debug_print(
        "input-2: test guest halted cleanly (reason=0x%llx, guest_rip=0x%llx) -- mouse packet "
        "0x%x/0x%x/0x%x delivered via PS/2 -> PIC (vector 0x%x) -> INT-1/INT-2, ISR read it back "
        "correctly\n",
        (unsigned long long)info.reason, (unsigned long long)info.guest_rip,
        (unsigned int)HYPE_INPUT_2_TEST_STATUS, (unsigned int)HYPE_INPUT_2_TEST_DX,
        (unsigned int)HYPE_INPUT_2_TEST_DY, (unsigned int)HYPE_INPUT_2_MOUSE_VECTOR);
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

/*
 * PCI-1: exercises the new ECAM-based PCI configuration-space model
 * (devices/pci.h, hype_svm_vcpu_handle_pci_ecam_npf() in
 * arch/x86_64/svm/svm_vcpu.c) end-to-end -- registers a host bridge
 * (bus 0/device 0, class 0x0600) and a fake AHCI-class device (bus 0/
 * device 1, class 0x010601, standing in for M4-5's real AHCI device
 * until PCI-2 wires that in for real), then has the guest read the
 * host bridge's vendor/device ID and class code, probe an absent
 * device (confirming the standard "reads as all-1s" convention every
 * real PCI bus-walk relies on), and run the BAR sizing/programming
 * protocol against the fake AHCI device's BAR0 -- all via ordinary
 * guest-RAM-mapped-looking MOV instructions that actually NPF-trap
 * through the ECAM region, same mechanism as M4-3/M4-5's own MMIO
 * devices.
 */
static uint8_t g_pci_1_guest_code[128] __attribute__((aligned(4096)));
static uint8_t g_pci_1_guest_stack[4096] __attribute__((aligned(4096)));
static uint8_t g_pci_1_result_buf[32] __attribute__((aligned(16)));
static hype_pci_t g_pci_1_pci;

/* Guest-physical base of the (bus-0-only) ECAM window -- 4GB,
 * comfortably clear of HYPE_M4_3_PFLASH_GPA (3GB) and
 * HYPE_M4_5_AHCI_GPA (3GB+2MB), well within the shared NPT/guest
 * identity map's reach. */
#define HYPE_PCI_1_ECAM_GPA (4ULL * HYPE_PAGING_1GB)
#define HYPE_PCI_1_AHCI_DEV 1u

/*
 *   mov rbx, <patched: ECAM base guest-physical address>
 *                                          48 BB 00*8
 *   mov rdi, <patched: result_buf guest-physical address>
 *                                          48 BF 00*8
 *   mov eax, [rbx+0]      (host bridge vendor/device ID)  8B 43 00
 *   mov [rdi+0], eax                                      89 47 00
 *   mov eax, [rbx+8]      (host bridge class code)        8B 43 08
 *   mov [rdi+4], eax                                      89 47 04
 *   mov eax, [rbx+0x28000] (device 5, absent)             8B 83 00 80 02 00
 *   mov [rdi+8], eax                                       89 47 08
 *   mov eax, 0xFFFFFFFF   (device1 BAR0 sizing probe value) B8 FF FF FF FF
 *   mov [rbx+0x8010], eax                                  89 83 10 80 00 00
 *   mov eax, [rbx+0x8010]                                  8B 83 10 80 00 00
 *   mov [rdi+12], eax                                      89 47 0C
 *   mov eax, 0xE0100123   (program a real address)         B8 23 01 10 E0
 *   mov [rbx+0x8010], eax                                  89 83 10 80 00 00
 *   mov eax, [rbx+0x8010]                                  8B 83 10 80 00 00
 *   mov [rdi+16], eax                                      89 47 10
 *   hlt                                                    F4
 *   jmp $-3                                                EB FD
 *
 * Note: BAR writes go through EAX rather than a "mov dword [mem],
 * imm32" (opcode 0xC7) form -- hype_mmio_decode() only supports the
 * MOV/MOVZX register forms every other test guest here already uses
 * (0x88/0x89/0x8A/0x8B/0F B6/0F B7), not immediate-to-memory stores;
 * matching that existing convention here rather than extending the
 * decoder just for this test's own convenience.
 */
static const uint8_t g_pci_1_payload_template[] = {
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x48, 0xBF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x8B, 0x43, 0x00,
    0x89, 0x47, 0x00,
    0x8B, 0x43, 0x08,
    0x89, 0x47, 0x04,
    0x8B, 0x83, 0x00, 0x80, 0x02, 0x00,
    0x89, 0x47, 0x08,
    0xB8, 0xFF, 0xFF, 0xFF, 0xFF,
    0x89, 0x83, 0x10, 0x80, 0x00, 0x00,
    0x8B, 0x83, 0x10, 0x80, 0x00, 0x00,
    0x89, 0x47, 0x0C,
    0xB8, 0x23, 0x01, 0x10, 0xE0,
    0x89, 0x83, 0x10, 0x80, 0x00, 0x00,
    0x8B, 0x83, 0x10, 0x80, 0x00, 0x00,
    0x89, 0x47, 0x10,
    0xF4,
    0xEB, 0xFD
};
#define HYPE_PCI_1_PAYLOAD_RBX_IMM_OFFSET 2
#define HYPE_PCI_1_PAYLOAD_RDI_IMM_OFFSET 12

static void run_pci_1_test(const hype_vmm_ops_t *ops, hype_vmm_kind_t kind) {
    unsigned long long i;
    uint64_t entry_rip, guest_cr3, rsp, npt_root_phys, result_buf_phys;
    hype_vcpu_ctx_t *ctx;
    hype_vmexit_info_t info;
    uint32_t got_hostbridge_id, got_hostbridge_class, got_absent, got_bar_mask, got_bar_addr;

    if (kind != HYPE_VMM_KIND_SVM) {
        hype_serial_print("pci-1: skipped -- %s has no working vcpu_run yet (see vmx_ops.c)\n", ops->name);
        return;
    }

    hype_guest_ram_zero(g_pci_1_guest_code, sizeof(g_pci_1_guest_code));
    hype_guest_ram_zero(g_pci_1_guest_stack, sizeof(g_pci_1_guest_stack));
    hype_guest_ram_zero(g_pci_1_result_buf, sizeof(g_pci_1_result_buf));

    hype_pci_reset(&g_pci_1_pci);
    hype_pci_add_device(&g_pci_1_pci, 0, HYPE_PCI_VENDOR_ID_HYPE, 0x0000u, 0x06, 0x00, 0x00);
    hype_pci_add_device(&g_pci_1_pci, HYPE_PCI_1_AHCI_DEV, HYPE_PCI_VENDOR_ID_HYPE, 0x0001u, 0x01, 0x06,
                         0x01);
    hype_pci_set_bar_size(&g_pci_1_pci, HYPE_PCI_1_AHCI_DEV, 0, 0x1000u);

    result_buf_phys = (uint64_t)(uintptr_t)g_pci_1_result_buf;

    for (i = 0; i < sizeof(g_pci_1_payload_template); i++) {
        g_pci_1_guest_code[i] = g_pci_1_payload_template[i];
    }
    hype_write_le64(g_pci_1_guest_code + HYPE_PCI_1_PAYLOAD_RBX_IMM_OFFSET, HYPE_PCI_1_ECAM_GPA);
    hype_write_le64(g_pci_1_guest_code + HYPE_PCI_1_PAYLOAD_RDI_IMM_OFFSET, result_buf_phys);

    entry_rip = (uint64_t)(uintptr_t)g_pci_1_guest_code;
    rsp = (uint64_t)(uintptr_t)(g_pci_1_guest_stack + sizeof(g_pci_1_guest_stack));

    hype_paging_build_identity(g_guest_pml4, g_guest_pdpt, g_guest_pd, HYPE_M3_5_GUEST_PAGING_GB);
    guest_cr3 = (uint64_t)(uintptr_t)g_guest_pml4;

    hype_npt_build_identity(g_npt_pml4, g_npt_pdpt, g_npt_pd, HYPE_NPT_MAX_GB);
    hype_npt_mark_not_present(g_npt_pd, HYPE_PCI_1_ECAM_GPA);
    npt_root_phys = (uint64_t)(uintptr_t)g_npt_pml4;

    hype_debug_print("pci-1: entry_rip=0x%llx ecam_gpa=0x%llx result_buf=0x%llx\n",
                      (unsigned long long)entry_rip, (unsigned long long)HYPE_PCI_1_ECAM_GPA,
                      (unsigned long long)result_buf_phys);

    ctx = hype_svm_vcpu_create_long_mode(entry_rip, guest_cr3, rsp, npt_root_phys);
    if (ctx == 0) {
        hype_fatal("pci-1: vcpu_create_long_mode failed");
    }

    for (;;) {
        if (ops->vcpu_run(ctx, &info) != 0) {
            hype_fatal("pci-1: VM-entry failed (reason=0x%llx)", (unsigned long long)info.reason);
        }

        if (info.reason == HYPE_SVM_EXITCODE_NPF) {
            if (hype_svm_vcpu_handle_pci_ecam_npf(ctx, &g_pci_1_pci, HYPE_PCI_1_ECAM_GPA,
                                                (const uint8_t *)(uintptr_t)info.guest_rip) != 0) {
                hype_fatal("pci-1: unhandled guest ECAM access (qual=0x%llx guest_rip=0x%llx)",
                           (unsigned long long)info.qualification, (unsigned long long)info.guest_rip);
            }
            continue;
        }

        break;
    }

    if (info.reason != HYPE_SVM_EXITCODE_HLT) {
        hype_fatal("pci-1: test guest did not halt cleanly (reason=0x%llx guest_rip=0x%llx)",
                   (unsigned long long)info.reason, (unsigned long long)info.guest_rip);
    }

    got_hostbridge_id = (uint32_t)g_pci_1_result_buf[0] | ((uint32_t)g_pci_1_result_buf[1] << 8) |
                        ((uint32_t)g_pci_1_result_buf[2] << 16) | ((uint32_t)g_pci_1_result_buf[3] << 24);
    got_hostbridge_class = (uint32_t)g_pci_1_result_buf[4] | ((uint32_t)g_pci_1_result_buf[5] << 8) |
                           ((uint32_t)g_pci_1_result_buf[6] << 16) | ((uint32_t)g_pci_1_result_buf[7] << 24);
    got_absent = (uint32_t)g_pci_1_result_buf[8] | ((uint32_t)g_pci_1_result_buf[9] << 8) |
                 ((uint32_t)g_pci_1_result_buf[10] << 16) | ((uint32_t)g_pci_1_result_buf[11] << 24);
    got_bar_mask = (uint32_t)g_pci_1_result_buf[12] | ((uint32_t)g_pci_1_result_buf[13] << 8) |
                   ((uint32_t)g_pci_1_result_buf[14] << 16) | ((uint32_t)g_pci_1_result_buf[15] << 24);
    got_bar_addr = (uint32_t)g_pci_1_result_buf[16] | ((uint32_t)g_pci_1_result_buf[17] << 8) |
                   ((uint32_t)g_pci_1_result_buf[18] << 16) | ((uint32_t)g_pci_1_result_buf[19] << 24);

    if (got_hostbridge_id != (((uint32_t)0x0000u << 16) | HYPE_PCI_VENDOR_ID_HYPE)) {
        hype_fatal("pci-1: host bridge vendor/device ID mismatch (got 0x%x)", got_hostbridge_id);
    }
    if (got_hostbridge_class != 0x06000000u) {
        hype_fatal("pci-1: host bridge class code mismatch (got 0x%x)", got_hostbridge_class);
    }
    if (got_absent != 0xFFFFFFFFu) {
        hype_fatal("pci-1: absent device did not read all-ones (got 0x%x)", got_absent);
    }
    if (got_bar_mask != 0xFFFFF000u) {
        hype_fatal("pci-1: BAR size mask mismatch (got 0x%x)", got_bar_mask);
    }
    if (got_bar_addr != 0xE0100000u) {
        hype_fatal("pci-1: BAR programmed address mismatch (got 0x%x)", got_bar_addr);
    }

    hype_debug_print(
        "pci-1: test guest halted cleanly (reason=0x%llx, guest_rip=0x%llx) -- host bridge id/class, "
        "absent-device probe, BAR sizing all verified via ECAM\n",
        (unsigned long long)info.reason, (unsigned long long)info.guest_rip);
}

/*
 * PCI-2: makes M4-5's real AHCI device genuinely PCI-discoverable --
 * registered via PCI-1's config-space model with BAR5 sized (the real
 * ABAR convention), instead of always living at a fixed guest-physical
 * address the guest never had to discover for itself. The guest here
 * plays the role a real PCI bus driver would: program BAR5 with an
 * address of its own choosing (deliberately different from
 * HYPE_M4_5_AHCI_GPA, to prove this is genuinely dynamic, not
 * incidentally the same fixed value), then set Command.Memory Space
 * Enable -- only *after* observing that exact config-space write does
 * this test's own dispatch loop (mirroring what a real PCI-aware
 * hypervisor's device model does) dynamically NPT-map the device at
 * that address, letting the rest of M4-5's own already-proven AHCI/
 * ATAPI register setup + READ(10) sequence run unchanged, just
 * retargeted to the newly-discovered address instead of a compile-time
 * constant.
 */
static uint8_t g_pci_2_media[4 * HYPE_ATAPI_SECTOR_SIZE] __attribute__((aligned(4096)));
static uint8_t g_pci_2_cmd_list[4096] __attribute__((aligned(4096)));
static uint8_t g_pci_2_cmd_table[4096] __attribute__((aligned(4096)));
static uint8_t g_pci_2_rx_fis[4096] __attribute__((aligned(4096)));
static uint8_t g_pci_2_dest_buffer[HYPE_ATAPI_SECTOR_SIZE] __attribute__((aligned(4096)));
static uint8_t g_pci_2_guest_code[192] __attribute__((aligned(4096)));
static uint8_t g_pci_2_guest_stack[4096] __attribute__((aligned(4096)));
static hype_ahci_t g_pci_2_ahci;
static hype_atapi_t g_pci_2_atapi;
static hype_pci_t g_pci_2_pci;

#define HYPE_PCI_2_AHCI_DEV 1u
/* The address the guest itself chooses to place the AHCI device at,
 * by programming it into BAR5 -- deliberately NOT
 * HYPE_M4_5_AHCI_GPA (3GB+2MB), to prove this is genuinely discovered/
 * dynamic rather than coincidentally the same fixed constant. Must fit
 * in 32 bits (BAR5 is an ordinary 32-bit memory BAR). */
#define HYPE_PCI_2_AHCI_GPA (HYPE_M4_3_PFLASH_GPA + 4ULL * 1024 * 1024)

/*
 * Section A (RBX = ECAM base): programs BAR5 with the chosen address,
 * then sets Command.Memory Space Enable -- the standard PCI
 * enumeration sequence a real bus driver performs once it has sized a
 * BAR and decided where to place it.
 * Section B (RBX = the just-programmed AHCI address): byte-for-byte
 * identical to g_m4_5_payload_template's own AHCI setup/trigger/poll
 * sequence (from its own "mov rbx, <ahci gpa>" onward) -- copied
 * verbatim rather than shared, matching this project's own established
 * "each milestone test owns its payload" convention; the "jnz poll"
 * relative branch keeps working unchanged since it's relative to its
 * own position, not the whole payload's start.
 *
 *   mov rbx, <patched: ECAM base>          48 BB 00*8
 *   mov eax, <patched: HYPE_PCI_2_AHCI_GPA>  B8 00*4
 *   mov [rbx+0x8024], eax  (BAR5)            89 83 24 80 00 00
 *   mov eax, 2  (Memory Space Enable)        B8 02 00 00 00
 *   mov [rbx+0x8004], eax  (Command)         89 83 04 80 00 00
 *   mov rbx, <patched: HYPE_PCI_2_AHCI_GPA>  48 BB 00*8
 *   mov eax, 0x80000000 (GHC.AE)             B8 00 00 00 80
 *   mov [rbx+4], eax                         89 43 04
 *   mov eax, <patched: CLB low32>            B8 00*4
 *   mov [rbx+0x100], eax                     89 83 00 01 00 00
 *   mov eax, <patched: CLB high32>           B8 00*4
 *   mov [rbx+0x104], eax                     89 83 04 01 00 00
 *   mov eax, <patched: FB low32>             B8 00*4
 *   mov [rbx+0x108], eax                     89 83 08 01 00 00
 *   mov eax, <patched: FB high32>            B8 00*4
 *   mov [rbx+0x10C], eax                     89 83 0C 01 00 00
 *   mov eax, 0x00000011 (PxCMD ST|FRE)       B8 11 00 00 00
 *   mov [rbx+0x118], eax                     89 83 18 01 00 00
 *   mov eax, 0x00000001 (PxCI slot 0)        B8 01 00 00 00
 *   mov [rbx+0x138], eax                     89 83 38 01 00 00  <- triggers
 * poll:
 *   mov eax, [rbx+0x138]                     8B 83 38 01 00 00
 *   test eax, eax                            85 C0
 *   jnz poll                                 75 F6
 *   hlt                                      F4
 *   jmp $-3                                  EB FD
 */
static const uint8_t g_pci_2_payload_template[] = {
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x24, 0x80, 0x00, 0x00,
    0xB8, 0x02, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x04, 0x80, 0x00, 0x00,
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
#define HYPE_PCI_2_PAYLOAD_ECAM_RBX_IMM_OFFSET 2
#define HYPE_PCI_2_PAYLOAD_BAR5_VALUE_IMM_OFFSET 11
#define HYPE_PCI_2_PAYLOAD_AHCI_RBX_IMM_OFFSET 34
#define HYPE_PCI_2_PAYLOAD_CLB_LOW_IMM_OFFSET 51
#define HYPE_PCI_2_PAYLOAD_CLB_HIGH_IMM_OFFSET 62
#define HYPE_PCI_2_PAYLOAD_FB_LOW_IMM_OFFSET 73
#define HYPE_PCI_2_PAYLOAD_FB_HIGH_IMM_OFFSET 84

static void run_pci_2_test(const hype_vmm_ops_t *ops, hype_vmm_kind_t kind) {
    unsigned long long i;
    uint64_t entry_rip, guest_cr3, rsp, npt_root_phys;
    uint64_t cmd_list_phys, cmd_table_phys, rx_fis_phys, dest_phys;
    hype_vcpu_ctx_t *ctx;
    hype_vmexit_info_t info;
    int ahci_mapped;
    uint64_t ahci_mapped_base;

    if (kind != HYPE_VMM_KIND_SVM) {
        hype_serial_print("pci-2: skipped -- %s has no working vcpu_run yet (see vmx_ops.c)\n", ops->name);
        return;
    }

    hype_guest_ram_zero(g_pci_2_cmd_list, sizeof(g_pci_2_cmd_list));
    hype_guest_ram_zero(g_pci_2_cmd_table, sizeof(g_pci_2_cmd_table));
    hype_guest_ram_zero(g_pci_2_rx_fis, sizeof(g_pci_2_rx_fis));
    hype_guest_ram_zero(g_pci_2_dest_buffer, sizeof(g_pci_2_dest_buffer));
    hype_guest_ram_zero(g_pci_2_guest_code, sizeof(g_pci_2_guest_code));
    hype_guest_ram_zero(g_pci_2_guest_stack, sizeof(g_pci_2_guest_stack));

    for (i = 0; i < sizeof(g_pci_2_media); i++) {
        g_pci_2_media[i] = (uint8_t)((i / HYPE_ATAPI_SECTOR_SIZE) & 0xFFu);
    }
    hype_atapi_reset(&g_pci_2_atapi, g_pci_2_media, sizeof(g_pci_2_media));
    hype_ahci_reset(&g_pci_2_ahci);

    hype_pci_reset(&g_pci_2_pci);
    hype_pci_add_device(&g_pci_2_pci, HYPE_PCI_2_AHCI_DEV, HYPE_PCI_VENDOR_ID_HYPE, 0x0002u, 0x01, 0x06,
                         0x01);
    hype_pci_set_bar_size(&g_pci_2_pci, HYPE_PCI_2_AHCI_DEV, 5, 0x1000u);

    cmd_list_phys = (uint64_t)(uintptr_t)g_pci_2_cmd_list;
    cmd_table_phys = (uint64_t)(uintptr_t)g_pci_2_cmd_table;
    rx_fis_phys = (uint64_t)(uintptr_t)g_pci_2_rx_fis;
    dest_phys = (uint64_t)(uintptr_t)g_pci_2_dest_buffer;

    hype_write_le32(g_pci_2_cmd_list + 0, 0x00010025u);
    hype_write_le32(g_pci_2_cmd_list + 4, 0);
    hype_write_le32(g_pci_2_cmd_list + 8, (uint32_t)cmd_table_phys);
    hype_write_le32(g_pci_2_cmd_list + 12, (uint32_t)(cmd_table_phys >> 32));

    g_pci_2_cmd_table[0] = 0x27;
    g_pci_2_cmd_table[1] = 0x80;
    g_pci_2_cmd_table[2] = 0xA0;
    g_pci_2_cmd_table[0x40 + 0] = HYPE_ATAPI_CMD_READ10;
    g_pci_2_cmd_table[0x40 + 5] = 2;
    g_pci_2_cmd_table[0x40 + 8] = 1;
    hype_write_le32(g_pci_2_cmd_table + 0x80 + 0, (uint32_t)dest_phys);
    hype_write_le32(g_pci_2_cmd_table + 0x80 + 4, (uint32_t)(dest_phys >> 32));
    hype_write_le32(g_pci_2_cmd_table + 0x80 + 12, HYPE_ATAPI_SECTOR_SIZE - 1u);

    for (i = 0; i < sizeof(g_pci_2_payload_template); i++) {
        g_pci_2_guest_code[i] = g_pci_2_payload_template[i];
    }
    hype_write_le64(g_pci_2_guest_code + HYPE_PCI_2_PAYLOAD_ECAM_RBX_IMM_OFFSET, HYPE_PCI_1_ECAM_GPA);
    hype_write_le32(g_pci_2_guest_code + HYPE_PCI_2_PAYLOAD_BAR5_VALUE_IMM_OFFSET,
                     (uint32_t)HYPE_PCI_2_AHCI_GPA);
    hype_write_le64(g_pci_2_guest_code + HYPE_PCI_2_PAYLOAD_AHCI_RBX_IMM_OFFSET, HYPE_PCI_2_AHCI_GPA);
    hype_write_le32(g_pci_2_guest_code + HYPE_PCI_2_PAYLOAD_CLB_LOW_IMM_OFFSET, (uint32_t)cmd_list_phys);
    hype_write_le32(g_pci_2_guest_code + HYPE_PCI_2_PAYLOAD_CLB_HIGH_IMM_OFFSET,
                     (uint32_t)(cmd_list_phys >> 32));
    hype_write_le32(g_pci_2_guest_code + HYPE_PCI_2_PAYLOAD_FB_LOW_IMM_OFFSET, (uint32_t)rx_fis_phys);
    hype_write_le32(g_pci_2_guest_code + HYPE_PCI_2_PAYLOAD_FB_HIGH_IMM_OFFSET,
                     (uint32_t)(rx_fis_phys >> 32));

    entry_rip = (uint64_t)(uintptr_t)g_pci_2_guest_code;
    rsp = (uint64_t)(uintptr_t)(g_pci_2_guest_stack + sizeof(g_pci_2_guest_stack));

    hype_paging_build_identity(g_guest_pml4, g_guest_pdpt, g_guest_pd, HYPE_M3_5_GUEST_PAGING_GB);
    guest_cr3 = (uint64_t)(uintptr_t)g_guest_pml4;

    hype_npt_build_identity(g_npt_pml4, g_npt_pdpt, g_npt_pd, HYPE_NPT_MAX_GB);
    hype_npt_mark_not_present(g_npt_pd, HYPE_PCI_1_ECAM_GPA);
    npt_root_phys = (uint64_t)(uintptr_t)g_npt_pml4;

    hype_debug_print("pci-2: entry_rip=0x%llx ecam_gpa=0x%llx chosen_ahci_gpa=0x%llx\n",
                      (unsigned long long)entry_rip, (unsigned long long)HYPE_PCI_1_ECAM_GPA,
                      (unsigned long long)HYPE_PCI_2_AHCI_GPA);

    ctx = hype_svm_vcpu_create_long_mode(entry_rip, guest_cr3, rsp, npt_root_phys);
    if (ctx == 0) {
        hype_fatal("pci-2: vcpu_create_long_mode failed");
    }

    ahci_mapped = 0;
    ahci_mapped_base = 0;

    for (;;) {
        if (ops->vcpu_run(ctx, &info) != 0) {
            hype_fatal("pci-2: VM-entry failed (reason=0x%llx)", (unsigned long long)info.reason);
        }

        if (info.reason == HYPE_SVM_EXITCODE_NPF) {
            if (hype_svm_vcpu_handle_pci_ecam_npf(ctx, &g_pci_2_pci, HYPE_PCI_1_ECAM_GPA,
                                                (const uint8_t *)(uintptr_t)info.guest_rip) == 0) {
                /* A config-space write may just have set Memory Space
                 * Enable with a valid BAR5 already programmed -- if
                 * so, this is the exact moment a real PCI-aware
                 * hypervisor would map the device's now-known MMIO
                 * window. Only ever done once per run (ahci_mapped
                 * guards it) -- this test's guest never reprograms
                 * BAR5 after enabling it. */
                if (!ahci_mapped &&
                    hype_pci_memory_space_enabled(&g_pci_2_pci, HYPE_PCI_2_AHCI_DEV)) {
                    uint64_t bar5 = hype_pci_get_bar_value(&g_pci_2_pci, HYPE_PCI_2_AHCI_DEV, 5);
                    if (bar5 != 0) {
                        hype_npt_mark_not_present(g_npt_pd, bar5);
                        ahci_mapped_base = bar5;
                        ahci_mapped = 1;
                        hype_debug_print("pci-2: AHCI BAR5 enabled at 0x%llx -- NPT-mapping it now\n",
                                          (unsigned long long)bar5);
                    }
                }
                continue;
            }

            if (ahci_mapped &&
                hype_svm_vcpu_handle_ahci_npf(ctx, &g_pci_2_ahci, &g_pci_2_atapi, ahci_mapped_base) == 0) {
                continue;
            }

            hype_fatal("pci-2: unhandled NPF (qual=0x%llx guest_rip=0x%llx)",
                       (unsigned long long)info.qualification, (unsigned long long)info.guest_rip);
        }

        break;
    }

    if (info.reason != HYPE_SVM_EXITCODE_HLT) {
        hype_fatal("pci-2: test guest did not halt cleanly (reason=0x%llx guest_rip=0x%llx)",
                   (unsigned long long)info.reason, (unsigned long long)info.guest_rip);
    }
    if (!ahci_mapped || ahci_mapped_base != HYPE_PCI_2_AHCI_GPA) {
        hype_fatal("pci-2: AHCI device was never dynamically mapped at the chosen BAR5 address");
    }

    for (i = 0; i < sizeof(g_pci_2_dest_buffer); i++) {
        if (g_pci_2_dest_buffer[i] != g_pci_2_media[2 * HYPE_ATAPI_SECTOR_SIZE + i]) {
            hype_fatal("pci-2: READ(10) round trip mismatch at byte %llu", i);
        }
    }

    hype_debug_print(
        "pci-2: test guest halted cleanly (reason=0x%llx, guest_rip=0x%llx) -- AHCI discovered via "
        "PCI BAR5=0x%llx, %u-byte READ(10) round trip verified byte-for-byte\n",
        (unsigned long long)info.reason, (unsigned long long)info.guest_rip,
        (unsigned long long)ahci_mapped_base, HYPE_ATAPI_SECTOR_SIZE);
}

/*
 * VIDEO-3: a post-boot, PCI-discoverable Bochs-VBE-class display
 * adapter (devices/bochs_vbe.h) -- discovered via the same ECAM/BAR
 * enumeration sequence PCI-2 established for AHCI, then programmed and
 * written to exactly the way a real Linux vesafb/efifb or Windows
 * Basic Display Adapter driver would: probe/place BAR0 (framebuffer)
 * and BAR2 (DISPI registers), set Memory Space Enable, program
 * XRES/YRES/BPP/ENABLE over BAR2, then write pixels directly into
 * BAR0 -- no further register access needed per pixel (this is the
 * "no separate commit" real-hardware behavior devices/bochs_vbe.h's
 * own header comment documents).
 *
 * BAR0's chosen address is deliberately g_video_3_framebuffer's own
 * real static-buffer address, NOT an arbitrary formula-based GPA the
 * way BAR2/ECAM are -- pixel writes must land on genuinely backed,
 * readable-back memory (this project's NPT is a blanket identity map,
 * but there's no guarantee real DRAM actually backs an arbitrary high
 * GPA on real hardware/QEMU the way there is for hype.efi's own loaded
 * image), and BAR0 is deliberately never NPT-trapped at all, unlike
 * BAR2 -- pixel writes take zero VM-exits, the same way real VRAM
 * works.
 */
static uint8_t g_video_3_guest_code[192] __attribute__((aligned(4096)));
static uint8_t g_video_3_guest_stack[4096] __attribute__((aligned(4096)));
static uint8_t g_video_3_framebuffer[0x40000] __attribute__((aligned(4096)));
static uint8_t g_video_3_host_screen[0x40000] __attribute__((aligned(4096)));
static hype_pci_t g_video_3_pci;
static hype_bochs_vbe_t g_video_3_bochs_vbe;

#define HYPE_VIDEO_3_DISPLAY_DEV 1u
/* Same "arbitrary offset from an existing constant, always NPT-
 * trapped so real backing doesn't matter" scheme PCI-2 established for
 * AHCI's own BAR5 -- reused verbatim since each test guest builds its
 * own isolated hype_pci_t/NPT and these tests never run concurrently. */
#define HYPE_VIDEO_3_MMIO_GPA (HYPE_M4_3_PFLASH_GPA + 4ULL * 1024 * 1024)
#define HYPE_VIDEO_3_WIDTH 320u
#define HYPE_VIDEO_3_HEIGHT 200u
#define HYPE_VIDEO_3_BPP 32u
/* Power-of-two BAR0 size (hype_pci_set_bar_size()'s own requirement);
 * WIDTH*HEIGHT*4 = 256,000 bytes of actual pixel data fits comfortably
 * within it with headroom to spare. */
#define HYPE_VIDEO_3_FB_BAR_SIZE 0x40000u

/*
 * Section A (RBX = ECAM base): places BAR0 (framebuffer) and BAR2
 * (DISPI registers), then sets Memory Space Enable -- byte-for-byte
 * the same PCI enumeration idiom PCI-2's own payload established, just
 * with a second BAR to place.
 * Section B (RBX = the just-programmed MMIO/BAR2 address): programs
 * XRES/YRES/BPP/ENABLE via 16-bit (0x66-prefixed) MOVs -- DISPI
 * registers are architecturally 16-bit only.
 * Section C (RBX = the just-programmed framebuffer/BAR0 address):
 * writes a recognizable pattern to the first and last pixel of the
 * 320x200x32bpp mode just enabled, proving real-hardware-style direct
 * pixel access with no further register interaction.
 *
 *   mov rbx, <patched: ECAM base>            48 BB 00*8
 *   mov eax, <patched: framebuffer addr>     B8 00*4
 *   mov [rbx+0x8010], eax  (BAR0)            89 83 10 80 00 00
 *   mov eax, <patched: MMIO addr>            B8 00*4
 *   mov [rbx+0x8018], eax  (BAR2)            89 83 18 80 00 00
 *   mov eax, 2  (Memory Space Enable)        B8 02 00 00 00
 *   mov [rbx+0x8004], eax  (Command)         89 83 04 80 00 00
 *   mov rbx, <patched: MMIO addr>            48 BB 00*8
 *   mov ax, 320  (XRES)                      66 B8 40 01
 *   mov [rbx+0x502], ax                      66 89 83 02 05 00 00
 *   mov ax, 200  (YRES)                      66 B8 C8 00
 *   mov [rbx+0x504], ax                      66 89 83 04 05 00 00
 *   mov ax, 32  (BPP)                        66 B8 20 00
 *   mov [rbx+0x506], ax                      66 89 83 06 05 00 00
 *   mov ax, 0x41 (ENABLE=ENABLED|LFB_ENABLED) 66 B8 41 00
 *   mov [rbx+0x508], ax                      66 89 83 08 05 00 00
 *   mov rbx, <patched: framebuffer addr>     48 BB 00*8
 *   mov eax, <patched: first pixel value>    B8 00*4
 *   mov [rbx], eax                           89 03
 *   mov eax, <patched: last pixel value>     B8 00*4
 *   mov [rbx+<patched: last pixel offset>], eax  89 83 00*4
 *   hlt                                      F4
 *   jmp $-3                                  EB FD
 */
static const uint8_t g_video_3_payload_template[] = {
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x10, 0x80, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x18, 0x80, 0x00, 0x00,
    0xB8, 0x02, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x04, 0x80, 0x00, 0x00,
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x66, 0xB8, 0x40, 0x01,
    0x66, 0x89, 0x83, 0x02, 0x05, 0x00, 0x00,
    0x66, 0xB8, 0xC8, 0x00,
    0x66, 0x89, 0x83, 0x04, 0x05, 0x00, 0x00,
    0x66, 0xB8, 0x20, 0x00,
    0x66, 0x89, 0x83, 0x06, 0x05, 0x00, 0x00,
    0x66, 0xB8, 0x41, 0x00,
    0x66, 0x89, 0x83, 0x08, 0x05, 0x00, 0x00,
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x03,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x00, 0x00, 0x00, 0x00,
    0xF4,
    0xEB, 0xFD
};
#define HYPE_VIDEO_3_PAYLOAD_ECAM_RBX_IMM_OFFSET 2
#define HYPE_VIDEO_3_PAYLOAD_FB_BAR0_IMM_OFFSET 11
#define HYPE_VIDEO_3_PAYLOAD_MMIO_BAR2_IMM_OFFSET 22
#define HYPE_VIDEO_3_PAYLOAD_MMIO_RBX_IMM_OFFSET 45
#define HYPE_VIDEO_3_PAYLOAD_FB_RBX_IMM_OFFSET 99
#define HYPE_VIDEO_3_PAYLOAD_FIRST_PIXEL_VALUE_IMM_OFFSET 108
#define HYPE_VIDEO_3_PAYLOAD_LAST_PIXEL_VALUE_IMM_OFFSET 115
#define HYPE_VIDEO_3_PAYLOAD_LAST_PIXEL_OFFSET_IMM_OFFSET 121

/* Top byte (the "X"/reserved channel in XRGB8888) is deliberately 0 --
 * hype_fb_blit_copy()'s own pack_rgb() legitimately zeroes it on every
 * repack (it's padding, not real pixel data), so a nonzero reserved
 * byte here would never survive the blit round trip below, for
 * reasons that have nothing to do with a blit bug. */
#define HYPE_VIDEO_3_FIRST_PIXEL_VALUE 0x00223344u
#define HYPE_VIDEO_3_LAST_PIXEL_VALUE 0x00667788u

static void run_video_3_test(const hype_vmm_ops_t *ops, hype_vmm_kind_t kind) {
    unsigned long long i;
    uint64_t entry_rip, guest_cr3, rsp, npt_root_phys;
    uint64_t fb_phys, mmio_phys;
    uint32_t stride_bytes, last_pixel_offset;
    hype_vcpu_ctx_t *ctx;
    hype_vmexit_info_t info;
    int mmio_mapped;
    uint64_t mmio_mapped_base;
    hype_bochs_vbe_mode_t mode;

    if (kind != HYPE_VMM_KIND_SVM) {
        hype_serial_print("video-3: skipped -- %s has no working vcpu_run yet (see vmx_ops.c)\n",
                           ops->name);
        return;
    }

    hype_guest_ram_zero(g_video_3_guest_code, sizeof(g_video_3_guest_code));
    hype_guest_ram_zero(g_video_3_guest_stack, sizeof(g_video_3_guest_stack));
    hype_guest_ram_zero(g_video_3_framebuffer, sizeof(g_video_3_framebuffer));
    hype_guest_ram_zero(g_video_3_host_screen, sizeof(g_video_3_host_screen));

    hype_bochs_vbe_reset(&g_video_3_bochs_vbe);
    hype_pci_reset(&g_video_3_pci);
    hype_pci_add_device(&g_video_3_pci, HYPE_VIDEO_3_DISPLAY_DEV, HYPE_BOCHS_VBE_PCI_VENDOR_ID,
                         HYPE_BOCHS_VBE_PCI_DEVICE_ID, HYPE_BOCHS_VBE_PCI_CLASS_BASE,
                         HYPE_BOCHS_VBE_PCI_CLASS_SUB, HYPE_BOCHS_VBE_PCI_CLASS_INTERFACE);
    hype_pci_set_bar_size(&g_video_3_pci, HYPE_VIDEO_3_DISPLAY_DEV, 0, HYPE_VIDEO_3_FB_BAR_SIZE);
    hype_pci_set_bar_size(&g_video_3_pci, HYPE_VIDEO_3_DISPLAY_DEV, 2, HYPE_BOCHS_VBE_MMIO_SIZE);

    fb_phys = (uint64_t)(uintptr_t)g_video_3_framebuffer;
    mmio_phys = HYPE_VIDEO_3_MMIO_GPA;
    stride_bytes = HYPE_VIDEO_3_WIDTH * (HYPE_VIDEO_3_BPP / 8u);
    last_pixel_offset = stride_bytes * (HYPE_VIDEO_3_HEIGHT - 1u) + (HYPE_VIDEO_3_WIDTH - 1u) * (HYPE_VIDEO_3_BPP / 8u);

    for (i = 0; i < sizeof(g_video_3_payload_template); i++) {
        g_video_3_guest_code[i] = g_video_3_payload_template[i];
    }
    hype_write_le64(g_video_3_guest_code + HYPE_VIDEO_3_PAYLOAD_ECAM_RBX_IMM_OFFSET, HYPE_PCI_1_ECAM_GPA);
    hype_write_le32(g_video_3_guest_code + HYPE_VIDEO_3_PAYLOAD_FB_BAR0_IMM_OFFSET, (uint32_t)fb_phys);
    hype_write_le32(g_video_3_guest_code + HYPE_VIDEO_3_PAYLOAD_MMIO_BAR2_IMM_OFFSET, (uint32_t)mmio_phys);
    hype_write_le64(g_video_3_guest_code + HYPE_VIDEO_3_PAYLOAD_MMIO_RBX_IMM_OFFSET, mmio_phys);
    hype_write_le64(g_video_3_guest_code + HYPE_VIDEO_3_PAYLOAD_FB_RBX_IMM_OFFSET, fb_phys);
    hype_write_le32(g_video_3_guest_code + HYPE_VIDEO_3_PAYLOAD_FIRST_PIXEL_VALUE_IMM_OFFSET,
                     HYPE_VIDEO_3_FIRST_PIXEL_VALUE);
    hype_write_le32(g_video_3_guest_code + HYPE_VIDEO_3_PAYLOAD_LAST_PIXEL_VALUE_IMM_OFFSET,
                     HYPE_VIDEO_3_LAST_PIXEL_VALUE);
    hype_write_le32(g_video_3_guest_code + HYPE_VIDEO_3_PAYLOAD_LAST_PIXEL_OFFSET_IMM_OFFSET,
                     last_pixel_offset);

    entry_rip = (uint64_t)(uintptr_t)g_video_3_guest_code;
    rsp = (uint64_t)(uintptr_t)(g_video_3_guest_stack + sizeof(g_video_3_guest_stack));

    hype_paging_build_identity(g_guest_pml4, g_guest_pdpt, g_guest_pd, HYPE_M3_5_GUEST_PAGING_GB);
    guest_cr3 = (uint64_t)(uintptr_t)g_guest_pml4;

    hype_npt_build_identity(g_npt_pml4, g_npt_pdpt, g_npt_pd, HYPE_NPT_MAX_GB);
    hype_npt_mark_not_present(g_npt_pd, HYPE_PCI_1_ECAM_GPA);
    npt_root_phys = (uint64_t)(uintptr_t)g_npt_pml4;

    hype_debug_print("video-3: entry_rip=0x%llx ecam_gpa=0x%llx fb_addr=0x%llx mmio_addr=0x%llx\n",
                      (unsigned long long)entry_rip, (unsigned long long)HYPE_PCI_1_ECAM_GPA,
                      (unsigned long long)fb_phys, (unsigned long long)mmio_phys);

    ctx = hype_svm_vcpu_create_long_mode(entry_rip, guest_cr3, rsp, npt_root_phys);
    if (ctx == 0) {
        hype_fatal("video-3: vcpu_create_long_mode failed");
    }

    mmio_mapped = 0;
    mmio_mapped_base = 0;

    for (;;) {
        if (ops->vcpu_run(ctx, &info) != 0) {
            hype_fatal("video-3: VM-entry failed (reason=0x%llx)", (unsigned long long)info.reason);
        }

        if (info.reason == HYPE_SVM_EXITCODE_NPF) {
            if (hype_svm_vcpu_handle_pci_ecam_npf(ctx, &g_video_3_pci, HYPE_PCI_1_ECAM_GPA,
                                                (const uint8_t *)(uintptr_t)info.guest_rip) == 0) {
                if (!mmio_mapped && hype_pci_memory_space_enabled(&g_video_3_pci, HYPE_VIDEO_3_DISPLAY_DEV)) {
                    uint64_t bar2 = hype_pci_get_bar_value(&g_video_3_pci, HYPE_VIDEO_3_DISPLAY_DEV, 2);
                    if (bar2 != 0) {
                        hype_npt_mark_not_present(g_npt_pd, bar2);
                        mmio_mapped_base = bar2;
                        mmio_mapped = 1;
                        hype_debug_print(
                            "video-3: display BAR2 (MMIO) enabled at 0x%llx -- NPT-mapping it now\n",
                            (unsigned long long)bar2);
                    }
                }
                continue;
            }

            if (mmio_mapped &&
                hype_svm_vcpu_handle_bochs_vbe_npf(ctx, &g_video_3_bochs_vbe, mmio_mapped_base) == 0) {
                continue;
            }

            hype_fatal("video-3: unhandled NPF (qual=0x%llx guest_rip=0x%llx)",
                       (unsigned long long)info.qualification, (unsigned long long)info.guest_rip);
        }

        break;
    }

    if (info.reason != HYPE_SVM_EXITCODE_HLT) {
        hype_fatal("video-3: test guest did not halt cleanly (reason=0x%llx guest_rip=0x%llx)",
                   (unsigned long long)info.reason, (unsigned long long)info.guest_rip);
    }
    if (!mmio_mapped || mmio_mapped_base != HYPE_VIDEO_3_MMIO_GPA) {
        hype_fatal("video-3: display adapter's MMIO BAR2 was never dynamically mapped");
    }

    hype_bochs_vbe_get_mode(&g_video_3_bochs_vbe, &mode);
    if (!mode.valid || mode.width != HYPE_VIDEO_3_WIDTH || mode.height != HYPE_VIDEO_3_HEIGHT ||
        mode.bytes_per_pixel != HYPE_VIDEO_3_BPP / 8u || mode.stride_bytes != stride_bytes ||
        mode.fb_offset_bytes != 0) {
        hype_fatal("video-3: guest-programmed display mode was not decoded as expected");
    }

    if (hype_read_le32(g_video_3_framebuffer + 0) != HYPE_VIDEO_3_FIRST_PIXEL_VALUE) {
        hype_fatal("video-3: first pixel write did not land in the framebuffer");
    }
    if (hype_read_le32(g_video_3_framebuffer + last_pixel_offset) != HYPE_VIDEO_3_LAST_PIXEL_VALUE) {
        hype_fatal("video-3: last pixel write did not land in the framebuffer");
    }

    /* Prove the other half of VIDEO-3 (VIDEO-2's own task.md note: the
     * actual blit onto the host's real screen is this milestone's job)
     * against this device's own real, guest-programmed output -- not
     * just synthetic buffers in test_fb_blit.c's own unit tests. */
    hype_fb_blit_copy(g_video_3_framebuffer, mode.width, mode.height, mode.stride_bytes,
                       HYPE_FB_PIXEL_FORMAT_XRGB8888, g_video_3_host_screen, mode.width, mode.height,
                       stride_bytes, HYPE_FB_PIXEL_FORMAT_XRGB8888);
    if (hype_read_le32(g_video_3_host_screen + 0) != HYPE_VIDEO_3_FIRST_PIXEL_VALUE) {
        hype_fatal("video-3: blit did not carry the first pixel to the host screen buffer");
    }
    if (hype_read_le32(g_video_3_host_screen + last_pixel_offset) != HYPE_VIDEO_3_LAST_PIXEL_VALUE) {
        hype_fatal("video-3: blit did not carry the last pixel to the host screen buffer");
    }

    hype_debug_print(
        "video-3: test guest halted cleanly (reason=0x%llx, guest_rip=0x%llx) -- display adapter "
        "discovered via PCI BAR0=0x%llx/BAR2=0x%llx, %ux%u@%ubpp mode decoded correctly, "
        "first/last pixel round-tripped through the framebuffer and the host-screen blit\n",
        (unsigned long long)info.reason, (unsigned long long)info.guest_rip,
        (unsigned long long)fb_phys, (unsigned long long)mmio_mapped_base, mode.width, mode.height,
        mode.bytes_per_pixel * 8u);
}

/*
 * M5-1: a modern (non-transitional) virtio-blk PCI device
 * (devices/virtio_blk.h) -- what a real Linux/BSD guest's own inbox
 * virtio_blk driver discovers and drives for disk I/O. A genuine
 * host-file-backed store is M5-3's own job ("blk_backend"); this
 * milestone's own scope is the device/transport itself, backed by a
 * fixed in-memory buffer standing in for a real disk.
 *
 * Unlike every earlier test guest here, the virtqueue's own
 * descriptor table/avail ring/used ring and the two requests' own
 * header/data/status buffers are pre-built by this HOST-side setup
 * code, not by the guest's own instruction stream -- this mirrors how
 * a real device's DMA engine reads/writes guest memory independently
 * of the guest CPU's own instruction stream; the guest payload below
 * only ever touches PCI/MMIO registers (every access NPF-routed), the
 * same "device does DMA, CPU never touches virtqueue memory directly"
 * split real hardware has. This also matches this project's own
 * existing precedent (PCI-2's AHCI command-list/table bytes are
 * likewise host-C-constructed, not built by guest asm).
 *
 * Exercises BOTH directions in one run: request 1 is a WRITE
 * (VIRTIO_BLK_T_OUT, guest -> backing store) at a fabricated sector,
 * request 2 is a READ (VIRTIO_BLK_T_IN, backing store -> guest) at a
 * different sector the host pre-fills with its own recognizable
 * pattern -- both chains are queued before a single NOTIFY kick (the
 * device's own process-the-queue loop drains every newly-avail entry
 * per notify, not just one), then the guest polls the used ring until
 * both have completed.
 */
static uint8_t g_m5_1_guest_code[512] __attribute__((aligned(4096)));
static uint8_t g_m5_1_guest_stack[4096] __attribute__((aligned(4096)));
static uint8_t g_m5_1_backing[0x10000] __attribute__((aligned(4096))); /* 128 sectors */
static uint8_t g_m5_1_desc_table[128] __attribute__((aligned(4096)));  /* 8 * 16 bytes */
static uint8_t g_m5_1_avail[22] __attribute__((aligned(4096)));
static uint8_t g_m5_1_used[70] __attribute__((aligned(4096)));
static uint8_t g_m5_1_req1_header[16] __attribute__((aligned(4096)));
static uint8_t g_m5_1_req1_data[16] __attribute__((aligned(4096)));
static uint8_t g_m5_1_req1_status[1] __attribute__((aligned(4096)));
static uint8_t g_m5_1_req2_header[16] __attribute__((aligned(4096)));
static uint8_t g_m5_1_req2_data[16] __attribute__((aligned(4096)));
static uint8_t g_m5_1_req2_status[1] __attribute__((aligned(4096)));
static hype_pci_t g_m5_1_pci;
static hype_virtio_blk_t g_m5_1_virtio_blk;

#define HYPE_M5_1_VIRTIO_DEV 1u
#define HYPE_M5_1_BAR_INDEX 4u
/* Same "arbitrary offset from an existing constant, always NPT-
 * trapped so real backing doesn't matter" scheme PCI-2/VIDEO-3 both
 * established. */
#define HYPE_M5_1_MMIO_GPA (HYPE_M4_3_PFLASH_GPA + 4ULL * 1024 * 1024)
#define HYPE_M5_1_CAPACITY_SECTORS 128u
#define HYPE_M5_1_REQ1_SECTOR 3ull
#define HYPE_M5_1_REQ2_SECTOR 10ull
#define HYPE_M5_1_DATA_LEN 16u

#define HYPE_M5_1_CAP_COMMON_OFF 0x40u
#define HYPE_M5_1_CAP_NOTIFY_OFF 0x50u
#define HYPE_M5_1_CAP_ISR_OFF 0x64u
#define HYPE_M5_1_CAP_DEVICE_OFF 0x74u
#define HYPE_M5_1_CFG_TYPE_COMMON 1u
#define HYPE_M5_1_CFG_TYPE_NOTIFY 2u
#define HYPE_M5_1_CFG_TYPE_ISR 3u
#define HYPE_M5_1_CFG_TYPE_DEVICE 4u
#define HYPE_M5_1_PCI_CAP_ID_VENDOR_SPECIFIC 0x09u
#define HYPE_M5_1_PCI_STATUS_OFFSET 0x06u
#define HYPE_M5_1_PCI_STATUS_CAP_LIST 0x10u
#define HYPE_M5_1_PCI_CAP_POINTER_OFFSET 0x34u

/* Writes one 16-byte virtio_pci_cap structure (spec §4.1.4) directly
 * into a device's own raw config-space bytes -- real hardware has
 * these burned into the device's own ROM/ASIC; this project's PCI
 * model exposes hype_pci_device_t.config[] as a plain public byte
 * array specifically so host-side device setup code can synthesize
 * fixed structures like this one directly, the same way devices/
 * acpi_loader.h's table synthesis works. */
static void hype_write_virtio_pci_cap(uint8_t *config, uint8_t cap_offset, uint8_t cap_next,
                                       uint8_t cap_len, uint8_t cfg_type, uint8_t bar_index,
                                       uint32_t region_offset, uint32_t region_length) {
    config[cap_offset + 0] = HYPE_M5_1_PCI_CAP_ID_VENDOR_SPECIFIC;
    config[cap_offset + 1] = cap_next;
    config[cap_offset + 2] = cap_len;
    config[cap_offset + 3] = cfg_type;
    config[cap_offset + 4] = bar_index;
    config[cap_offset + 5] = 0;
    config[cap_offset + 6] = 0;
    config[cap_offset + 7] = 0;
    hype_write_le32(config + cap_offset + 8, region_offset);
    hype_write_le32(config + cap_offset + 12, region_length);
}

static void hype_write_virtq_desc(uint8_t *raw, uint64_t addr, uint32_t len, uint16_t flags,
                                   uint16_t next) {
    hype_write_le64(raw + 0, addr);
    hype_write_le32(raw + 8, len);
    hype_write_le16(raw + 12, flags);
    hype_write_le16(raw + 14, next);
}

/*
 * Section A (RBX = ECAM base): places BAR4 (the device's single MMIO
 * region) and sets Memory Space Enable -- PCI-2/VIDEO-3's own
 * established idiom, one BAR this time.
 * Section B (RBX = MMIO addr): the real virtio device-init handshake
 * (spec's own "Device Initialization" sequence) -- reset, ACKNOWLEDGE,
 * DRIVER, negotiate the one offered feature bit (read then accept,
 * not just blindly assumed), FEATURES_OK, program the single
 * virtqueue's three addresses, enable it, DRIVER_OK.
 * Section C (RBX = MMIO addr + NOTIFY_CFG offset): a single kick --
 * both pre-built request chains are already queued (avail.idx=2), so
 * one notify drains both.
 * Section D (RBX = used ring addr): poll used.idx until both
 * requests have completed.
 *
 *   mov rbx, <patched: ECAM base>              48 BB 00*8
 *   mov eax, <patched: MMIO addr>               B8 00*4
 *   mov [rbx+0x8020], eax  (BAR4)               89 83 20 80 00 00
 *   mov eax, 2  (Memory Space Enable)           B8 02 00 00 00
 *   mov [rbx+0x8004], eax  (Command)            89 83 04 80 00 00
 *   mov rbx, <patched: MMIO addr>                48 BB 00*8
 *   mov al, 0                                    B0 00
 *   mov [rbx+0x14], al  (device_status=0)        88 83 14 00 00 00
 *   mov al, 1                                    B0 01
 *   mov [rbx+0x14], al  (ACKNOWLEDGE)             88 83 14 00 00 00
 *   mov al, 3                                     B0 03
 *   mov [rbx+0x14], al  (ACKNOWLEDGE|DRIVER)      88 83 14 00 00 00
 *   mov eax, 1                                    B8 01 00 00 00
 *   mov [rbx+0x00], eax (device_feature_select=1) 89 83 00 00 00 00
 *   mov eax, [rbx+0x04] (read device_feature)     8B 83 04 00 00 00
 *   mov ecx, 1                                    B9 01 00 00 00
 *   mov [rbx+0x08], ecx (driver_feature_select=1) 89 8B 08 00 00 00
 *   mov [rbx+0x0C], eax (driver_feature=eax)      89 83 0C 00 00 00
 *   mov al, 0x0B (+FEATURES_OK)                   B0 0B
 *   mov [rbx+0x14], al                            88 83 14 00 00 00
 *   mov ax, 0                                     66 B8 00 00
 *   mov [rbx+0x16], ax  (queue_select=0)          66 89 83 16 00 00 00
 *   mov eax, <patched: desc_table addr low32>     B8 00*4
 *   mov [rbx+0x20], eax (queue_desc_lo)           89 83 20 00 00 00
 *   mov eax, <patched: desc_table addr high32>    B8 00*4
 *   mov [rbx+0x24], eax (queue_desc_hi)           89 83 24 00 00 00
 *   mov eax, <patched: avail addr low32>          B8 00*4
 *   mov [rbx+0x28], eax (queue_driver_lo)         89 83 28 00 00 00
 *   mov eax, <patched: avail addr high32>         B8 00*4
 *   mov [rbx+0x2C], eax (queue_driver_hi)         89 83 2C 00 00 00
 *   mov eax, <patched: used addr low32>           B8 00*4
 *   mov [rbx+0x30], eax (queue_device_lo)         89 83 30 00 00 00
 *   mov eax, <patched: used addr high32>          B8 00*4
 *   mov [rbx+0x34], eax (queue_device_hi)         89 83 34 00 00 00
 *   mov ax, 1                                     66 B8 01 00
 *   mov [rbx+0x1C], ax  (queue_enable=1)          66 89 83 1C 00 00 00
 *   mov al, 0x0F (+DRIVER_OK)                     B0 0F
 *   mov [rbx+0x14], al                            88 83 14 00 00 00
 *   mov rbx, <patched: MMIO addr + 0x1000>        48 BB 00*8
 *   mov eax, 0                                    B8 00 00 00 00
 *   mov [rbx], eax  (notify kick)                 89 03
 *   mov rbx, <patched: used ring addr>            48 BB 00*8
 * poll:
 *   mov ax, [rbx+2]  (used.idx)                   66 8B 83 02 00 00 00
 *   cmp ax, 2                                     66 3D 02 00
 *   jne poll                                       75 F3
 *   hlt                                            F4
 *   jmp $-3                                        EB FD
 */
static const uint8_t g_m5_1_payload_template[] = {
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x20, 0x80, 0x00, 0x00,
    0xB8, 0x02, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x04, 0x80, 0x00, 0x00,
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB0, 0x00,
    0x88, 0x83, 0x14, 0x00, 0x00, 0x00,
    0xB0, 0x01,
    0x88, 0x83, 0x14, 0x00, 0x00, 0x00,
    0xB0, 0x03,
    0x88, 0x83, 0x14, 0x00, 0x00, 0x00,
    0xB8, 0x01, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x00, 0x00, 0x00, 0x00,
    0x8B, 0x83, 0x04, 0x00, 0x00, 0x00,
    0xB9, 0x01, 0x00, 0x00, 0x00,
    0x89, 0x8B, 0x08, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x0C, 0x00, 0x00, 0x00,
    0xB0, 0x0B,
    0x88, 0x83, 0x14, 0x00, 0x00, 0x00,
    0x66, 0xB8, 0x00, 0x00,
    0x66, 0x89, 0x83, 0x16, 0x00, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x20, 0x00, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x24, 0x00, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x28, 0x00, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x2C, 0x00, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x30, 0x00, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x34, 0x00, 0x00, 0x00,
    0x66, 0xB8, 0x01, 0x00,
    0x66, 0x89, 0x83, 0x1C, 0x00, 0x00, 0x00,
    0xB0, 0x0F,
    0x88, 0x83, 0x14, 0x00, 0x00, 0x00,
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x03,
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x66, 0x8B, 0x83, 0x02, 0x00, 0x00, 0x00,
    0x66, 0x3D, 0x02, 0x00,
    0x75, 0xF3,
    0xF4,
    0xEB, 0xFD
};
#define HYPE_M5_1_PAYLOAD_ECAM_RBX_IMM_OFFSET 2
#define HYPE_M5_1_PAYLOAD_BAR4_VALUE_IMM_OFFSET 11
#define HYPE_M5_1_PAYLOAD_MMIO_RBX_IMM_OFFSET 34
#define HYPE_M5_1_PAYLOAD_DESC_LOW_IMM_OFFSET 120
#define HYPE_M5_1_PAYLOAD_DESC_HIGH_IMM_OFFSET 131
#define HYPE_M5_1_PAYLOAD_AVAIL_LOW_IMM_OFFSET 142
#define HYPE_M5_1_PAYLOAD_AVAIL_HIGH_IMM_OFFSET 153
#define HYPE_M5_1_PAYLOAD_USED_LOW_IMM_OFFSET 164
#define HYPE_M5_1_PAYLOAD_USED_HIGH_IMM_OFFSET 175
#define HYPE_M5_1_PAYLOAD_NOTIFY_RBX_IMM_OFFSET 206
#define HYPE_M5_1_PAYLOAD_USED_RING_RBX_IMM_OFFSET 223

static void run_m5_1_test(const hype_vmm_ops_t *ops, hype_vmm_kind_t kind) {
    unsigned long long i;
    uint64_t entry_rip, guest_cr3, rsp, npt_root_phys;
    uint64_t mmio_phys, desc_phys, avail_phys, used_phys;
    uint8_t *config;
    hype_vcpu_ctx_t *ctx;
    hype_vmexit_info_t info;
    int mmio_mapped;
    uint64_t mmio_mapped_base;
    uint64_t sector10_offset;

    if (kind != HYPE_VMM_KIND_SVM) {
        hype_serial_print("m5-1: skipped -- %s has no working vcpu_run yet (see vmx_ops.c)\n", ops->name);
        return;
    }

    hype_guest_ram_zero(g_m5_1_guest_code, sizeof(g_m5_1_guest_code));
    hype_guest_ram_zero(g_m5_1_guest_stack, sizeof(g_m5_1_guest_stack));
    hype_guest_ram_zero(g_m5_1_backing, sizeof(g_m5_1_backing));
    hype_guest_ram_zero(g_m5_1_desc_table, sizeof(g_m5_1_desc_table));
    hype_guest_ram_zero(g_m5_1_avail, sizeof(g_m5_1_avail));
    hype_guest_ram_zero(g_m5_1_used, sizeof(g_m5_1_used));
    hype_guest_ram_zero(g_m5_1_req1_header, sizeof(g_m5_1_req1_header));
    hype_guest_ram_zero(g_m5_1_req1_data, sizeof(g_m5_1_req1_data));
    hype_guest_ram_zero(g_m5_1_req1_status, sizeof(g_m5_1_req1_status));
    hype_guest_ram_zero(g_m5_1_req2_header, sizeof(g_m5_1_req2_header));
    hype_guest_ram_zero(g_m5_1_req2_data, sizeof(g_m5_1_req2_data));
    hype_guest_ram_zero(g_m5_1_req2_status, sizeof(g_m5_1_req2_status));

    /* Request 1 (WRITE): the guest wants to persist this pattern at
     * HYPE_M5_1_REQ1_SECTOR. */
    hype_write_le32(g_m5_1_req1_header + 0, HYPE_VIRTIO_BLK_T_OUT);
    hype_write_le32(g_m5_1_req1_header + 4, 0);
    hype_write_le64(g_m5_1_req1_header + 8, HYPE_M5_1_REQ1_SECTOR);
    for (i = 0; i < HYPE_M5_1_DATA_LEN; i++) {
        g_m5_1_req1_data[i] = (uint8_t)(0xA0u + i);
    }
    g_m5_1_req1_status[0] = 0xFFu; /* poison -- expect the device to overwrite with S_OK (0) */

    /* Request 2 (READ): the host places its own known pattern on the
     * "disk" at HYPE_M5_1_REQ2_SECTOR first, standing in for data a
     * prior write (or the disk image itself) already put there. */
    sector10_offset = HYPE_M5_1_REQ2_SECTOR * HYPE_VIRTIO_BLK_SECTOR_SIZE;
    for (i = 0; i < HYPE_M5_1_DATA_LEN; i++) {
        g_m5_1_backing[sector10_offset + i] = (uint8_t)(0xB0u + i);
    }
    hype_write_le32(g_m5_1_req2_header + 0, HYPE_VIRTIO_BLK_T_IN);
    hype_write_le32(g_m5_1_req2_header + 4, 0);
    hype_write_le64(g_m5_1_req2_header + 8, HYPE_M5_1_REQ2_SECTOR);
    g_m5_1_req2_status[0] = 0xFFu;

    /* Descriptor table: two 3-descriptor chains (header/data/status). */
    hype_write_virtq_desc(g_m5_1_desc_table + 0 * 16, (uint64_t)(uintptr_t)g_m5_1_req1_header, 16,
                           HYPE_VIRTQ_DESC_F_NEXT, 1);
    hype_write_virtq_desc(g_m5_1_desc_table + 1 * 16, (uint64_t)(uintptr_t)g_m5_1_req1_data,
                           HYPE_M5_1_DATA_LEN, HYPE_VIRTQ_DESC_F_NEXT, 2);
    hype_write_virtq_desc(g_m5_1_desc_table + 2 * 16, (uint64_t)(uintptr_t)g_m5_1_req1_status, 1,
                           HYPE_VIRTQ_DESC_F_WRITE, 0);
    hype_write_virtq_desc(g_m5_1_desc_table + 3 * 16, (uint64_t)(uintptr_t)g_m5_1_req2_header, 16,
                           HYPE_VIRTQ_DESC_F_NEXT, 4);
    hype_write_virtq_desc(g_m5_1_desc_table + 4 * 16, (uint64_t)(uintptr_t)g_m5_1_req2_data,
                           HYPE_M5_1_DATA_LEN, (uint16_t)(HYPE_VIRTQ_DESC_F_NEXT | HYPE_VIRTQ_DESC_F_WRITE),
                           5);
    hype_write_virtq_desc(g_m5_1_desc_table + 5 * 16, (uint64_t)(uintptr_t)g_m5_1_req2_status, 1,
                           HYPE_VIRTQ_DESC_F_WRITE, 0);

    /* Avail ring: both chains queued up front (idx=2), so the guest's
     * own single notify kick below drains both. */
    hype_write_le16(g_m5_1_avail + 0, 0);
    hype_write_le16(g_m5_1_avail + 2, 2);
    hype_write_le16(g_m5_1_avail + 4, 0); /* ring[0] = head descriptor of chain 1 */
    hype_write_le16(g_m5_1_avail + 6, 3); /* ring[1] = head descriptor of chain 2 */

    hype_virtio_blk_reset(&g_m5_1_virtio_blk, HYPE_M5_1_CAPACITY_SECTORS);
    hype_pci_reset(&g_m5_1_pci);
    hype_pci_add_device(&g_m5_1_pci, HYPE_M5_1_VIRTIO_DEV, HYPE_VIRTIO_BLK_PCI_VENDOR_ID,
                         HYPE_VIRTIO_BLK_PCI_DEVICE_ID, HYPE_VIRTIO_BLK_PCI_CLASS_BASE,
                         HYPE_VIRTIO_BLK_PCI_CLASS_SUB, HYPE_VIRTIO_BLK_PCI_CLASS_INTERFACE);
    hype_pci_set_bar_size(&g_m5_1_pci, HYPE_M5_1_VIRTIO_DEV, HYPE_M5_1_BAR_INDEX, HYPE_VIRTIO_BLK_BAR_SIZE);

    /* Real virtio-pci capability list (spec §4.1.4) -- not walked by
     * this synthetic guest (which targets BAR4 directly, the same
     * "test guest knows the device's own structure" convention PCI-2/
     * VIDEO-3 already established), but built faithfully so a real
     * guest OS driver's own generic capability walk would find it. */
    config = g_m5_1_pci.devices[HYPE_M5_1_VIRTIO_DEV].config;
    config[HYPE_M5_1_PCI_STATUS_OFFSET] |= HYPE_M5_1_PCI_STATUS_CAP_LIST;
    config[HYPE_M5_1_PCI_CAP_POINTER_OFFSET] = HYPE_M5_1_CAP_COMMON_OFF;
    hype_write_virtio_pci_cap(config, HYPE_M5_1_CAP_COMMON_OFF, HYPE_M5_1_CAP_NOTIFY_OFF, 16,
                               HYPE_M5_1_CFG_TYPE_COMMON, HYPE_M5_1_BAR_INDEX,
                               HYPE_VIRTIO_BLK_BAR_COMMON_CFG_OFFSET, HYPE_VIRTIO_COMMON_CFG_SIZE);
    hype_write_virtio_pci_cap(config, HYPE_M5_1_CAP_NOTIFY_OFF, HYPE_M5_1_CAP_ISR_OFF, 20,
                               HYPE_M5_1_CFG_TYPE_NOTIFY, HYPE_M5_1_BAR_INDEX,
                               HYPE_VIRTIO_BLK_BAR_NOTIFY_CFG_OFFSET, 4);
    hype_write_le32(config + HYPE_M5_1_CAP_NOTIFY_OFF + 16, HYPE_VIRTIO_BLK_BAR_NOTIFY_CFG_MULTIPLIER);
    hype_write_virtio_pci_cap(config, HYPE_M5_1_CAP_ISR_OFF, HYPE_M5_1_CAP_DEVICE_OFF, 16,
                               HYPE_M5_1_CFG_TYPE_ISR, HYPE_M5_1_BAR_INDEX,
                               HYPE_VIRTIO_BLK_BAR_ISR_CFG_OFFSET, 1);
    hype_write_virtio_pci_cap(config, HYPE_M5_1_CAP_DEVICE_OFF, 0, 16, HYPE_M5_1_CFG_TYPE_DEVICE,
                               HYPE_M5_1_BAR_INDEX, HYPE_VIRTIO_BLK_BAR_DEVICE_CFG_OFFSET,
                               HYPE_VIRTIO_BLK_CFG_SIZE);

    mmio_phys = HYPE_M5_1_MMIO_GPA;
    desc_phys = (uint64_t)(uintptr_t)g_m5_1_desc_table;
    avail_phys = (uint64_t)(uintptr_t)g_m5_1_avail;
    used_phys = (uint64_t)(uintptr_t)g_m5_1_used;

    for (i = 0; i < sizeof(g_m5_1_payload_template); i++) {
        g_m5_1_guest_code[i] = g_m5_1_payload_template[i];
    }
    hype_write_le64(g_m5_1_guest_code + HYPE_M5_1_PAYLOAD_ECAM_RBX_IMM_OFFSET, HYPE_PCI_1_ECAM_GPA);
    hype_write_le32(g_m5_1_guest_code + HYPE_M5_1_PAYLOAD_BAR4_VALUE_IMM_OFFSET, (uint32_t)mmio_phys);
    hype_write_le64(g_m5_1_guest_code + HYPE_M5_1_PAYLOAD_MMIO_RBX_IMM_OFFSET, mmio_phys);
    hype_write_le32(g_m5_1_guest_code + HYPE_M5_1_PAYLOAD_DESC_LOW_IMM_OFFSET, (uint32_t)desc_phys);
    hype_write_le32(g_m5_1_guest_code + HYPE_M5_1_PAYLOAD_DESC_HIGH_IMM_OFFSET, (uint32_t)(desc_phys >> 32));
    hype_write_le32(g_m5_1_guest_code + HYPE_M5_1_PAYLOAD_AVAIL_LOW_IMM_OFFSET, (uint32_t)avail_phys);
    hype_write_le32(g_m5_1_guest_code + HYPE_M5_1_PAYLOAD_AVAIL_HIGH_IMM_OFFSET,
                     (uint32_t)(avail_phys >> 32));
    hype_write_le32(g_m5_1_guest_code + HYPE_M5_1_PAYLOAD_USED_LOW_IMM_OFFSET, (uint32_t)used_phys);
    hype_write_le32(g_m5_1_guest_code + HYPE_M5_1_PAYLOAD_USED_HIGH_IMM_OFFSET, (uint32_t)(used_phys >> 32));
    hype_write_le64(g_m5_1_guest_code + HYPE_M5_1_PAYLOAD_NOTIFY_RBX_IMM_OFFSET,
                     mmio_phys + HYPE_VIRTIO_BLK_BAR_NOTIFY_CFG_OFFSET);
    hype_write_le64(g_m5_1_guest_code + HYPE_M5_1_PAYLOAD_USED_RING_RBX_IMM_OFFSET, used_phys);

    entry_rip = (uint64_t)(uintptr_t)g_m5_1_guest_code;
    rsp = (uint64_t)(uintptr_t)(g_m5_1_guest_stack + sizeof(g_m5_1_guest_stack));

    hype_paging_build_identity(g_guest_pml4, g_guest_pdpt, g_guest_pd, HYPE_M3_5_GUEST_PAGING_GB);
    guest_cr3 = (uint64_t)(uintptr_t)g_guest_pml4;

    hype_npt_build_identity(g_npt_pml4, g_npt_pdpt, g_npt_pd, HYPE_NPT_MAX_GB);
    hype_npt_mark_not_present(g_npt_pd, HYPE_PCI_1_ECAM_GPA);
    npt_root_phys = (uint64_t)(uintptr_t)g_npt_pml4;

    hype_debug_print("m5-1: entry_rip=0x%llx ecam_gpa=0x%llx mmio_addr=0x%llx\n",
                      (unsigned long long)entry_rip, (unsigned long long)HYPE_PCI_1_ECAM_GPA,
                      (unsigned long long)mmio_phys);

    ctx = hype_svm_vcpu_create_long_mode(entry_rip, guest_cr3, rsp, npt_root_phys);
    if (ctx == 0) {
        hype_fatal("m5-1: vcpu_create_long_mode failed");
    }

    mmio_mapped = 0;
    mmio_mapped_base = 0;

    for (;;) {
        if (ops->vcpu_run(ctx, &info) != 0) {
            hype_fatal("m5-1: VM-entry failed (reason=0x%llx)", (unsigned long long)info.reason);
        }

        if (info.reason == HYPE_SVM_EXITCODE_NPF) {
            if (hype_svm_vcpu_handle_pci_ecam_npf(ctx, &g_m5_1_pci, HYPE_PCI_1_ECAM_GPA,
                                                (const uint8_t *)(uintptr_t)info.guest_rip) == 0) {
                if (!mmio_mapped && hype_pci_memory_space_enabled(&g_m5_1_pci, HYPE_M5_1_VIRTIO_DEV)) {
                    uint64_t bar4 = hype_pci_get_bar_value(&g_m5_1_pci, HYPE_M5_1_VIRTIO_DEV,
                                                            HYPE_M5_1_BAR_INDEX);
                    if (bar4 != 0) {
                        hype_npt_mark_not_present(g_npt_pd, bar4);
                        mmio_mapped_base = bar4;
                        mmio_mapped = 1;
                        hype_debug_print("m5-1: virtio-blk BAR%u (MMIO) enabled at 0x%llx -- NPT-mapping "
                                          "it now\n",
                                          HYPE_M5_1_BAR_INDEX, (unsigned long long)bar4);
                    }
                }
                continue;
            }

            if (mmio_mapped &&
                hype_svm_vcpu_handle_virtio_blk_npf(ctx, &g_m5_1_virtio_blk, g_m5_1_backing,
                                                     sizeof(g_m5_1_backing), mmio_mapped_base) == 0) {
                continue;
            }

            hype_fatal("m5-1: unhandled NPF (qual=0x%llx guest_rip=0x%llx)",
                       (unsigned long long)info.qualification, (unsigned long long)info.guest_rip);
        }

        break;
    }

    if (info.reason != HYPE_SVM_EXITCODE_HLT) {
        hype_fatal("m5-1: test guest did not halt cleanly (reason=0x%llx guest_rip=0x%llx)",
                   (unsigned long long)info.reason, (unsigned long long)info.guest_rip);
    }
    if (!mmio_mapped || mmio_mapped_base != HYPE_M5_1_MMIO_GPA) {
        hype_fatal("m5-1: virtio-blk device's MMIO BAR was never dynamically mapped");
    }

    /* Request 1 (WRITE) round trip: the guest's own pattern must now
     * be in the backing store at HYPE_M5_1_REQ1_SECTOR, and its status
     * byte must read S_OK. */
    {
        uint64_t sector3_offset = HYPE_M5_1_REQ1_SECTOR * HYPE_VIRTIO_BLK_SECTOR_SIZE;
        for (i = 0; i < HYPE_M5_1_DATA_LEN; i++) {
            if (g_m5_1_backing[sector3_offset + i] != (uint8_t)(0xA0u + i)) {
                hype_fatal("m5-1: WRITE request did not persist correctly at byte %llu", i);
            }
        }
    }
    if (g_m5_1_req1_status[0] != HYPE_VIRTIO_BLK_S_OK) {
        hype_fatal("m5-1: WRITE request's own status byte is not S_OK (got 0x%x)",
                   g_m5_1_req1_status[0]);
    }

    /* Request 2 (READ) round trip: the host's own pre-placed pattern
     * at HYPE_M5_1_REQ2_SECTOR must now be in the guest's own data
     * buffer, and its status byte must read S_OK. */
    for (i = 0; i < HYPE_M5_1_DATA_LEN; i++) {
        if (g_m5_1_req2_data[i] != (uint8_t)(0xB0u + i)) {
            hype_fatal("m5-1: READ request did not deliver the backing store's own data at byte %llu", i);
        }
    }
    if (g_m5_1_req2_status[0] != HYPE_VIRTIO_BLK_S_OK) {
        hype_fatal("m5-1: READ request's own status byte is not S_OK (got 0x%x)", g_m5_1_req2_status[0]);
    }

    hype_debug_print(
        "m5-1: test guest halted cleanly (reason=0x%llx, guest_rip=0x%llx) -- virtio-blk discovered "
        "via PCI BAR%u=0x%llx, feature negotiation + queue setup succeeded, WRITE and READ requests "
        "both round-tripped correctly through the backing store\n",
        (unsigned long long)info.reason, (unsigned long long)info.guest_rip, HYPE_M5_1_BAR_INDEX,
        (unsigned long long)mmio_mapped_base);
}

/*
 * M5-2: a plain ATA hard-disk device (devices/ata_disk.h) behind its
 * own, second AHCI HBA instance -- genuine ATA commands (IDENTIFY
 * DEVICE, READ/WRITE DMA EXT) via a Register H2D FIS, no ATAPI/SCSI-
 * CDB indirection at all (M4-5's own, entirely separate optical
 * drive). A second, independent hype_ahci_t/PCI function rather than
 * a second port on the existing single-port model -- see devices/
 * ata_disk.h's own top comment for why.
 *
 * Reuses M4-5/PCI-2's own exact PCI-discovery-then-port-bring-up
 * payload shape (GHC.AE, CLB/CLBU, FB/FBU, PxCMD ST|FRE, PxCI
 * trigger+poll) verbatim for its own first command, then demonstrates
 * that a single AHCI port can issue a SEQUENCE of distinct ATA
 * commands (this project's own single-command-slot scope, one at a
 * time, not concurrently) by patching the same Command Table's own
 * FIS command/LBA bytes and PRDT entry in place between commands --
 * ordinary (non-intercepted) guest-RAM writes, no different from
 * VIDEO-3/M5-1's own direct buffer writes, since the Command
 * Table/List/PRDT never NPT-trap (only the AHCI MMIO BAR itself
 * does). Three commands in one run: IDENTIFY DEVICE (proving command
 * dispatch + PRDT streaming for a no-LBA command), WRITE DMA EXT
 * (guest-supplied pattern -> backing store at one sector), READ DMA
 * EXT (backing store, pre-filled by the host at a different sector,
 * -> a guest destination buffer) -- exercising both data directions
 * the same way M5-1's own virtio-blk test does.
 */
static uint8_t g_m5_2_cmd_list[1024] __attribute__((aligned(4096)));
static uint8_t g_m5_2_cmd_table[4096] __attribute__((aligned(4096)));
static uint8_t g_m5_2_rx_fis[4096] __attribute__((aligned(4096)));
static uint8_t g_m5_2_guest_code[512] __attribute__((aligned(4096)));
static uint8_t g_m5_2_guest_stack[4096] __attribute__((aligned(4096)));
static uint8_t g_m5_2_media[0x10000] __attribute__((aligned(4096))); /* 128 sectors */
static uint8_t g_m5_2_identify_result[512] __attribute__((aligned(4096)));
static uint8_t g_m5_2_write_data[16] __attribute__((aligned(4096)));
static uint8_t g_m5_2_read_dest[16] __attribute__((aligned(4096)));
static hype_pci_t g_m5_2_pci;
static hype_ahci_t g_m5_2_ahci;
static hype_ata_disk_t g_m5_2_disk;

#define HYPE_M5_2_ATA_DISK_DEV 1u
/* Same "arbitrary offset from an existing constant, always NPT-
 * trapped so real backing doesn't matter" scheme M4-5/PCI-2/VIDEO-3/
 * M5-1 all already established. */
#define HYPE_M5_2_AHCI_GPA (HYPE_M4_3_PFLASH_GPA + 4ULL * 1024 * 1024)
#define HYPE_M5_2_WRITE_SECTOR 5ull
#define HYPE_M5_2_READ_SECTOR 20ull
#define HYPE_M5_2_DATA_LEN 16u

/*
 * Section A/B (RBX = ECAM base, then RBX = AHCI addr): PCI discovery +
 * port bring-up, byte-for-byte identical to g_pci_2_payload_template's
 * own AHCI setup sequence -- triggers the host-pre-built command 1
 * (IDENTIFY DEVICE, already sitting in slot 0) and polls for it.
 * Section C (RBX = Command Table addr): patches command byte + LBA/
 * device fields + PRDT DBA/DBC in place for command 2 (WRITE DMA EXT),
 * then (RBX = AHCI addr again) triggers + polls it.
 * Section D: same patch-then-trigger-then-poll shape for command 3
 * (READ DMA EXT).
 *
 *   [-- identical to g_pci_2_payload_template through its own poll --]
 *   mov rbx, <patched: Command Table addr>       48 BB 00*8
 *   mov al, 0x35 (WRITE DMA EXT)                  B0 35
 *   mov [rbx+2], al                                88 83 02 00 00 00
 *   mov eax, 0x40000005 (device=0x40, LBA=5)       B8 05 00 00 40
 *   mov [rbx+4], eax                               89 83 04 00 00 00
 *   mov eax, <patched: write_data addr>            B8 00*4
 *   mov [rbx+0x80], eax  (PRDT DBA)                89 83 80 00 00 00
 *   mov eax, 15  (PRDT DBC = len-1)                B8 0F 00 00 00
 *   mov [rbx+0x8C], eax                            89 83 8C 00 00 00
 *   mov rbx, <patched: AHCI addr>                  48 BB 00*8
 *   mov eax, 1                                     B8 01 00 00 00
 *   mov [rbx+0x138], eax  (PxCI trigger)           89 83 38 01 00 00
 * poll2:
 *   mov eax, [rbx+0x138]                           8B 83 38 01 00 00
 *   test eax, eax                                   85 C0
 *   jnz poll2                                        75 F6
 *   mov rbx, <patched: Command Table addr>         48 BB 00*8
 *   mov al, 0x25 (READ DMA EXT)                     B0 25
 *   mov [rbx+2], al                                 88 83 02 00 00 00
 *   mov eax, 0x40000014 (device=0x40, LBA=20)       B8 14 00 00 40
 *   mov [rbx+4], eax                                89 83 04 00 00 00
 *   mov eax, <patched: read_dest addr>              B8 00*4
 *   mov [rbx+0x80], eax                              89 83 80 00 00 00
 *   mov eax, 15                                      B8 0F 00 00 00
 *   mov [rbx+0x8C], eax                              89 83 8C 00 00 00
 *   mov rbx, <patched: AHCI addr>                    48 BB 00*8
 *   mov eax, 1                                       B8 01 00 00 00
 *   mov [rbx+0x138], eax                             89 83 38 01 00 00
 * poll3:
 *   mov eax, [rbx+0x138]                             8B 83 38 01 00 00
 *   test eax, eax                                     85 C0
 *   jnz poll3                                          75 F6
 *   hlt                                                F4
 *   jmp $-3                                            EB FD
 */
static const uint8_t g_m5_2_payload_template[] = {
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x24, 0x80, 0x00, 0x00,
    0xB8, 0x02, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x04, 0x80, 0x00, 0x00,
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
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB0, 0x35,
    0x88, 0x83, 0x02, 0x00, 0x00, 0x00,
    0xB8, 0x05, 0x00, 0x00, 0x40,
    0x89, 0x83, 0x04, 0x00, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x80, 0x00, 0x00, 0x00,
    0xB8, 0x0F, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x8C, 0x00, 0x00, 0x00,
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB8, 0x01, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x38, 0x01, 0x00, 0x00,
    0x8B, 0x83, 0x38, 0x01, 0x00, 0x00,
    0x85, 0xC0,
    0x75, 0xF6,
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB0, 0x25,
    0x88, 0x83, 0x02, 0x00, 0x00, 0x00,
    0xB8, 0x14, 0x00, 0x00, 0x40,
    0x89, 0x83, 0x04, 0x00, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x80, 0x00, 0x00, 0x00,
    0xB8, 0x0F, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x8C, 0x00, 0x00, 0x00,
    0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB8, 0x01, 0x00, 0x00, 0x00,
    0x89, 0x83, 0x38, 0x01, 0x00, 0x00,
    0x8B, 0x83, 0x38, 0x01, 0x00, 0x00,
    0x85, 0xC0,
    0x75, 0xF6,
    0xF4,
    0xEB, 0xFD
};
#define HYPE_M5_2_PAYLOAD_ECAM_RBX_IMM_OFFSET 2
#define HYPE_M5_2_PAYLOAD_BAR5_VALUE_IMM_OFFSET 11
#define HYPE_M5_2_PAYLOAD_AHCI_RBX_IMM_OFFSET 34
#define HYPE_M5_2_PAYLOAD_CLB_LOW_IMM_OFFSET 51
#define HYPE_M5_2_PAYLOAD_CLB_HIGH_IMM_OFFSET 62
#define HYPE_M5_2_PAYLOAD_FB_LOW_IMM_OFFSET 73
#define HYPE_M5_2_PAYLOAD_FB_HIGH_IMM_OFFSET 84
#define HYPE_M5_2_PAYLOAD_TABLE_RBX_1_IMM_OFFSET 128
#define HYPE_M5_2_PAYLOAD_WRITE_DATA_ADDR_IMM_OFFSET 156
#define HYPE_M5_2_PAYLOAD_AHCI_RBX_2_IMM_OFFSET 179
#define HYPE_M5_2_PAYLOAD_TABLE_RBX_2_IMM_OFFSET 210
#define HYPE_M5_2_PAYLOAD_READ_DEST_ADDR_IMM_OFFSET 238
#define HYPE_M5_2_PAYLOAD_AHCI_RBX_3_IMM_OFFSET 261

static void run_m5_2_test(const hype_vmm_ops_t *ops, hype_vmm_kind_t kind) {
    unsigned long long i;
    uint64_t entry_rip, guest_cr3, rsp, npt_root_phys;
    uint64_t cmd_list_phys, cmd_table_phys, rx_fis_phys;
    uint64_t identify_phys, write_data_phys, read_dest_phys;
    hype_vcpu_ctx_t *ctx;
    hype_vmexit_info_t info;
    int ahci_mapped;
    uint64_t ahci_mapped_base;
    uint64_t write_sector_offset, read_sector_offset;

    if (kind != HYPE_VMM_KIND_SVM) {
        hype_serial_print("m5-2: skipped -- %s has no working vcpu_run yet (see vmx_ops.c)\n", ops->name);
        return;
    }

    hype_guest_ram_zero(g_m5_2_cmd_list, sizeof(g_m5_2_cmd_list));
    hype_guest_ram_zero(g_m5_2_cmd_table, sizeof(g_m5_2_cmd_table));
    hype_guest_ram_zero(g_m5_2_rx_fis, sizeof(g_m5_2_rx_fis));
    hype_guest_ram_zero(g_m5_2_guest_code, sizeof(g_m5_2_guest_code));
    hype_guest_ram_zero(g_m5_2_guest_stack, sizeof(g_m5_2_guest_stack));
    hype_guest_ram_zero(g_m5_2_media, sizeof(g_m5_2_media));
    hype_guest_ram_zero(g_m5_2_identify_result, sizeof(g_m5_2_identify_result));
    hype_guest_ram_zero(g_m5_2_write_data, sizeof(g_m5_2_write_data));
    hype_guest_ram_zero(g_m5_2_read_dest, sizeof(g_m5_2_read_dest));

    for (i = 0; i < HYPE_M5_2_DATA_LEN; i++) {
        g_m5_2_write_data[i] = (uint8_t)(0xC0u + i);
    }
    read_sector_offset = HYPE_M5_2_READ_SECTOR * HYPE_ATA_SECTOR_SIZE;
    for (i = 0; i < HYPE_M5_2_DATA_LEN; i++) {
        g_m5_2_media[read_sector_offset + i] = (uint8_t)(0xD0u + i);
    }

    hype_ata_disk_reset(&g_m5_2_disk, g_m5_2_media, sizeof(g_m5_2_media));
    hype_ahci_reset(&g_m5_2_ahci);
    hype_pci_reset(&g_m5_2_pci);
    hype_pci_add_device(&g_m5_2_pci, HYPE_M5_2_ATA_DISK_DEV, HYPE_PCI_VENDOR_ID_HYPE, 0x0004u, 0x01, 0x06,
                         0x01);
    hype_pci_set_bar_size(&g_m5_2_pci, HYPE_M5_2_ATA_DISK_DEV, 5, 0x1000u);

    cmd_list_phys = (uint64_t)(uintptr_t)g_m5_2_cmd_list;
    cmd_table_phys = (uint64_t)(uintptr_t)g_m5_2_cmd_table;
    rx_fis_phys = (uint64_t)(uintptr_t)g_m5_2_rx_fis;
    identify_phys = (uint64_t)(uintptr_t)g_m5_2_identify_result;
    write_data_phys = (uint64_t)(uintptr_t)g_m5_2_write_data;
    read_dest_phys = (uint64_t)(uintptr_t)g_m5_2_read_dest;

    /* Command Header slot 0: CFL=5 (unused by this project's own ATA
     * dispatch), prdtl=1 -- stays fixed across all 3 commands, only
     * the Command Table's own FIS/PRDT bytes change between them. */
    hype_write_le32(g_m5_2_cmd_list + 0, 0x00010005u);
    hype_write_le32(g_m5_2_cmd_list + 4, 0);
    hype_write_le32(g_m5_2_cmd_list + 8, (uint32_t)cmd_table_phys);
    hype_write_le32(g_m5_2_cmd_list + 12, (uint32_t)(cmd_table_phys >> 32));

    /* Command Table: H2D Register FIS pre-built for command 1
     * (IDENTIFY DEVICE) -- the guest's own asm triggers this one
     * as-is, then patches command/LBA/PRDT bytes in place for
     * commands 2 and 3. */
    g_m5_2_cmd_table[0] = 0x27;
    g_m5_2_cmd_table[1] = 0x80;
    g_m5_2_cmd_table[2] = HYPE_ATA_CMD_IDENTIFY_DEVICE;
    /* Count field (bytes 12-13): IDENTIFY ignores it, but WRITE/READ
     * DMA EXT (commands 2/3) both want exactly 1 sector -- the guest's
     * own asm never patches these two bytes between commands (nothing
     * else needs to change here), so setting it once, here, is enough
     * for all 3 commands. Left at 0 this would resolve to 65536 via
     * the real "0 means max" convention, failing WRITE/READ's own
     * bounds check against this test's 128-sector disk. */
    g_m5_2_cmd_table[12] = 1;
    g_m5_2_cmd_table[13] = 0;
    hype_write_le32(g_m5_2_cmd_table + 0x80 + 0, (uint32_t)identify_phys);
    hype_write_le32(g_m5_2_cmd_table + 0x80 + 4, (uint32_t)(identify_phys >> 32));
    hype_write_le32(g_m5_2_cmd_table + 0x80 + 12, HYPE_ATA_IDENTIFY_SIZE - 1u);

    for (i = 0; i < sizeof(g_m5_2_payload_template); i++) {
        g_m5_2_guest_code[i] = g_m5_2_payload_template[i];
    }
    hype_write_le64(g_m5_2_guest_code + HYPE_M5_2_PAYLOAD_ECAM_RBX_IMM_OFFSET, HYPE_PCI_1_ECAM_GPA);
    hype_write_le32(g_m5_2_guest_code + HYPE_M5_2_PAYLOAD_BAR5_VALUE_IMM_OFFSET,
                     (uint32_t)HYPE_M5_2_AHCI_GPA);
    hype_write_le64(g_m5_2_guest_code + HYPE_M5_2_PAYLOAD_AHCI_RBX_IMM_OFFSET, HYPE_M5_2_AHCI_GPA);
    hype_write_le32(g_m5_2_guest_code + HYPE_M5_2_PAYLOAD_CLB_LOW_IMM_OFFSET, (uint32_t)cmd_list_phys);
    hype_write_le32(g_m5_2_guest_code + HYPE_M5_2_PAYLOAD_CLB_HIGH_IMM_OFFSET,
                     (uint32_t)(cmd_list_phys >> 32));
    hype_write_le32(g_m5_2_guest_code + HYPE_M5_2_PAYLOAD_FB_LOW_IMM_OFFSET, (uint32_t)rx_fis_phys);
    hype_write_le32(g_m5_2_guest_code + HYPE_M5_2_PAYLOAD_FB_HIGH_IMM_OFFSET,
                     (uint32_t)(rx_fis_phys >> 32));
    hype_write_le64(g_m5_2_guest_code + HYPE_M5_2_PAYLOAD_TABLE_RBX_1_IMM_OFFSET, cmd_table_phys);
    hype_write_le32(g_m5_2_guest_code + HYPE_M5_2_PAYLOAD_WRITE_DATA_ADDR_IMM_OFFSET,
                     (uint32_t)write_data_phys);
    hype_write_le64(g_m5_2_guest_code + HYPE_M5_2_PAYLOAD_AHCI_RBX_2_IMM_OFFSET, HYPE_M5_2_AHCI_GPA);
    hype_write_le64(g_m5_2_guest_code + HYPE_M5_2_PAYLOAD_TABLE_RBX_2_IMM_OFFSET, cmd_table_phys);
    hype_write_le32(g_m5_2_guest_code + HYPE_M5_2_PAYLOAD_READ_DEST_ADDR_IMM_OFFSET,
                     (uint32_t)read_dest_phys);
    hype_write_le64(g_m5_2_guest_code + HYPE_M5_2_PAYLOAD_AHCI_RBX_3_IMM_OFFSET, HYPE_M5_2_AHCI_GPA);

    entry_rip = (uint64_t)(uintptr_t)g_m5_2_guest_code;
    rsp = (uint64_t)(uintptr_t)(g_m5_2_guest_stack + sizeof(g_m5_2_guest_stack));

    hype_paging_build_identity(g_guest_pml4, g_guest_pdpt, g_guest_pd, HYPE_M3_5_GUEST_PAGING_GB);
    guest_cr3 = (uint64_t)(uintptr_t)g_guest_pml4;

    hype_npt_build_identity(g_npt_pml4, g_npt_pdpt, g_npt_pd, HYPE_NPT_MAX_GB);
    hype_npt_mark_not_present(g_npt_pd, HYPE_PCI_1_ECAM_GPA);
    npt_root_phys = (uint64_t)(uintptr_t)g_npt_pml4;

    hype_debug_print("m5-2: entry_rip=0x%llx ecam_gpa=0x%llx chosen_ahci_gpa=0x%llx\n",
                      (unsigned long long)entry_rip, (unsigned long long)HYPE_PCI_1_ECAM_GPA,
                      (unsigned long long)HYPE_M5_2_AHCI_GPA);

    ctx = hype_svm_vcpu_create_long_mode(entry_rip, guest_cr3, rsp, npt_root_phys);
    if (ctx == 0) {
        hype_fatal("m5-2: vcpu_create_long_mode failed");
    }

    ahci_mapped = 0;
    ahci_mapped_base = 0;

    for (;;) {
        if (ops->vcpu_run(ctx, &info) != 0) {
            hype_fatal("m5-2: VM-entry failed (reason=0x%llx)", (unsigned long long)info.reason);
        }

        if (info.reason == HYPE_SVM_EXITCODE_NPF) {
            if (hype_svm_vcpu_handle_pci_ecam_npf(ctx, &g_m5_2_pci, HYPE_PCI_1_ECAM_GPA,
                                                (const uint8_t *)(uintptr_t)info.guest_rip) == 0) {
                if (!ahci_mapped &&
                    hype_pci_memory_space_enabled(&g_m5_2_pci, HYPE_M5_2_ATA_DISK_DEV)) {
                    uint64_t bar5 = hype_pci_get_bar_value(&g_m5_2_pci, HYPE_M5_2_ATA_DISK_DEV, 5);
                    if (bar5 != 0) {
                        hype_npt_mark_not_present(g_npt_pd, bar5);
                        ahci_mapped_base = bar5;
                        ahci_mapped = 1;
                        hype_debug_print("m5-2: ATA disk BAR5 enabled at 0x%llx -- NPT-mapping it now\n",
                                          (unsigned long long)bar5);
                    }
                }
                continue;
            }

            if (ahci_mapped &&
                hype_svm_vcpu_handle_ahci_disk_npf(ctx, &g_m5_2_ahci, &g_m5_2_disk, ahci_mapped_base) ==
                    0) {
                continue;
            }

            hype_fatal("m5-2: unhandled NPF (qual=0x%llx guest_rip=0x%llx)",
                       (unsigned long long)info.qualification, (unsigned long long)info.guest_rip);
        }

        break;
    }

    if (info.reason != HYPE_SVM_EXITCODE_HLT) {
        hype_fatal("m5-2: test guest did not halt cleanly (reason=0x%llx guest_rip=0x%llx)",
                   (unsigned long long)info.reason, (unsigned long long)info.guest_rip);
    }
    if (!ahci_mapped || ahci_mapped_base != HYPE_M5_2_AHCI_GPA) {
        hype_fatal("m5-2: ATA disk device was never dynamically mapped at the chosen BAR5 address");
    }

    /* IDENTIFY DEVICE round trip. Word 0's bit 15 lives in its own
     * HIGH byte (byte 1 of the little-endian word), not byte 0. */
    if ((g_m5_2_identify_result[1] & 0x80u) != 0) {
        hype_fatal("m5-2: IDENTIFY DEVICE word 0 bit 15 (ATAPI) is set on what should be an ATA device");
    }
    if ((g_m5_2_identify_result[99] & 0x02u) == 0) {
        hype_fatal("m5-2: IDENTIFY DEVICE word 49 does not report LBA support");
    }
    if ((g_m5_2_identify_result[167] & 0x04u) == 0) {
        hype_fatal("m5-2: IDENTIFY DEVICE word 83 does not report LBA48 support");
    }

    /* WRITE DMA EXT round trip. */
    write_sector_offset = HYPE_M5_2_WRITE_SECTOR * HYPE_ATA_SECTOR_SIZE;
    for (i = 0; i < HYPE_M5_2_DATA_LEN; i++) {
        if (g_m5_2_media[write_sector_offset + i] != (uint8_t)(0xC0u + i)) {
            hype_fatal("m5-2: WRITE DMA EXT did not persist correctly at byte %llu", i);
        }
    }

    /* READ DMA EXT round trip. */
    for (i = 0; i < HYPE_M5_2_DATA_LEN; i++) {
        if (g_m5_2_read_dest[i] != (uint8_t)(0xD0u + i)) {
            hype_fatal("m5-2: READ DMA EXT did not deliver the backing store's own data at byte %llu", i);
        }
    }

    hype_debug_print(
        "m5-2: test guest halted cleanly (reason=0x%llx, guest_rip=0x%llx) -- ATA disk discovered via "
        "PCI BAR5=0x%llx, IDENTIFY DEVICE + WRITE DMA EXT + READ DMA EXT all round-tripped correctly\n",
        (unsigned long long)info.reason, (unsigned long long)info.guest_rip,
        (unsigned long long)ahci_mapped_base);
}

/*
 * FW-1: the first attempt at launching this project's own real,
 * unmodified vendored OVMF (M4-2) -- not a hand-written synthetic test
 * guest. Real-mode entry at the classic x86 reset vector, using the
 * genuine hardware reset convention (CS.base=0xFFFF0000, RIP=0xFFF0,
 * linear=0xFFFFFFF0) rather than folding the whole address into
 * CS.base with RIP=0 the way every prior synthetic test guest does --
 * real firmware's own ResetVector code depends on RIP starting near
 * the top of its 16-bit range (see hype_svm_vcpu_set_rip()'s comment).
 * Reuses hype_svm_vcpu_create()'s existing "CS.base is whatever the
 * hypervisor sets directly" convention (M2-7's own, already-proven
 * mechanism) rather than a new VMCB builder -- the guest never inspects
 * its own CS *selector* value, only the linear address it lands at,
 * which is correct here regardless.
 *
 * This is fundamentally exploratory: nothing in this project has ever
 * run real third-party firmware before, only fully-controlled
 * hand-written payloads. The dispatch loop below handles exactly the
 * VM-exits already built for that reason (CPUID, MSR) and lets
 * anything else fall through to a fatal with full diagnostic info
 * (reason/qualification/guest_rip) -- that fatal message is the actual
 * point of this first attempt: whatever it reports is what needs
 * building next, iterated the same log-driven way every other
 * real-hardware-facing piece of this project has been.
 */

/*
 * FW-1b: translate a guest-physical address to the host address that
 * backs it under FW-1's (non-identity) NPT map, or 0 if unmapped. Used
 * to fetch the faulting instruction's bytes for MMIO decode: unlike the
 * fully-identity-mapped test guests, FW-1 remaps both guest RAM and the
 * flash window, so save.rip cannot be dereferenced as a host pointer
 * directly. (FW-1's guest paging is identity, so a guest-linear RIP
 * equals its guest-physical address here.) Mirrors exactly the two
 * hype_npt_map_range() calls in the NPT build below. */
static const uint8_t *fw_1_guest_phys_to_host(uint64_t gpa) {
    uint64_t flash_base = 0x100000000ULL - g_fw_1_combined_size;
    if (gpa < HYPE_FW_1_GUEST_RAM_BYTES) {
        return (const uint8_t *)(uintptr_t)(g_fw_1_ram_host_phys + gpa);
    }
    if (gpa >= flash_base && gpa < 0x100000000ULL) {
        return (const uint8_t *)(uintptr_t)(g_fw_1_combined_host_phys + (gpa - flash_base));
    }
    return 0;
}

/* Reads one 64-bit guest page-table entry at guest-physical `gpa`, or
 * returns 0 (a not-present entry) if it isn't backed. */
static uint64_t fw_1_read_guest_pte(uint64_t gpa) {
    const uint8_t *p = fw_1_guest_phys_to_host(gpa & ~0xFFFULL);
    if (p == 0) {
        return 0;
    }
    p += (uint32_t)(gpa & 0xFFFULL);
    return (uint64_t)hype_read_le32(p) | ((uint64_t)hype_read_le32(p + 4) << 32);
}

/* Walks the guest's own 4-level long-mode page tables (rooted at CR3) to
 * translate a guest-VIRTUAL address to guest-physical. Needed to fetch
 * the faulting instruction's bytes for MMIO decode once the guest runs
 * in its own virtual address space (a Linux kernel's RIP is a high-
 * canonical virtual address, unlike OVMF's identity-mapped RIP): AMD's
 * decode-assist capture is the preferred source, but QEMU+KVM nested SVM
 * does not populate it, so this is the fallback there. Honors 1 GiB and
 * 2 MiB large pages (PS bit); returns -1 on any not-present level.
 * Assumes 4-level paging (this project never advertises LA57 to guests).
 * The PHYSMASK matches x86-64's 52-bit max physical address. */
static int fw_1_guest_virt_to_phys(uint64_t cr3, uint64_t gva, uint64_t *out_gpa) {
    const uint64_t PHYS = 0x000FFFFFFFFFF000ULL; /* bits 51:12 */
    uint64_t e;

    e = fw_1_read_guest_pte((cr3 & PHYS) + (((gva >> 39) & 0x1FFULL) * 8ULL)); /* PML4E */
    if ((e & 1ULL) == 0) {
        return -1;
    }
    e = fw_1_read_guest_pte((e & PHYS) + (((gva >> 30) & 0x1FFULL) * 8ULL)); /* PDPTE */
    if ((e & 1ULL) == 0) {
        return -1;
    }
    if (e & (1ULL << 7)) { /* 1 GiB page */
        *out_gpa = (e & 0x000FFFFFC0000000ULL) | (gva & 0x3FFFFFFFULL);
        return 0;
    }
    e = fw_1_read_guest_pte((e & PHYS) + (((gva >> 21) & 0x1FFULL) * 8ULL)); /* PDE */
    if ((e & 1ULL) == 0) {
        return -1;
    }
    if (e & (1ULL << 7)) { /* 2 MiB page */
        *out_gpa = (e & 0x000FFFFFFFE00000ULL) | (gva & 0x1FFFFFULL);
        return 0;
    }
    e = fw_1_read_guest_pte((e & PHYS) + (((gva >> 12) & 0x1FFULL) * 8ULL)); /* PTE */
    if ((e & 1ULL) == 0) {
        return -1;
    }
    *out_gpa = (e & PHYS) | (gva & 0xFFFULL);
    return 0;
}

/* Best-effort host pointer to the faulting instruction bytes for a guest
 * whose RIP may be virtual: try a guest page-table walk (CR3), then fall
 * back to treating RIP as guest-physical (correct for an identity-paged
 * guest such as OVMF). Used only when decode assists gave nothing. */
static const uint8_t *fw_1_insn_bytes_via_ptwalk(hype_vcpu_ctx_t *ctx, uint64_t guest_rip) {
    uint64_t gpa = 0;
    if (fw_1_guest_virt_to_phys(hype_svm_vcpu_get_cr3(ctx), guest_rip, &gpa) == 0) {
        const uint8_t *p = fw_1_guest_phys_to_host(gpa);
        if (p != 0) {
            return p;
        }
    }
    return fw_1_guest_phys_to_host(guest_rip);
}

/* FW-1e: drain the guest UART's transmit ring, strip terminal escape
 * sequences (hype's GOP console can't interpret them), and emit the
 * guest's console output to hype's own console (serial + GOP) one line
 * at a time -- one GOP Blt per line rather than per byte. `filter`,
 * `line` and `line_len` persist across calls (owned by the caller). */
static unsigned int fw_1_drain_uart_console(hype_guest_uart_t *uart, hype_vt_filter_t *filter, char *line,
                                             unsigned int *line_len, unsigned int line_cap) {
    unsigned int emitted = 0;
    uint8_t b;
    while (hype_guest_uart_tx_dequeue(uart, &b)) {
        char c;
        if (!hype_vt_filter(filter, b, &c)) {
            continue;
        }
        emitted++;
        if (c == '\n') {
            line[*line_len] = '\0';
            hype_debug_print("%s\n", line);
            *line_len = 0;
            continue;
        }
        if (*line_len >= line_cap - 1) {
            line[*line_len] = '\0';
            hype_debug_print("%s\n", line);
            *line_len = 0;
        }
        line[(*line_len)++] = c;
    }
    return emitted;
}

/* FW-1g: feed one byte of the guest's OVMF debug-io-port (0x402) log to
 * hype's console, a line at a time, tagged so it's distinguishable from
 * the guest's ConOut console and hype's own output. Debug text is plain
 * ASCII, but reuse the VT filter for safety. */
static void fw_1_debug_feed(hype_vt_filter_t *filter, char *line, unsigned int *line_len,
                             unsigned int line_cap, uint8_t byte) {
    char c;
    if (!hype_vt_filter(filter, byte, &c)) {
        return;
    }
    if (c == '\n') {
        line[*line_len] = '\0';
        hype_debug_print("fw-1 ovmf-dbg| %s\n", line);
        *line_len = 0;
        return;
    }
    if (*line_len >= line_cap - 1) {
        line[*line_len] = '\0';
        hype_debug_print("fw-1 ovmf-dbg| %s\n", line);
        *line_len = 0;
    }
    line[(*line_len)++] = c;
}

/* Real-hardware, serial-less logging (core/logbuf.h + core/file_io.h).
 * The boot volume root is located ONCE on the BSP (fw_1_log_init, before
 * the guest is dispatched -- possibly to a pinned AP), so a flush from
 * the FW-1 loop or from hype_fatal() only re-opens+writes the file rather
 * than doing HandleProtocol from a non-BSP context. Fully best-effort:
 * the first write that errors (a read-only or non-FAT volume) disables
 * further attempts so it can never wedge the boot. */
static EFI_FILE_PROTOCOL *g_fw_1_log_root = 0;
static int g_fw_1_log_disabled = 0;

static void fw_1_flush_log(void) {
    if (g_fw_1_log_disabled || g_fw_1_log_root == 0) {
        return;
    }
    if (hype_file_write_new(g_fw_1_log_root, (CHAR16 *)L"\\hype-log.txt", hype_logbuf_data(),
                             (UINTN)hype_logbuf_len()) != EFI_SUCCESS) {
        g_fw_1_log_disabled = 1;
    }
}

/* Called once on the BSP (Boot Services file I/O proven -- the ISO was
 * just read from this volume) to cache the root + register the crash-time
 * flush hook. */
static void fw_1_log_init(EFI_HANDLE image_handle, EFI_BOOT_SERVICES *bs) {
    if (hype_file_locate_root(image_handle, bs, &g_fw_1_log_root) != EFI_SUCCESS) {
        g_fw_1_log_root = 0;
        g_fw_1_log_disabled = 1;
        return;
    }
    /* Clear any stale log from a previous run ONCE, here on the BSP, so
     * the subsequent in-place-overwrite flushes never have to delete
     * (which churns fragile FAT write paths). */
    hype_file_delete(g_fw_1_log_root, (CHAR16 *)L"\\hype-log.txt");
    hype_fatal_set_flush_hook(fw_1_flush_log);
}

/* M4-6d4: catch a HOST-side CPU exception during the FW-1 guest loop.
 *
 * The loop runs BEFORE ExitBootServices (so the \hype-log.txt flush works),
 * but the hypervisor's own IDT isn't installed until AFTER the guests return
 * (efi_main, hype_idt_load). So during the loop the firmware's IDT is live,
 * and a HOST-side fault in one of our exit handlers (a bad guest-controlled
 * pointer, say) takes a #PF/#GP the firmware doesn't expect -- on real
 * hardware that silently triple-faults and RESETS the machine, leaving no
 * PANIC in the log (exactly the M4-6d4 real-HW symptom: both a 5950x and a
 * Zen2 laptop reboot at the same "Scanning hardware for mdev" point with no
 * fatal message). This surgically overrides ONLY the 32 architectural
 * exception vectors (0-31) in the live firmware IDT to point at our own ISR
 * stubs -- which decode + hype_fatal(), flushing the log with the faulting
 * vector name and RIP before halting. Hardware IRQ vectors (32-255) are left
 * exactly as firmware set them, so firmware IRQs and the Boot-Services
 * storage the log flush rides on keep working untouched. The gate selector
 * is the CURRENT CS (we're still on the firmware GDT here, not ours). The
 * originals are restored after the loop. This is a strict improvement, not
 * just a diagnostic: a caught, reported host fault beats a silent reset. */
static hype_idt_entry_t g_fw_1_saved_idt_exc[32];
static int g_fw_1_idt_patched;

static void fw_1_install_exception_catcher(void) {
    hype_idt_ptr_t idtr;
    hype_idt_entry_t *live;
    uint16_t cs = 0;
    unsigned v;
    __asm__ volatile("sidt %0" : "=m"(idtr));
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    if (idtr.base == 0 || idtr.limit < (uint16_t)(32u * sizeof(hype_idt_entry_t) - 1u)) {
        hype_debug_print("fw-1: exception catcher NOT armed (firmware IDT base=0x%llx limit=%u too small)\n",
                          (unsigned long long)idtr.base, (unsigned int)idtr.limit);
        return;
    }
    live = (hype_idt_entry_t *)(uintptr_t)idtr.base;
    for (v = 0; v < 32u; v++) {
        g_fw_1_saved_idt_exc[v] = live[v];
        hype_idt_encode_entry(&live[v], hype_isr_stub_table[v], cs, 0, HYPE_IDT_TYPE_INTERRUPT_GATE);
    }
    g_fw_1_idt_patched = 1;
    hype_debug_print("fw-1: host exception catcher armed (IDT base=0x%llx cs=0x%x) -- a host fault "
                      "in the guest loop now flushes a PANIC instead of resetting the machine\n",
                      (unsigned long long)idtr.base, (unsigned int)cs);
}

static void fw_1_remove_exception_catcher(void) {
    hype_idt_ptr_t idtr;
    hype_idt_entry_t *live;
    unsigned v;
    if (!g_fw_1_idt_patched) {
        return;
    }
    __asm__ volatile("sidt %0" : "=m"(idtr));
    live = (hype_idt_entry_t *)(uintptr_t)idtr.base;
    for (v = 0; v < 32u; v++) {
        live[v] = g_fw_1_saved_idt_exc[v];
    }
    g_fw_1_idt_patched = 0;
}

static void run_fw_1_test(const hype_vmm_ops_t *ops, hype_vmm_kind_t kind) {
    uint64_t reset_cs_base, reset_rip, stack_top, npt_root_phys;
    hype_vcpu_ctx_t *ctx;
    hype_vmexit_info_t info;

    if (kind != HYPE_VMM_KIND_SVM) {
        hype_serial_print("fw-1: skipped -- %s has no working vcpu_run yet (see vmx_ops.c)\n", ops->name);
        return;
    }

    hype_guest_ram_zero(g_fw_1_guest_stack, sizeof(g_fw_1_guest_stack));
    hype_pic_emu_reset(&g_fw_1_pic);
    hype_pit_emu_reset(&g_fw_1_pit);
    hype_guest_lapic_reset(&g_fw_1_lapic);
    hype_guest_uart_reset(&g_fw_1_uart);
    hype_guest_uart_reset(&g_fw_1_uart2);
    hype_ps2_kbd_reset(&g_fw_1_ps2);
    hype_ps2_mouse_reset(&g_fw_1_mouse);
    hype_pci_reset(&g_fw_1_pci);
    hype_pci_add_device(&g_fw_1_pci, 0, HYPE_FW_1_PCI_VENDOR_ID_INTEL, HYPE_FW_1_PCI_DEVICE_ID_Q35_MCH, 0x06,
                         0x00, 0x00);
    hype_pci_add_device(&g_fw_1_pci, HYPE_FW_1_PCI_DEV_ICH9_LPC, HYPE_FW_1_PCI_VENDOR_ID_INTEL,
                         HYPE_FW_1_PCI_DEVICE_ID_ICH9_LPC, 0x06, 0x01, 0x00);
    /* FW-1h: AHCI SATA controller (class 0x01 / subclass 0x06 / prog-IF
     * 0x01 -- "AHCI 1.0"), with a 4KB BAR5 (ABAR) for OVMF to size and
     * place, exactly as run_pci_2_test registers it. Backed by ISO-1's
     * real loaded ISO so OVMF's BDS finds a bootable CD. */
    hype_pci_add_device(&g_fw_1_pci, HYPE_FW_1_PCI_DEV_AHCI, HYPE_PCI_VENDOR_ID_HYPE, 0x0005u, 0x01, 0x06,
                         0x01);
    hype_pci_set_bar_size(&g_fw_1_pci, HYPE_FW_1_PCI_DEV_AHCI, 5, 0x1000u);
    /* M4-6d2: advertise a legacy PCI interrupt on the AHCI function
     * (Interrupt Pin INTA=1) so the guest treats it as interrupt-capable
     * and libata uses its interrupt-driven path rather than the forced-
     * polling one (which WARNs -- ata_host_activate WARN_ON(irq_handler)
     * -- and stalls the probe, confirmed on real AMD). Line 11 is just an
     * initial value; OVMF reprograms Interrupt Line (config 0x3C) via its
     * Q35 routing and the guest reads that back, so the vCPU loop
     * delivers the completion IRQ on whatever line 0x3C actually holds
     * (hype_pci_get_interrupt_line), master or slave. */
    hype_pci_set_interrupt(&g_fw_1_pci, HYPE_FW_1_PCI_DEV_AHCI, 1, 11);
    hype_ahci_reset(&g_fw_1_ahci);
    hype_atapi_reset(&g_fw_1_atapi, (uint8_t *)(uintptr_t)g_iso_host_phys, (uint32_t)g_iso_size);
    /* FW-1h: per-command AHCI/ATAPI tracing is available for debugging
     * the CD-ROM discovery sequence -- hype_svm_set_ahci_trace(1) -- but
     * left off here: a real boot issues thousands of commands and each
     * trace line is GOP-rendered. */
    /* M4-6: for the real-OS guest, an MSR the allow-list doesn't
     * recognize is logged and safely stubbed (RDMSR -> 0, WRMSR ->
     * ignored) instead of being fatal. This stays isolation-safe -- it
     * never reads or writes a real host MSR -- while letting a real
     * kernel, which touches many benign MSRs (microcode rev, TSC_AUX,
     * ...), boot past them; the log shows exactly which it wanted. The
     * synthetic milestone test guests keep the fail-closed default (this
     * flag is per-run and only FW-1 sets it). VMSAVE/VMLOAD-managed and
     * VMCB-backed MSRs (FS/GS/syscall/sysenter, PAT, EFER) are handled
     * for real, not stubbed -- see configure_guest_msrpm(). */
    hype_svm_set_msr_trace(1);
    /* FW-1g: per-access PS/2 tracing (hype_svm_set_ps2_trace(1)) is
     * available for debugging the keyboard handshake, left off -- an
     * interactive prompt busy-polls 0x64 thousands of times and each
     * trace line is GOP-rendered. */

    /* Real OVMF's own platform init needs real ACPI content via
     * fw_cfg, the same "etc/acpi/rsdp"/"etc/acpi/tables"/
     * "etc/table-loader" mechanism M4-4 already built and proved works
     * against real SVM -- confirmed necessary here too: with no fw_cfg
     * device registered at all, every fw_cfg port access (0x510/0x511)
     * was absorbed as all-1s (visible in the unhandled-port log),
     * giving real OVMF no valid ACPI/RSDP content to find. */
    {
        hype_acpi_layout_t layout;
        hype_acpi_config_t cfg;
        uint32_t loader_entries;
        unsigned int z;

        for (z = 0; z < HYPE_ACPI_MAX_CPUS; z++) {
            cfg.apic_ids[z] = (uint8_t)z;
        }
        cfg.cpu_count = 1;
        cfg.local_apic_address = 0xFEE00000u;
        cfg.io_apic_id = 1;
        cfg.io_apic_address = 0xFEC00000u;
        cfg.io_apic_gsi_base = 0;
        cfg.mcfg_base_address = HYPE_FW_1_ECAM_GPA;
        cfg.pci_segment = 0;
        cfg.pci_start_bus = 0;
        cfg.pci_end_bus = 255;
        cfg.sci_interrupt = 9;

        if (hype_acpi_build_tables_blob(g_fw_1_tables_blob, sizeof(g_fw_1_tables_blob), &cfg, &layout) !=
            0) {
            hype_fatal("fw-1: hype_acpi_build_tables_blob failed");
        }
        hype_acpi_build_rsdp(&g_fw_1_rsdp, layout.xsdt_offset);
        loader_entries = hype_acpi_loader_build_script(g_fw_1_loader_script, &layout);

        hype_fw_cfg_reset(&g_fw_1_fw_cfg);
        if (hype_fw_cfg_add_file(&g_fw_1_fw_cfg, HYPE_ACPI_LOADER_FILE_RSDP, (const uint8_t *)&g_fw_1_rsdp,
                                  sizeof(g_fw_1_rsdp)) < 0) {
            hype_fatal("fw-1: fw_cfg registry full while registering rsdp");
        }
        if (hype_fw_cfg_add_file(&g_fw_1_fw_cfg, HYPE_ACPI_LOADER_FILE_TABLES, g_fw_1_tables_blob,
                                  layout.total_length) < 0) {
            hype_fatal("fw-1: fw_cfg registry full while registering tables");
        }
        if (hype_fw_cfg_add_file(&g_fw_1_fw_cfg, "etc/table-loader", (const uint8_t *)g_fw_1_loader_script,
                                  loader_entries * (uint32_t)sizeof(hype_acpi_loader_entry_t)) < 0) {
            hype_fatal("fw-1: fw_cfg registry full while registering table-loader");
        }

        /* FW-1a: etc/e820 -- the memory map OVMF reads FIRST (before the
         * CMOS fallback below) to size low RAM
         * (PlatformInitLib/MemDetect.c: PlatformScanE820). Declares
         * exactly the RAM this project actually backs with a real host
         * buffer: a single usable region [0, HYPE_FW_1_GUEST_RAM_BYTES),
         * well below the Q35 32-bit MMIO hole. This is what makes OVMF
         * place its DXE stack inside backed RAM instead of just below
         * 4GB in the host's MMIO hole (the jump-to-(-1) root cause). */
        {
            hype_e820_region_t ram_region;
            int e820_len;
            ram_region.base = 0;
            ram_region.length = HYPE_FW_1_GUEST_RAM_BYTES;
            ram_region.type = HYPE_E820_TYPE_RAM;
            e820_len = hype_e820_build(g_fw_1_e820_blob, (uint32_t)sizeof(g_fw_1_e820_blob), &ram_region, 1);
            if (e820_len < 0) {
                hype_fatal("fw-1: hype_e820_build failed");
            }
            if (hype_fw_cfg_add_file(&g_fw_1_fw_cfg, "etc/e820", g_fw_1_e820_blob, (uint32_t)e820_len) < 0) {
                hype_fatal("fw-1: fw_cfg registry full while registering e820");
            }
        }
    }

    /* CMOS 0x34/0x35: OVMF's memory-size fallback if it doesn't honor
     * etc/e820 above. Report the SAME guest RAM size (in 64KB units above
     * 16MB) so the two agree -- NOT the host's total RAM, which is what
     * put OVMF's stack just below 4GB in the host MMIO hole. 1 GiB is
     * 0x3F00 units, well under the register's 16-bit range; the clamp
     * stays only as a fail-safe. */
    {
        uint64_t above_16mb = (HYPE_FW_1_GUEST_RAM_BYTES > 16ULL * 1024 * 1024)
                                  ? (HYPE_FW_1_GUEST_RAM_BYTES - 16ULL * 1024 * 1024)
                                  : 0;
        uint64_t units_64kb = above_16mb / 65536ULL;
        if (units_64kb > 0xFFFFULL) {
            units_64kb = 0xFFFFULL;
        }
        hype_cmos_reset(&g_fw_1_cmos);
        hype_cmos_set_extended_memory_above_16mb(&g_fw_1_cmos, (uint16_t)units_64kb);
    }

    /* Real x86 reset state is CS.base=0xFFFF0000, RIP=0xFFF0 -- NOT
     * CS.base=0xFFFFFFF0, RIP=0, despite both giving the identical
     * initial linear address. Real firmware's own ResetVector code
     * relies on RIP starting near the *top* of its 16-bit range (its
     * own first jump is a negative/backward displacement); starting
     * RIP at 0 instead underflows that same displacement into a #GP
     * against the real-mode CS limit (see hype_svm_vcpu_set_rip()'s own
     * comment -- confirmed empirically, not guessed). */
    reset_cs_base = 0x100000000ULL - 0x10000ULL; /* 0xFFFF0000 */
    reset_rip = 0xFFF0ULL;
    stack_top = (uint64_t)(uintptr_t)(g_fw_1_guest_stack + sizeof(g_fw_1_guest_stack));

    /* FW-1a NPT layout (replaces the old flat identity map, which handed
     * the guest the host's real sub-4GB MMIO hole as if it were RAM):
     * only build the low 4GB of guest-physical space -- guest-physical
     * >= 4GB is left not-present (pdpt[4..] stay zero), since a 1 GiB
     * guest never addresses there. Then:
     *   [0, GUEST_RAM)            -> the real allocated host RAM buffer
     *   [4GB - combined, 4GB)     -> the OVMF flash window (as before)
     *   [GUEST_RAM, 4GB-combined) -> not present, so a stray or MMIO
     *                                access (MMCONFIG 0xE0000000, LAPIC/
     *                                IOAPIC, an unassigned BAR, ...)
     *                                takes a located #VMEXIT_NPF instead
     *                                of silently reaching host RAM/MMIO.
     * combined_size is a whole 2MB multiple (both .fd sizes are
     * page-aligned and this vendored build's CODE+VARS land on a
     * 2MB-aligned combined size); GUEST_RAM and 4GB are 2MB-aligned too,
     * so every range below is 2MB-granular. */
    hype_npt_build_identity(g_npt_pml4, g_npt_pdpt, g_npt_pd, 4);
    hype_npt_map_range(g_npt_pd, 0, g_fw_1_ram_host_phys, HYPE_FW_1_GUEST_RAM_BYTES);
    hype_npt_map_range(g_npt_pd, 0x100000000ULL - g_fw_1_combined_size, g_fw_1_combined_host_phys,
                        g_fw_1_combined_size);
    hype_npt_mark_range_not_present(g_npt_pd, HYPE_FW_1_GUEST_RAM_BYTES,
                                     (0x100000000ULL - g_fw_1_combined_size) - HYPE_FW_1_GUEST_RAM_BYTES);
    npt_root_phys = (uint64_t)(uintptr_t)g_npt_pml4;

    /* VALID-1/VALID-3: the guest-physical -> host map, mirroring the two
     * hype_npt_map_range() calls above exactly. The AHCI DMA path
     * bounds-checks every guest-supplied address against this before
     * dereferencing it, so OVMF/the guest OS can never steer a device
     * DMA outside its own RAM/flash. */
    hype_gpa_map_reset(&g_fw_1_dma_map);
    hype_gpa_map_add(&g_fw_1_dma_map, 0, g_fw_1_ram_host_phys, HYPE_FW_1_GUEST_RAM_BYTES);
    hype_gpa_map_add(&g_fw_1_dma_map, 0x100000000ULL - g_fw_1_combined_size, g_fw_1_combined_host_phys,
                      g_fw_1_combined_size);

    hype_debug_print(
        "fw-1: launching real OVMF at cs_base=0x%llx rip=0x%llx (guest-physical [0x%llx,0x100000000) -> "
        "host-physical 0x%llx)\n",
        (unsigned long long)reset_cs_base, (unsigned long long)reset_rip,
        (unsigned long long)(0x100000000ULL - g_fw_1_combined_size),
        (unsigned long long)g_fw_1_combined_host_phys);

    ctx = hype_svm_vcpu_create(reset_cs_base, stack_top, npt_root_phys);
    if (ctx == 0) {
        hype_fatal("fw-1: vcpu_create failed");
    }
    hype_svm_vcpu_set_rip(ctx, reset_rip);
    /* M4-6: let the guest own every exception vector. OVMF and any OS it
     * boots (real Linux takes routine #PF/#GP/#UD/#NM) handle their own
     * faults via their own IDTs; intercepting exceptions -- the strict
     * default that caught the FW-1 bring-up faults -- is fatal to a real
     * guest. An unrecoverable triple fault still returns to us as
     * HYPE_SVM_EXITCODE_SHUTDOWN. */
    hype_svm_vcpu_set_exception_intercepts(ctx, 0);

    {
    unsigned long long productive_exits = 0;
    unsigned long long total_exits = 0;
    /* Lightweight per-exit-reason histogram (M4-6 real-HW perf triage): a
     * running count of exits by kind, plus the three suspected hot
     * sub-buckets (io_delay port 0x80, SPEC_CTRL MSR 0x48, AHCI ABAR
     * NPF). Dumped to the log every few seconds so diffing consecutive
     * lines shows which exit type dominates a slow stretch. Just integer
     * increments -- no per-exit formatting. */
    unsigned long long ex_hlt = 0, ex_npf = 0, ex_ioio = 0, ex_msr = 0, ex_cpuid = 0, ex_vintr = 0,
                       ex_other = 0;
    unsigned long long ex_io80 = 0, ex_ahci_npf = 0;
    uint64_t last_exhist_tsc = 0;
    int booted = 0;
    hype_vt_filter_t uart_filter;
    char uart_line[256];
    unsigned int uart_line_len = 0;
    hype_vt_filter_t uart_filter2;
    char uart_line2[256];
    unsigned int uart_line_len2 = 0;
    int key_injected = 0;
    int key_reacted = 0;
    unsigned long long kbd_poll_run = 0; /* FW-1g: consecutive empty keyboard status polls */
    unsigned long long timer_irqs = 0;   /* M4-6b: guest LAPIC-timer IRQs actually delivered */
    unsigned long long pit_irqs = 0;     /* M4-6b4: PIT IRQ0 (legacy clockevent) IRQs delivered */
    unsigned long long ahci_irqs = 0;    /* M4-6d2: AHCI completion IRQs raised on the guest's line */
    unsigned long long console_chars = 0;
    unsigned long long inject_chars = 0;
    unsigned long long inject_productive = 0;
    unsigned long long inject_total = 0;
    hype_vt_filter_t dbg_filter;
    char dbg_line[256];
    unsigned int dbg_line_len = 0;
    /* FW-1h: set once OVMF has sized+placed BAR5 and enabled Memory
     * Space on the AHCI function -- from then on, faults inside the
     * ABAR window route to the (RAM-remap-aware) AHCI MMIO handler. */
    int ahci_mapped = 0;
    uint64_t ahci_abar = 0;
    /* M4-6b1: drive the guest timebase from real elapsed host TSC. */
    uint64_t tb_last_tsc = hype_rdtsc();
    uint64_t tb_accum = 0; /* fractional-tick carry (units of host TSC * PIT_HZ) */
    /* M4-6d2b: host TSC of the most recent productive (non-HLT) exit --
     * "last time the guest did real work". The idle detector measures
     * wall-clock since this to decide the guest is quiescent. */
    uint64_t last_progress_tsc = tb_last_tsc;

    hype_vt_filter_reset(&uart_filter);
    hype_vt_filter_reset(&uart_filter2);
    hype_vt_filter_reset(&dbg_filter);

    /* M4-6d4: arm the host exception catcher for the duration of the loop
     * so a host fault produces a flushed PANIC (with the faulting vector +
     * RIP) instead of a silent triple-fault machine reset. */
    fw_1_install_exception_catcher();

    for (;;) {
        uint8_t timer_vector;

        /* M4-6d4: localise the ~219us/exit real-HW cost. Bracket VMRUN to
         * separate the SVM world-switch cost (t_pre..t_post) from the
         * loop-body cost (prev t_post..this t_pre). If world-switch
         * dominates it is a CPU/microcode/TLB-flush matter; if the body
         * dominates it is our own code (GOP framebuffer render, real-serial
         * writes, console drain) and is fixable in software. */
        {
            uint64_t t_pre = hype_rdtsc();
            if (g_fw_1_prev_post_tsc != 0) {
                g_fw_1_body_tsc += t_pre - g_fw_1_prev_post_tsc;
            }
            if (ops->vcpu_run(ctx, &info) != 0) {
                hype_fatal("fw-1: VM-entry failed (reason=0x%llx)", (unsigned long long)info.reason);
            }
            {
                uint64_t t_post = hype_rdtsc();
                uint64_t this_vmrun = t_post - t_pre;
                g_fw_1_vmrun_tsc += this_vmrun;
                g_fw_1_prev_post_tsc = t_post;
                /* M4-6d4: the DECISIVE probe for the host-preemption-timer
                 * question. A single VMRUN's duration = how long the guest ran
                 * before it voluntarily exited. If the MAX is multi-second, the
                 * guest runs long non-intercepting stretches during which we
                 * cannot inject its timer tick (no INTR intercept) -- exactly
                 * what a host preemption timer fixes, and invisible to the
                 * between-exits RECOVER metric. If the max stays small (<~10ms)
                 * the guest exits frequently and the preemption timer would not
                 * help; the slowness is genuine guest work. Also count how many
                 * VMRUNs ran longer than 100ms. */
                if (this_vmrun > g_fw_1_vmrun_max_tsc) {
                    g_fw_1_vmrun_max_tsc = this_vmrun;
                }
                if (g_fw_1_host_tsc_hz != 0 && this_vmrun > g_fw_1_host_tsc_hz / 10ULL) {
                    g_fw_1_vmrun_over100ms++;
                }
            }
        }

        if (++total_exits > HYPE_FW_1_MAX_EXITS) {
            hype_fatal("fw-1: exceeded %llu VM-exits without reaching a stable idle -- guest stuck "
                       "(last reason=0x%llx guest_rip=0x%llx)",
                       (unsigned long long)HYPE_FW_1_MAX_EXITS, (unsigned long long)info.reason,
                       (unsigned long long)info.guest_rip);
        }
        /* FW-1e: keep the first (riskiest) VMRUN traced, then silence the
         * per-exit CLGI/VMLOAD/VMRUN spam -- real OVMF does thousands of
         * exits and each trace line is GOP-rendered. */
        if (total_exits == 1) {
            hype_svm_set_vmrun_trace(0);
        }
        if (info.reason != HYPE_SVM_EXITCODE_HLT) {
            productive_exits++;
        }

        /* Per-exit-reason tally (see the ex_* declarations). Sub-buckets
         * (io80/msr_spec/ahci) are bumped at their handlers below. */
        switch (info.reason) {
            case HYPE_SVM_EXITCODE_HLT:   ex_hlt++;   break;
            case HYPE_SVM_EXITCODE_NPF:   ex_npf++;   break;
            case HYPE_SVM_EXITCODE_IOIO:  ex_ioio++;  break;
            case HYPE_SVM_EXITCODE_MSR:   ex_msr++;   break;
            case HYPE_SVM_EXITCODE_CPUID: ex_cpuid++; break;
            case HYPE_SVM_EXITCODE_VINTR: ex_vintr++; break;
            default:                      ex_other++; break;
        }
        /* Dump the histogram every ~5s of wall-clock. The periodic flush
         * below writes it to \hype-log.txt, so diffing two EXHIST lines
         * gives per-second exit rates by kind for the slow stretch. */
        if (g_fw_1_host_tsc_hz != 0) {
            uint64_t now_eh = hype_rdtsc();
            if (last_exhist_tsc == 0 || now_eh - last_exhist_tsc >= 5ULL * g_fw_1_host_tsc_hz) {
                last_exhist_tsc = now_eh;
                hype_debug_print("fw-1 EXHIST: total=%llu hlt=%llu npf=%llu(ahci=%llu) ioio=%llu(io80=%llu) "
                                 "msr=%llu cpuid=%llu vintr=%llu other=%llu\n",
                                 total_exits, ex_hlt, ex_npf, ex_ahci_npf, ex_ioio, ex_io80, ex_msr,
                                 ex_cpuid, ex_vintr, ex_other);
                /* M4-6d4: mean per-exit cost split VMRUN world-switch vs our
                 * loop body, in nanoseconds (TSC / host_tsc_hz * 1e9). Tells
                 * whether the ~219us/exit is the CPU's world-switch (VMRUN
                 * dominates -> hard) or our code (body dominates -> fixable
                 * GOP/serial/drain work). */
                if (total_exits != 0) {
                    unsigned long long vmrun_ns =
                        (g_fw_1_vmrun_tsc / total_exits) * 1000000000ULL / g_fw_1_host_tsc_hz;
                    unsigned long long body_ns =
                        (g_fw_1_body_tsc / total_exits) * 1000000000ULL / g_fw_1_host_tsc_hz;
                    hype_debug_print("fw-1 COSTHIST: mean_per_exit vmrun=%lluns body=%lluns "
                                     "(vmrun_tot=%llums body_tot=%llums)\n",
                                     vmrun_ns, body_ns,
                                     (g_fw_1_vmrun_tsc / g_fw_1_host_tsc_hz) * 1000ULL,
                                     (g_fw_1_body_tsc / g_fw_1_host_tsc_hz) * 1000ULL);
                    /* M4-6d4 MEASUREMENT: cumulative wall-clock the timer IRQ0
                     * spent pending+deliverable but BLOCKED by an in-service
                     * lower-priority IRQ (guest IF=1) -- the time a fair
                     * priority-preemption scheme would reclaim. Big = the tick
                     * fix is worth it; small = the slowness is elsewhere (busy
                     * CD-read work, not recoverable by that fix). */
                    hype_debug_print("fw-1 RECOVER: irq0_pending_undelivered=%llums (tick lateness, "
                                     "upper bound) | blocked_by_isr_IF1=%llums (priority-fix reclaims)\n",
                                     (g_fw_1_irq0_pending_tsc * 1000ULL) / g_fw_1_host_tsc_hz,
                                     (g_fw_1_irq0_recoverable_tsc * 1000ULL) / g_fw_1_host_tsc_hz);
                    /* M4-6d4: longest uninterrupted guest run. max_vmrun large
                     * => guest runs non-intercepting stretches we can't inject
                     * ticks into => host preemption timer is the fix. (ms via
                     * multiply-before-divide so sub-second values survive --
                     * the whole point is telling <10ms from multi-second.) */
                    hype_debug_print("fw-1 PREEMPT: max_single_vmrun=%llums vmruns_over_100ms=%llu\n",
                                     (g_fw_1_vmrun_max_tsc * 1000ULL) / g_fw_1_host_tsc_hz,
                                     g_fw_1_vmrun_over100ms);
                }
                /* M4-6d4: companion line to EXHIST. The real-HW soft lockups
                 * (blkid stuck 24s, kworker stuck 26s -- confirmed by a
                 * "clocksource: Long readout interval ... 22.6s" the guest
                 * itself logged) are the scheduler-tick timer IRQ starving a
                 * task for 20+s while the loop still takes occasional
                 * productive exits (so neither the WEDGE dump, gated on a
                 * stuck in-service bit, nor the 10s idle-giveup ever fires --
                 * and the GIVEUP TIMER-STATE only prints on loop exit, which a
                 * live boot never reaches). Emitting the cumulative delivered
                 * timer-IRQ counts + the current clockevent programming every
                 * 5s lets a diff of two TIMERHIST lines across the slow region
                 * show directly whether (and which) clockevent source stopped
                 * firing, and whether the guest masked IRQ0 (mIMR bit0). Pure
                 * diagnostic -- no guest-visible state changes. */
                hype_debug_print("fw-1 TIMERHIST: pit_irq0=%llu lapic_irq=%llu ahci_irq=%llu | "
                                 "PIT0 mode=%u reload=%u counter=%u | LAPIC lvt=0x%x(%s) init=%u cur=%u | "
                                 "mIMR=0x%x sIMR=0x%x mISR=0x%x sISR=0x%x\n",
                                 (unsigned long long)pit_irqs, (unsigned long long)timer_irqs,
                                 (unsigned long long)ahci_irqs,
                                 (unsigned int)g_fw_1_pit.channels[0].mode,
                                 (unsigned int)g_fw_1_pit.channels[0].reload,
                                 (unsigned int)g_fw_1_pit.channels[0].counter,
                                 (unsigned int)g_fw_1_lapic.lvt_timer,
                                 (g_fw_1_lapic.lvt_timer & HYPE_GUEST_LAPIC_LVT_MASKED)
                                     ? "masked"
                                     : ((g_fw_1_lapic.lvt_timer & HYPE_GUEST_LAPIC_LVT_PERIODIC)
                                            ? "periodic"
                                            : "1shot"),
                                 (unsigned int)g_fw_1_lapic.init_count,
                                 (unsigned int)g_fw_1_lapic.current_count,
                                 (unsigned int)g_fw_1_pic.master.imr, (unsigned int)g_fw_1_pic.slave.imr,
                                 (unsigned int)g_fw_1_pic.master.isr, (unsigned int)g_fw_1_pic.slave.isr);
            }
        }

        /* Periodically flush the captured console log to \hype-log.txt so
         * a mid-boot hang OR crash on real hardware still leaves an
         * up-to-date log on the USB stick (the end-of-run write only fires
         * if the loop returns; a live boot never does). ~3s of wall-clock
         * between flushes keeps the file I/O cost negligible; fw_1_flush_log
         * self-disables if the volume ever rejects a write. */
        {
            static uint64_t last_flush_tsc = 0;
            uint64_t now_flush = hype_rdtsc();
            if (!g_fw_1_log_disabled && g_fw_1_host_tsc_hz != 0 &&
                (last_flush_tsc == 0 || now_flush - last_flush_tsc >= 3ULL * g_fw_1_host_tsc_hz)) {
                last_flush_tsc = now_flush;
                fw_1_flush_log();
            }
        }

        /* M4-6b1: advance the guest PIT + LAPIC timer by the number of
         * 1.193182 MHz ticks that really elapsed since the last exit
         * (real host TSC delta scaled by PIT_HZ / host_tsc_hz), instead
         * of a fixed one-tick-per-exit. This makes guest time a stable
         * fraction of the TSC the guest reads natively, so the kernel's
         * PIT-based TSC calibration lands at the true CPU frequency and
         * its LAPIC-timer calibration (against the same timebase) is
         * consistent -- the prerequisite for a working clockevent. The
         * fractional remainder is carried in tb_accum so no ticks are
         * lost to truncation. Falls back to one tick/exit if calibration
         * was unavailable (host_tsc_hz == 0). When the timer IRQ comes
         * due, deliver it via the INT-1/INT-2 EVENTINJ/VINTR path. */
        {
            uint64_t now_tsc = hype_rdtsc();
            uint64_t delta = now_tsc - tb_last_tsc;
            uint64_t ticks;
            tb_last_tsc = now_tsc;
            /* M4-6d2b: any non-HLT exit is the guest doing real work
             * (MMIO, port I/O, NPF, CPUID/MSR ...) -- mark progress so the
             * idle detector only fires after a true quiescent stretch. */
            if (info.reason != HYPE_SVM_EXITCODE_HLT) {
                last_progress_tsc = now_tsc;
            }
            if (g_fw_1_host_tsc_hz != 0) {
                /* Cap the delta only to prevent the delta * PIT_HZ
                 * multiply from overflowing (UINT64_MAX / PIT_HZ is ~1.5e13
                 * TSC, so 300s at any realistic clock is comfortably safe).
                 * The old 1s cap was far too tight: on real hardware the
                 * guest genuinely idles for multi-second stretches (a
                 * tickless kernel waits on a one-shot LAPIC timer, and HLT
                 * blocks until the next exit), and clamping each such gap to
                 * 1s DISCARDED the elapsed time -- guest time fell ~16x
                 * behind real time, so a short one-shot timeout took minutes
                 * and never fired inside the idle-giveup window. Advancing
                 * by the full elapsed time lets the one-shot fire when the
                 * guest expects. (QEMU fast-spins the intercepted HLT with
                 * microsecond deltas, so it never approaches this cap.) */
                uint64_t delta_cap = 300ULL * g_fw_1_host_tsc_hz;
                if (delta > delta_cap) {
                    delta = delta_cap;
                }
                tb_accum += delta * HYPE_PIT_HZ;
                ticks = tb_accum / g_fw_1_host_tsc_hz;
                tb_accum -= ticks * g_fw_1_host_tsc_hz;
            } else {
                ticks = 1;
            }
            if (ticks != 0) {
                hype_guest_lapic_advance(&g_fw_1_lapic, ticks);
                /* Channel-0 terminal-count crossings during this advance
                 * are PIT IRQ0 timer edges (M4-6b4). */
                if (hype_pit_emu_advance(&g_fw_1_pit, ticks) != 0) {
                    hype_pic_emu_raise_irq(&g_fw_1_pic.master, 0);
                }
            }
        }
        if (hype_guest_lapic_take_timer_irq(&g_fw_1_lapic, &timer_vector)) {
            hype_svm_vcpu_request_interrupt(ctx, timer_vector);
            timer_irqs++;
        }
        /* M4-6d2: raise the AHCI completion IRQ on the line the guest
         * actually programmed. hype_ahci_irq_pending() is level-sensitive
         * (GHC.IE && PxIS&PxIE); the line comes from the AHCI function's
         * PCI Interrupt Line (config 0x3C, which OVMF routed -- typically
         * 11, a slave-PIC line). Only (re)raise when that line isn't
         * already in service, so a still-asserted level doesn't re-request
         * while the guest's ISR is running. */
        if (ahci_mapped && hype_ahci_irq_pending(&g_fw_1_ahci)) {
            uint8_t line = hype_pci_get_interrupt_line(&g_fw_1_pci, HYPE_FW_1_PCI_DEV_AHCI);
            if (line != 0u && line < 16u) {
                int in_service = (line < 8u)
                    ? ((g_fw_1_pic.master.isr & (uint8_t)(1u << line)) != 0)
                    : ((g_fw_1_pic.slave.isr & (uint8_t)(1u << (line - 8u))) != 0);
                if (!in_service) {
                    hype_pic_emu_raise_global_irq(&g_fw_1_pic, line);
                    ahci_irqs++;
                }
            }
        }
        /* M4-6d3: raise the serial TX/RX interrupt (COM1=IRQ4, COM2=IRQ3)
         * when the guest has enabled it (IER.ETBEI/ERBFI). The kernel's
         * printk uses the polled console path (LSR.THRE, always ready), but
         * a userspace tty write goes through the 8250 driver's interrupt-
         * driven TX: it enables ETBEI and sleeps until the TX-empty IRQ
         * fires. Without this, any process that writes enough to the serial
         * console (e.g. apk's --progress bars during "Installing packages")
         * blocks forever. Gated on the line being unmasked (the guest
         * actually uses serial IRQs on it -- e.g. COM1/IRQ4) and not
         * already in service; raising a masked line (e.g. COM2/IRQ3 when
         * the guest only polls ttyS1) would just leave a stuck, unhandled
         * IRR bit. */
        {
            unsigned u;
            for (u = 0; u < 2u; u++) {
                hype_guest_uart_t *uart = (u == 0) ? &g_fw_1_uart : &g_fw_1_uart2;
                uint8_t irqn = (u == 0) ? 4u : 3u; /* COM1=IRQ4, COM2=IRQ3 */
                uint8_t bit = (uint8_t)(1u << irqn);
                if (hype_guest_uart_irq_pending(uart) &&
                    (g_fw_1_pic.master.imr & bit) == 0 &&
                    (g_fw_1_pic.master.isr & bit) == 0) {
                    hype_pic_emu_raise_irq(&g_fw_1_pic.master, irqn);
                }
            }
        }
        /* M4-6b4/M4-6d2: deliver the highest-priority pending PIC IRQ --
         * the PIT IRQ0 clockevent (master) or the AHCI completion IRQ
         * (master or slave, via the cascade). A fully-legacy guest (our
         * FW-1 guest gets no RSDP) drives both through the 8259 pair.
         * Deliver only when nothing is already in service anywhere (both
         * ISRs clear), i.e. the guest has EOI'd the previous IRQ --
         * modelling single-in-service INTA gating and preventing a fast
         * timer cadence from flooding nested IRQs. acknowledge() honours
         * the guest's IMRs and the master/slave cascade, and returns the
         * guest-programmed vector. */
        /* M4-6d2: first deliver any already-deferred IRQ the moment the
         * guest can accept it (don't wait only on the VINTR intercept --
         * that gap stranded the timer tick and froze jiffies). If it
         * injected, skip acknowledging a new one this iteration so the
         * freshly-staged EVENTINJ isn't clobbered. */
        if (!hype_svm_vcpu_deliver_pending_if_ready(ctx) &&
            g_fw_1_pic.master.isr == 0 && g_fw_1_pic.slave.isr == 0) {
            uint8_t pic_vector;
            if (hype_pic_emu_acknowledge(&g_fw_1_pic, &pic_vector)) {
                hype_svm_vcpu_request_interrupt(ctx, pic_vector);
                /* Attribute by the master vector base: IRQ0 = PIT
                 * clockevent, anything else = the AHCI line. */
                if (pic_vector == g_fw_1_pic.master.irq_offset) {
                    pit_irqs++;
                } else {
                    ahci_irqs++;
                }
            }
        }

        /* M4-6d4 MEASUREMENT: accumulate wall-clock during which the timer
         * IRQ0 is pending+unmasked, an ISR is in service (so the strict gate
         * above blocked it), AND the guest can accept it (IF=1, no shadow) --
         * the exact time a fair priority-preemption scheme would reclaim.
         * Cheap PIC pre-check first; only read the guest intr state (the
         * costly part) when IRQ0 is actually pending behind an in-service
         * IRQ, so steady-state overhead stays negligible. */
        {
            uint64_t now_sb = hype_rdtsc();
            if (g_fw_1_stall_prev_tsc != 0) {
                int irq0_pending = (g_fw_1_pic.master.irr & 0x01u) != 0 &&
                                   (g_fw_1_pic.master.imr & 0x01u) == 0;
                if (irq0_pending) {
                    uint64_t dt = now_sb - g_fw_1_stall_prev_tsc;
                    int isr_busy = (g_fw_1_pic.master.isr != 0 || g_fw_1_pic.slave.isr != 0);
                    g_fw_1_irq0_pending_tsc += dt; /* IF-agnostic: total tick lateness */
                    if (isr_busy) {
                        hype_svm_intr_state_t sb;
                        hype_svm_vcpu_get_intr_state(ctx, &sb);
                        if (((sb.rflags >> 9) & 1u) != 0 && sb.interrupt_shadow == 0) {
                            /* Blocked ONLY by the in-service gate, guest able
                             * to accept -- what a priority fix reclaims. */
                            g_fw_1_irq0_recoverable_tsc += dt;
                        }
                    }
                }
            }
            g_fw_1_stall_prev_tsc = now_sb;
        }

        /* M4-6d4: TIMER-STARVATION detector. The real-HW soft lockups
         * happen while the guest is BUSY (an AHCI-IRQ storm from CD reads),
         * so the WEDGE dump above -- gated on 2s of QUIESCENCE -- never sees
         * them. This one instead watches the timer clockevent directly: if
         * IRQ0 has been raised in the master PIC's IRR (a PIT tick edge is
         * pending) but NOT delivered for >2s of wall-clock, the scheduler
         * tick is starving -- exactly the soft-lockup condition. Dump, once
         * per starvation episode, the precise reason: a non-zero ISR means
         * the "both ISRs clear" delivery gate is blocked by an in-service
         * IRQ the guest hasn't EOI'd (e.g. a stuck AHCI/cascade line);
         * can_accept=0 (IF=0/shadow) means the guest has interrupts masked;
         * a growing defer-overwrite means staged injections are being lost.
         * The episode re-arms whenever a PIT IRQ0 is actually delivered
         * (pit_irqs advances) or the IRR bit clears. Pure diagnostic. */
        {
            static uint64_t irq0_raised_since = 0;
            static unsigned long long pit_irqs_at_arm = 0;
            static int starve_dumped = 0;
            int irq0_in_irr = (g_fw_1_pic.master.irr & 0x01u) != 0;
            uint64_t now_sv = hype_rdtsc();
            if (pit_irqs != pit_irqs_at_arm) {
                /* A tick was delivered -- reset the episode. */
                pit_irqs_at_arm = pit_irqs;
                irq0_raised_since = 0;
                starve_dumped = 0;
            }
            if (!irq0_in_irr) {
                irq0_raised_since = 0;
                starve_dumped = 0;
            } else if (g_fw_1_host_tsc_hz != 0) {
                if (irq0_raised_since == 0) {
                    irq0_raised_since = now_sv;
                } else if (!starve_dumped &&
                           now_sv - irq0_raised_since >= 2ULL * g_fw_1_host_tsc_hz) {
                    hype_svm_intr_state_t sv;
                    unsigned long long ei = 0, df = 0, wn = 0, ov = 0;
                    starve_dumped = 1;
                    hype_svm_vcpu_get_intr_state(ctx, &sv);
                    hype_svm_vcpu_get_int_diag(&ei, &df, &wn, &ov);
                    hype_debug_print(
                        "fw-1 TIMER-STARVE: IRQ0 undelivered >2s | IF=%d shadow=0x%llx can_accept=%d "
                        "pending=%d/vec0x%x eventinj=0x%llx | mIRR=0x%x mISR=0x%x mIMR=0x%x "
                        "sIRR=0x%x sISR=0x%x sIMR=0x%x | ahci p_is=0x%x p_ie=0x%x p_ci=0x%x | "
                        "defer=%llu overwrite=%llu | pit_irqs=%llu ahci_irqs=%llu\n",
                        (int)((sv.rflags >> 9) & 1u), (unsigned long long)sv.interrupt_shadow,
                        sv.can_accept, sv.pending_valid, (unsigned int)sv.pending_vector,
                        (unsigned long long)sv.eventinj,
                        (unsigned int)g_fw_1_pic.master.irr, (unsigned int)g_fw_1_pic.master.isr,
                        (unsigned int)g_fw_1_pic.master.imr, (unsigned int)g_fw_1_pic.slave.irr,
                        (unsigned int)g_fw_1_pic.slave.isr, (unsigned int)g_fw_1_pic.slave.imr,
                        (unsigned int)g_fw_1_ahci.p_is, (unsigned int)g_fw_1_ahci.p_ie,
                        (unsigned int)g_fw_1_ahci.p_ci, df, ov,
                        (unsigned long long)pit_irqs, (unsigned long long)ahci_irqs);
                }
            }
        }

        /* FW-1e: surface any console text the guest wrote to either UART.
         * FW-1f uses the emitted-char count as evidence the guest reacted
         * to an injected keystroke. */
        console_chars += fw_1_drain_uart_console(&g_fw_1_uart, &uart_filter, uart_line, &uart_line_len,
                                                  (unsigned int)sizeof(uart_line));
        console_chars += fw_1_drain_uart_console(&g_fw_1_uart2, &uart_filter2, uart_line2, &uart_line_len2,
                                                  (unsigned int)sizeof(uart_line2));

        /* M4-6d3: flush a buffered partial line that looks like an
         * interactive prompt (ends in ": ", "# ", "$ ", "> ") when the
         * guest goes idle. An agetty "localhost login: " prompt sends no
         * trailing newline, so the line-buffered drain above would never
         * surface it -- yet it's the very milestone we care about. Only
         * prompt-suffixed partials are flushed, so ordinary mid-line
         * output isn't fragmented. */
        if (info.reason == HYPE_SVM_EXITCODE_HLT) {
            char *bufs[2];
            unsigned int *lens[2];
            const char *tags[2];
            unsigned bi;
            bufs[0] = uart_line;  lens[0] = &uart_line_len;  tags[0] = "fw-1 ttyS0|";
            bufs[1] = uart_line2; lens[1] = &uart_line_len2; tags[1] = "fw-1 ttyS1|";
            for (bi = 0; bi < 2u; bi++) {
                unsigned int n = *lens[bi];
                if (n >= 2u && bufs[bi][n - 1u] == ' ') {
                    char p = bufs[bi][n - 2u];
                    if (p == ':' || p == '#' || p == '$' || p == '>') {
                        bufs[bi][n] = '\0';
                        hype_debug_print("%s %s\n", tags[bi], bufs[bi]);
                        *lens[bi] = 0;
                    }
                }
            }
        }

        /* FW-1g: "reacted" = the guest emitted new CONSOLE output after
         * we fed it a key -- evidence the keystroke registered and drove
         * the guest forward. Recorded for the final report; it does NOT
         * end the run. The key injection (in the PS/2 branch below)
         * UNBLOCKS each interactive prompt so the guest keeps booting --
         * FW-1h needs the CD to boot and M4-6 needs the kernel to start,
         * both of which happen only if the loop runs on past the first
         * prompt rather than terminating at it. */
        if (key_injected && !key_reacted &&
            console_chars - inject_chars >= HYPE_FW_1_KEY_REACTION_CHARS) {
            key_reacted = 1;
        }

        if (info.reason == HYPE_SVM_EXITCODE_CPUID) {
            hype_svm_vcpu_handle_cpuid(ctx);
            continue;
        }
        if (info.reason == HYPE_SVM_EXITCODE_MSR) {
            if (hype_svm_vcpu_handle_msr(ctx) != 0) {
                hype_fatal("fw-1: unhandled guest MSR access (qual=0x%llx guest_rip=0x%llx)",
                           (unsigned long long)info.qualification, (unsigned long long)info.guest_rip);
            }
            continue;
        }

        if (info.reason == HYPE_SVM_EXITCODE_VINTR) {
            /* The VINTR window opened -- the guest can now take the
             * pending timer IRQ (INT-2). */
            hype_svm_vcpu_handle_vintr_window(ctx);
            continue;
        }

        if (info.reason == HYPE_SVM_EXITCODE_NPF) {
            hype_svm_npf_t npf;
            /* Faulting-instruction bytes for MMIO decode. Prefer AMD's
             * decode-assist capture (valid regardless of guest paging):
             * once a guest runs its own virtual address space (a Linux
             * kernel's RIP is a high-canonical virtual address), RIP is
             * no longer a guest-physical address, so translating it can't
             * reach the instruction. Fall back to a guest-physical
             * translation of RIP for an identity-paged guest (OVMF) on a
             * CPU without decode assists. */
            uint8_t insn_n = 0;
            const uint8_t *insn = hype_svm_vcpu_guest_insn_bytes(ctx, &insn_n);
            /* M4-6 real-AMD DIAG: the decode-assist path (insn_n > 0) is
             * used only on real AMD -- QEMU+KVM nested SVM never populates
             * num_bytes_fetched, so it's the ptwalk fallback there and this
             * path has never actually been exercised. One-shot dump of the
             * first NPF's decode-assist state + bytes: n=0 means real HW
             * ALSO isn't giving decode assists (unexpected); n>0 with sane
             * instruction bytes means the path is live and we can trust the
             * MMIO decode. */
            {
                static int fw1_da_diag = 0;
                if (!fw1_da_diag) {
                    fw1_da_diag = 1;
                    hype_debug_print("fw-1 DIAG: 1st NPF decode-assist n=%u (0=none->ptwalk) "
                                      "insn=%02x %02x %02x %02x guest_rip=0x%llx\n",
                                      (unsigned int)insn_n,
                                      insn_n > 0 && insn ? insn[0] : 0, insn_n > 1 && insn ? insn[1] : 0,
                                      insn_n > 2 && insn ? insn[2] : 0, insn_n > 3 && insn ? insn[3] : 0,
                                      (unsigned long long)info.guest_rip);
                }
            }
            if (insn_n == 0) {
                insn = fw_1_insn_bytes_via_ptwalk(ctx, info.guest_rip);
            }
            /* FW-1b: the guest Local APIC at 0xFEE00000 is the expected
             * NPF here (the region is not-present by design). */
            if (hype_svm_vcpu_handle_lapic_npf(ctx, &g_fw_1_lapic, HYPE_LAPIC_DEFAULT_BASE, insn) == 0) {
                continue;
            }
            /* FW-1c: PCI config space via MMCONFIG ECAM at 0xE0000000
             * (OVMF's Q35 PcdPciExpressBaseAddress). Reuses PCI-1's ECAM
             * config model over FW-1's own host bridge + LPC devices. */
            if (hype_svm_vcpu_handle_pci_ecam_npf(ctx, &g_fw_1_pci, HYPE_FW_1_ECAM_GPA, insn) == 0) {
                /* FW-1h: an ECAM config write may have just programmed
                 * BAR5 and set Memory Space Enable on the AHCI function.
                 * That is the moment OVMF's PciBusDxe finalizes the
                 * controller's MMIO window -- capture the guest-physical
                 * ABAR base so ABAR-window faults route to the AHCI
                 * handler from here on. No NPT change is needed: the
                 * whole [GUEST_RAM, 4GB-flash) span is already not-
                 * present (FW-1a's mark_range_not_present), so the ABAR
                 * OVMF assigns inside the 32-bit PCI aperture already
                 * faults as an NPF -- unlike PCI-2's full identity map,
                 * which had to mark it not-present explicitly. Only
                 * latched once; this guest never reprograms BAR5. */
                if (!ahci_mapped && hype_pci_memory_space_enabled(&g_fw_1_pci, HYPE_FW_1_PCI_DEV_AHCI)) {
                    uint64_t bar5 = hype_pci_get_bar_value(&g_fw_1_pci, HYPE_FW_1_PCI_DEV_AHCI, 5);
                    if (bar5 != 0) {
                        ahci_abar = bar5;
                        ahci_mapped = 1;
                        hype_debug_print("fw-1: AHCI BAR5 (ABAR) enabled at guest-physical 0x%llx -- "
                                          "routing its MMIO to the CD-ROM model now\n",
                                          (unsigned long long)bar5);
                    }
                }
                continue;
            }
            /* FW-1h: AHCI ABAR MMIO. Gate on the faulting guest-physical
             * address so this only claims accesses actually inside the
             * ABAR window (the ATAPI handler itself now range-checks too,
             * but keeping the gate here keeps an out-of-window fault on
             * the clear "unhandled NPF" path below). The _xlat variant
             * adds g_fw_1_ram_host_phys to every guest-physical DMA
             * pointer OVMF programmed, since FW-1 remaps guest RAM. */
            if (ahci_mapped) {
                hype_svm_npf_t ahci_npf;
                hype_svm_vcpu_get_last_npf(ctx, &ahci_npf);
                if (ahci_npf.guest_phys_addr >= ahci_abar &&
                    ahci_npf.guest_phys_addr < ahci_abar + HYPE_AHCI_MMIO_SIZE) {
                    if (hype_svm_vcpu_handle_ahci_npf_map(ctx, &g_fw_1_ahci, &g_fw_1_atapi, ahci_abar,
                                                           &g_fw_1_dma_map, insn) == 0) {
                        ex_ahci_npf++; /* exit-histogram sub-bucket */
                        /* M4-6 real-AMD DIAG (compact, screen-only-friendly):
                         * report CD progress at milestones instead of tracing
                         * every command. First ATAPI command proves OVMF is
                         * driving the controller; first READ(10) proves CD
                         * data I/O works; the running READ(10) count (every
                         * 64) shows sustained reads. If the guest OVMF drops
                         * to the shell with read10=0, its storage stack never
                         * read the CD (AHCI/ATAPI issue); with read10>0 but no
                         * boot, it's a boot-order/bootable-media issue. */
                        {
                            static uint32_t last_reported_reads = 0;
                            static int first_cmd_reported = 0;
                            if (!first_cmd_reported && g_fw_1_atapi.command_count > 0) {
                                first_cmd_reported = 1;
                                hype_debug_print("fw-1 DIAG: guest issued 1st ATAPI CDB (opcode=0x%x)\n",
                                                  (unsigned int)g_fw_1_atapi.last_cdb);
                            }
                            if (g_fw_1_atapi.read10_count >= 1 && last_reported_reads == 0) {
                                last_reported_reads = 1; /* latch: fire once, not per-MMIO-access */
                                hype_debug_print("fw-1 DIAG: 1st ATAPI READ(10) done -- CD data I/O "
                                                  "works on real HW (cmds=%u)\n",
                                                  (unsigned int)g_fw_1_atapi.command_count);
                            }
                            if (g_fw_1_atapi.read10_count >= last_reported_reads + 64) {
                                last_reported_reads = g_fw_1_atapi.read10_count;
                                hype_debug_print("fw-1 DIAG: ATAPI READ(10) count=%u (cmds=%u)\n",
                                                  (unsigned int)g_fw_1_atapi.read10_count,
                                                  (unsigned int)g_fw_1_atapi.command_count);
                            }
                        }
                        continue;
                    }
                    hype_fatal("fw-1: unhandled AHCI ABAR MMIO at guest-physical 0x%llx (%s, "
                               "guest_rip=0x%llx)",
                               (unsigned long long)ahci_npf.guest_phys_addr,
                               ahci_npf.is_write ? "write" : "read", (unsigned long long)info.guest_rip);
                }
            }
            hype_svm_vcpu_get_last_npf(ctx, &npf);
            hype_fatal("fw-1: unhandled NPF at guest-physical 0x%llx (%s, guest_rip=0x%llx, "
                       "decode_assist_bytes=%u insn[0..2]=%x %x %x)",
                       (unsigned long long)npf.guest_phys_addr, npf.is_write ? "write" : "read",
                       (unsigned long long)info.guest_rip, (unsigned int)insn_n,
                       insn_n > 0 ? insn[0] : 0, insn_n > 1 ? insn[1] : 0, insn_n > 2 ? insn[2] : 0);
        }

        if (info.reason == HYPE_SVM_EXITCODE_IOIO) {
            if (hype_svm_vcpu_handle_ioio(ctx, &g_fw_1_pic, &g_fw_1_pit) == 0) {
                continue;
            }
            if (hype_svm_vcpu_handle_pci_cf8_ioio(ctx, &g_fw_1_pci) == 0) {
                continue;
            }
            if (hype_svm_vcpu_handle_fw_cfg_ioio(ctx, &g_fw_1_fw_cfg) == 0) {
                continue;
            }
            if (hype_svm_vcpu_handle_cmos_ioio(ctx, &g_fw_1_cmos) == 0) {
                continue;
            }
            /* FW-1f/g: guest PS/2 keyboard/mouse (0x60/0x64) -- OVMF's
             * Ps2KeyboardDxe ConIn. Must be modeled (not absorbed) so it
             * can read the controller status + our injected scancode.
             * out_kbd_wait flags a keyboard status poll that found no
             * data -- FW-1g counts a run of these (an interactive prompt
             * busy-polling for a key, e.g. the shell's "press any key to
             * continue" countdown) to time the key injection into that
             * exact window; init-phase polling interleaves data reads and
             * command writes, which reset the run to 0. */
            {
                int kbd_wait = 0;
                if (hype_svm_vcpu_handle_ps2_ioio(ctx, &g_fw_1_ps2, &g_fw_1_mouse, &kbd_wait) == 0) {
                    if (kbd_wait) {
                        kbd_poll_run++;
                    } else {
                        kbd_poll_run = 0;
                    }
                    /* Feed Enter whenever the guest has clearly settled
                     * into an input-wait (a long run of empty status
                     * polls) past the boot threshold. This UNBLOCKS the
                     * prompt so the guest proceeds -- the early OVMF/BDS
                     * "press a key to continue" prompt, then GRUB's menu,
                     * then the shell -- rather than stalling. Re-arms only
                     * after another full poll run (kbd_poll_run reset to
                     * 0), so it never holds OBF perpetually set. */
                    if (productive_exits >= HYPE_FW_1_BOOTED_EXITS &&
                        kbd_poll_run >= HYPE_FW_1_KBD_POLL_INJECT_RUN) {
                        if (!key_injected) {
                            hype_debug_print(
                                "fw-1: guest polling the keyboard at an interactive prompt (%llu "
                                "consecutive empty status reads, %llu productive exits) -- feeding Enter "
                                "to drive it forward (FW-1g)\n",
                                (unsigned long long)kbd_poll_run, (unsigned long long)productive_exits);
                        }
                        hype_ps2_kbd_enqueue_scancode(&g_fw_1_ps2, HYPE_FW_1_KEY_ENTER_MAKE);
                        kbd_poll_run = 0;
                        key_injected = 1;
                        inject_chars = console_chars;
                        inject_productive = productive_exits;
                        inject_total = total_exits;
                    }
                    continue;
                }
            }
            /* FW-1e: guest COM1 UART (0x3F8-0x3FF). TX bytes are buffered
             * in the model and drained to hype's console at the top of
             * the loop. */
            if (hype_svm_vcpu_handle_uart_ioio(ctx, &g_fw_1_uart, HYPE_SERIAL_COM1) == 0) {
                continue;
            }
            if (hype_svm_vcpu_handle_uart_ioio(ctx, &g_fw_1_uart2, HYPE_SERIAL_COM2) == 0) {
                continue;
            }
            /* FW-1g: OVMF's SEC/PEI debug-io port (0x402). Forward the
             * log to hype's console (only active in a DEBUG OVMF build;
             * harmless otherwise). */
            {
                uint8_t dbg_byte = 0;
                int dr = hype_svm_vcpu_handle_debug_port_ioio(ctx, HYPE_FW_1_DEBUG_PORT, &dbg_byte);
                if (dr == 0) {
                    fw_1_debug_feed(&dbg_filter, dbg_line, &dbg_line_len, (unsigned int)sizeof(dbg_line),
                                    dbg_byte);
                    continue;
                }
                if (dr == 1) {
                    continue;
                }
            }
            if (hype_svm_vcpu_handle_acpi_pm_timer_ioio(ctx) == 0) {
                continue;
            }

            {
                hype_svm_ioio_t io;
                /* Absorb every access to an unmodeled port (harmless), but
                 * trace each distinct (port,direction) only ONCE. Linux's
                 * io_delay writes port 0x80 after nearly every inb/outb, so
                 * rendering a line per access to the GOP framebuffer slows
                 * the boot to a crawl on real hardware; latch it like the
                 * MSR-trace flood so discovery still works without the
                 * per-access render. */
                static uint32_t seen_ports[128];
                static unsigned int seen_ports_n = 0;
                uint32_t key;
                unsigned int k;
                int already = 0;
                hype_svm_vcpu_handle_unknown_ioio(ctx, &io);
                if (io.port == 0x80u) {
                    ex_io80++; /* exit-histogram sub-bucket: io_delay port */
                }
                key = (uint32_t)io.port | (io.is_in ? 0x80000000u : 0u);
                for (k = 0; k < seen_ports_n; k++) {
                    if (seen_ports[k] == key) {
                        already = 1;
                        break;
                    }
                }
                if (!already) {
                    hype_debug_print("fw-1: unhandled port 0x%x %s size=%u rip=0x%llx -- absorbing "
                                      "(further hits on this port silenced)\n",
                                      (unsigned int)io.port, io.is_in ? "IN" : "OUT",
                                      (unsigned int)io.size_bytes, (unsigned long long)info.guest_rip);
                    if (seen_ports_n < 128u) {
                        seen_ports[seen_ports_n++] = key;
                    }
                }
            }
            continue;
        }

        if (info.reason >= HYPE_SVM_EXITCODE_EXCEPTION_BASE &&
            info.reason <= HYPE_SVM_EXITCODE_EXCEPTION_BASE + 31) {
            hype_svm_debug_state_t dbg;
            uint8_t *fault_bytes;
            hype_svm_vcpu_get_debug_state(ctx, &dbg);

            /* Core fault summary. Printed FIRST, via hype_debug_print()
             * (not hype_fatal(), which halts and would make the deeper
             * dumps below unreachable), so it survives even if a later
             * dereference itself faults.
             *
             * CRITICAL (APM Vol 2 24593 Rev 3.44 §15.12.15): for an
             * intercepted #PF the intercept is tested BEFORE CR2 is
             * written, so the save-area CR2 printed here is STALE (the
             * previous fault's value), NOT this fault's address. The
             * real faulting address is EXITINFO2 (printed below). An
             * earlier session misread cr2=0 here as a confirmed
             * NULL-pointer deref -- it was never written for this exit.
             * cr2 is kept in the dump only to show it is stale/ignored. */
            hype_debug_print("fw-1: exc vec=%llu err=0x%llx cr0=0x%llx cr3=0x%llx rip=0x%llx (cr2=0x%llx STALE)\n",
                              (unsigned long long)(info.reason - HYPE_SVM_EXITCODE_EXCEPTION_BASE),
                              (unsigned long long)info.qualification, (unsigned long long)dbg.cr0,
                              (unsigned long long)dbg.cr3, (unsigned long long)dbg.rip,
                              (unsigned long long)dbg.cr2);
            hype_debug_print("fw-1: cs_selector=0x%x cs_base=0x%llx rflags=0x%llx rsp=0x%llx\n",
                              (unsigned int)dbg.cs_selector, (unsigned long long)dbg.cs_base,
                              (unsigned long long)dbg.rflags, (unsigned long long)dbg.rsp);
            /* THE authoritative faulting address for an intercepted #PF
             * (§15.12.15: "The faulting address is saved in the
             * EXITINFO2 field"). Real-hardware finding: this reads
             * 0xFFFFFFFFFFFFFFFF, exactly equal to rip -- i.e. the guest
             * transferred control to -1 and faulted FETCHING the
             * instruction there (err=0 is consistent: the #PF error
             * code's I/D "instruction fetch" bit is only defined when
             * EFER.NXE=1 && CR4.PAE=1, §8.4.2, and this guest has NXE
             * off, so a fetch fault reports err=0). rip=-1 is therefore
             * CORRECT, not a corrupt save. The garbage -1 pointer came
             * from guest memory that reads all-1s -- see the rsp dump. */
            hype_debug_print("fw-1: FAULT ADDR (exitinfo2)=0x%llx  [rip==this => bad control transfer to it]\n",
                              (unsigned long long)dbg.exitinfo2);

            /* §15.7.2/§15.20: EXITINTINFO is written when the intercept
             * fired while the guest was delivering a PRIOR event through
             * its IDT. Real hardware: valid=0 -- so this is the PRIMARY
             * fault, not a nested-delivery fault. NRIP cross-checks rip. */
            hype_debug_print("fw-1: exitintinfo=0x%llx (valid=%u type=%u vec=%u) nrip=0x%llx\n",
                              (unsigned long long)dbg.exitintinfo,
                              (unsigned int)((dbg.exitintinfo >> 31) & 0x1ULL),
                              (unsigned int)((dbg.exitintinfo >> 8) & 0x7ULL),
                              (unsigned int)(dbg.exitintinfo & 0xFFULL), (unsigned long long)dbg.nrip);

            /* Real-hardware finding: dbg.rip itself can be an
             * implausible sentinel-looking value (observed: exactly
             * 0xFFFFFFFFFFFFFFFF, all bits set -- not a remotely
             * plausible guest address, unlike CR0/CR2/CR3 alongside it,
             * which read as completely sane values) for this specific
             * fault on real hardware, never seen under QEMU. Whatever
             * the underlying cause (still unconfirmed -- worth its own
             * follow-up investigation), unconditionally dereferencing
             * it as a host pointer is exactly what silently killed the
             * machine here before: nothing maps the very top of the
             * 64-bit address space. Same plausibility guard as the
             * stack[2] candidate-return-address check below (nonzero,
             * below 4GB -- this project's own "usable low memory"
             * convention) now applies to dbg.rip/dbg.rsp themselves,
             * so a garbage value degrades to a clear, printed "skipped"
             * notice instead of a second, unhandled fault. cs_base=0
             * (flat protected mode) here, so a plausible dbg.rip is
             * already the linear == guest-physical == host-physical
             * address (this project's NPT identity-maps ordinary RAM)
             * -- a raw host pointer dereference is otherwise safe
             * pre-ExitBootServices, same reasoning devices/pflash.h's
             * own direct-memory-access callers already rely on. */
            if (dbg.rip != 0 && dbg.rip < 0x100000000ULL) {
                fault_bytes = (uint8_t *)(uintptr_t)dbg.rip;
                hype_debug_print("fw-1: bytes at rip: %x %x %x %x %x %x %x %x %x %x %x %x\n",
                                  fault_bytes[0], fault_bytes[1], fault_bytes[2], fault_bytes[3],
                                  fault_bytes[4], fault_bytes[5], fault_bytes[6], fault_bytes[7],
                                  fault_bytes[8], fault_bytes[9], fault_bytes[10], fault_bytes[11]);
            } else {
                hype_debug_print("fw-1: rip=0x%llx is not a plausible host pointer -- skipping the "
                                  "raw-byte dump\n",
                                  (unsigned long long)dbg.rip);
            }

            /* This is the generic CopyMem() leaf primitive (confirmed
             * against fw/OVMF_CODE.fd's own raw bytes), pushed rsi/rdi
             * at entry -- the return address into whoever actually
             * called CopyMem(dest=0, ...) is 2 qwords above the current
             * RSP. Same raw-host-pointer reasoning as fault_bytes
             * above. */
            if (dbg.rsp != 0 && dbg.rsp < 0x100000000ULL) {
                uint64_t *stack = (uint64_t *)(uintptr_t)dbg.rsp;
                hype_debug_print("fw-1: rsp=0x%llx [0]=0x%llx [1]=0x%llx [2]=0x%llx [3]=0x%llx\n",
                                  (unsigned long long)dbg.rsp, (unsigned long long)stack[0],
                                  (unsigned long long)stack[1], (unsigned long long)stack[2],
                                  (unsigned long long)stack[3]);
                /* Only a plausible-looking (nonzero, low-memory)
                 * candidate return address is worth dereferencing --
                 * unlike the earlier temp-RAM fault, this stack slot
                 * isn't guaranteed to hold a return address at all, and
                 * blindly dereferencing whatever's there (confirmed:
                 * 0 last time) wildly corrupts *host* memory since this
                 * is a raw host-pointer read from our own L1 context. */
                if (stack[2] != 0 && stack[2] < 0x100000000ULL) {
                    uint8_t *caller_bytes = (uint8_t *)(uintptr_t)stack[2];
                    hype_debug_print("fw-1: bytes at [2]-8: %x %x %x %x %x %x %x %x | %x %x %x %x\n",
                                      caller_bytes[-8], caller_bytes[-7], caller_bytes[-6], caller_bytes[-5],
                                      caller_bytes[-4], caller_bytes[-3], caller_bytes[-2], caller_bytes[-1],
                                      caller_bytes[0], caller_bytes[1], caller_bytes[2], caller_bytes[3]);
                }
            } else {
                hype_debug_print("fw-1: rsp=0x%llx is not a plausible host pointer -- skipping the "
                                  "stack dump\n",
                                  (unsigned long long)dbg.rsp);
            }

            hype_fatal("fw-1: exc vec=%llu err=0x%llx cr0=0x%llx cr3=0x%llx rip=0x%llx faultaddr=0x%llx",
                       (unsigned long long)(info.reason - HYPE_SVM_EXITCODE_EXCEPTION_BASE),
                       (unsigned long long)info.qualification, (unsigned long long)dbg.cr0,
                       (unsigned long long)dbg.cr3, (unsigned long long)dbg.rip,
                       (unsigned long long)dbg.exitinfo2);
        }

        if (info.reason == HYPE_SVM_EXITCODE_HLT) {
            /* OVMF's idle wait (CpuSleep). It never HLTs during DXE/BDS
             * init, so a HLT past HYPE_FW_1_BOOTED_EXITS productive exits
             * means the firmware finished booting and is idle-waiting for
             * a timer tick / a keypress. An earlier HLT is plain
             * wait-for-interrupt: keep running so the LAPIC timer wakes
             * it (bounded by HYPE_FW_1_MAX_EXITS above). */
            if (productive_exits < HYPE_FW_1_BOOTED_EXITS) {
                continue;
            }
            /* FW-1f: OVMF is idle at "Press any key to enter the Boot
             * Manager Menu". Inject one keystroke (Enter) into the guest
             * serial RX to exercise guest console INPUT. Feed BOTH COM1
             * and COM2 -- this build's DEBUG log is on COM2, so the
             * interactive Terminal ConIn may be on either; whichever OVMF
             * actually polls consumes it. */
            if (!key_injected) {
                hype_debug_print(
                    "fw-1: OVMF idle at its prompt -- injecting Enter via PS/2 + COM1/COM2 (ConIn test)\n");
                /* PS/2 is OVMF's real ConIn here; serial RX is belt-and-
                 * suspenders in case the Terminal console is active. */
                hype_ps2_kbd_enqueue_scancode(&g_fw_1_ps2, HYPE_FW_1_KEY_ENTER_MAKE);
                hype_guest_uart_rx_enqueue(&g_fw_1_uart, (uint8_t)'\r');
                hype_guest_uart_rx_enqueue(&g_fw_1_uart2, (uint8_t)'\r');
                key_injected = 1;
                inject_chars = console_chars;
                inject_productive = productive_exits;
                inject_total = total_exits;
                continue;
            }
            /* M4-6d2: a HLT with IF=1 is an interrupt-wait (Linux idle /
             * msleep does `sti; hlt`). Because we intercept the HLT before
             * it retires, the STI interrupt-shadow that covered it never
             * clears, so a pending timer/AHCI IRQ stays blocked
             * (can_accept=0) and jiffies freeze -- the M4-6d2 plateau. When
             * an interrupt is actually deliverable now, model the wake:
             * retire the HLT (RIP past it + drop the shadow) and inject, so
             * the guest resumes after the HLT with the IRQ taken. Nothing
             * deliverable => fall through and keep re-halting (advancing
             * time) until one comes due. */
            {
                hype_svm_intr_state_t is;
                int if_set, pic_ready;
                hype_svm_vcpu_get_intr_state(ctx, &is);
                if_set = (int)((is.rflags >> 9) & 1u);
                pic_ready = (g_fw_1_pic.master.isr == 0 && g_fw_1_pic.slave.isr == 0) &&
                            (((g_fw_1_pic.master.irr & (uint8_t)~g_fw_1_pic.master.imr) != 0) ||
                             ((g_fw_1_pic.slave.irr & (uint8_t)~g_fw_1_pic.slave.imr) != 0 &&
                              (g_fw_1_pic.master.imr & (uint8_t)(1u << 2)) == 0));
                if (if_set && (is.pending_valid || pic_ready)) {
                    hype_svm_vcpu_wake_hlt(ctx); /* retire HLT + clear STI shadow */
                    if (!hype_svm_vcpu_deliver_pending_if_ready(ctx) &&
                        g_fw_1_pic.master.isr == 0 && g_fw_1_pic.slave.isr == 0) {
                        uint8_t v;
                        if (hype_pic_emu_acknowledge(&g_fw_1_pic, &v)) {
                            hype_svm_vcpu_request_interrupt(ctx, v);
                            if (v == g_fw_1_pic.master.irq_offset) {
                                pit_irqs++;
                            } else {
                                ahci_irqs++;
                            }
                        }
                    }
                    continue;
                }
            }
            /* M4-6d2b: stop once the guest has been quiescent (no
             * productive exit) for HYPE_FW_1_IDLE_GIVEUP_SECONDS of real
             * time -- it has reached a stable idle (OVMF at its prompt, or
             * a booted OS at a login/shell prompt) or genuinely hung. This
             * replaces the old "exits since the injected key" test, which
             * measured from the OVMF-prompt keystroke and so fired on a
             * booting kernel's very first idle-HLT (it legitimately idles
             * for real-time stretches on the async libata probe, long
             * after the key). Measuring wall-clock since the last
             * productive exit lets the kernel's probe/init proceed while
             * still catching a true stall. Falls back to the exit-count
             * test if TSC calibration was unavailable. */
            /* M4-6d2 DIAG: the moment we first notice the guest quiescent
             * for ~2s (well before the 10s give-up) with a PIC IRQ still
             * in service, dump its interrupt-acceptance state -- this is
             * the timer-IRQ wedge, and IF/shadow/eventinj/vintr/pending
             * tell whether it's a dead IF=0 halt or ready-but-undelivered. */
            {
                static int wedge_dumped = 0;
                if (!wedge_dumped && g_fw_1_host_tsc_hz != 0 &&
                    tb_last_tsc - last_progress_tsc >= 2ULL * g_fw_1_host_tsc_hz &&
                    (g_fw_1_pic.master.isr != 0 || g_fw_1_pic.slave.isr != 0)) {
                    hype_svm_intr_state_t is;
                    wedge_dumped = 1;
                    hype_svm_vcpu_get_intr_state(ctx, &is);
                    hype_debug_print("fw-1: M4-6d2 WEDGE: IF=%d shadow=0x%llx eventinj=0x%llx vintr=0x%llx "
                                      "can_accept=%d pending=%d/vec0x%x (master ISR=0x%x IMR=0x%x, "
                                      "slave ISR=0x%x IMR=0x%x)\n",
                                      (int)((is.rflags >> 9) & 1u), (unsigned long long)is.interrupt_shadow,
                                      (unsigned long long)is.eventinj, (unsigned long long)is.vintr,
                                      is.can_accept, is.pending_valid, (unsigned int)is.pending_vector,
                                      (unsigned int)g_fw_1_pic.master.isr, (unsigned int)g_fw_1_pic.master.imr,
                                      (unsigned int)g_fw_1_pic.slave.isr, (unsigned int)g_fw_1_pic.slave.imr);
                }
            }
            if (g_fw_1_host_tsc_hz != 0) {
                if (tb_last_tsc - last_progress_tsc >=
                    HYPE_FW_1_IDLE_GIVEUP_SECONDS * g_fw_1_host_tsc_hz) {
                    booted = 1;
                    break;
                }
            } else if (total_exits - inject_total >= HYPE_FW_1_KEY_WAIT_EXITS) {
                booted = 1;
                break;
            }
            /* M4-6d4: host-side idle wait -- the dominant efficiency lever.
             * Reaching here means the guest HLTed with IF=1 and nothing
             * deliverable yet: it is idle-waiting for its next timer edge.
             * Instead of immediately re-entering the guest (which HLTs again
             * at once -- QEMU measured 24.6M of 24.8M total exits as this
             * idle spin, each a full VMRUN world-switch at ~4.3us), busy-wait
             * on the host TSC until the nearest armed timer is actually due,
             * then re-enter ONCE. This advances real wall-clock -- and thus
             * the guest timebase on the next advance, since guest timers are
             * ticked from the real elapsed TSC -- IDENTICALLY, so there is NO
             * clock skew; it only removes the wasted world-switches. Nothing
             * else can wake an idle guest in this model: AHCI/serial/PS2
             * completions are produced synchronously by the guest's own
             * MMIO/port exits, which cannot occur while it is halted. Capped
             * at 10ms so the periodic flush, host input, and the idle-giveup
             * stay responsive; a longer idle just re-waits in 10ms steps. */
            if (g_fw_1_host_tsc_hz != 0) {
                hype_svm_intr_state_t iw;
                hype_svm_vcpu_get_intr_state(ctx, &iw);
                if (((iw.rflags >> 9) & 1u) != 0) { /* IF=1: a real interrupt-wait */
                    uint64_t ttn = 0; /* PIT ticks until the nearest armed timer edge */
                    if (g_fw_1_pit.channels[0].counter != 0u) {
                        ttn = (uint64_t)g_fw_1_pit.channels[0].counter;
                    }
                    if (g_fw_1_lapic.init_count != 0u &&
                        (g_fw_1_lapic.lvt_timer & HYPE_GUEST_LAPIC_LVT_MASKED) == 0 &&
                        g_fw_1_lapic.current_count != 0u) {
                        uint64_t lt = (uint64_t)g_fw_1_lapic.current_count;
                        if (ttn == 0u || lt < ttn) { ttn = lt; }
                    }
                    if (ttn != 0u) {
                        uint64_t wait_tsc = ttn * g_fw_1_host_tsc_hz / HYPE_PIT_HZ;
                        uint64_t cap = g_fw_1_host_tsc_hz / 100ULL; /* 10ms */
                        uint64_t start = hype_rdtsc();
                        if (wait_tsc > cap) { wait_tsc = cap; }
                        while (hype_rdtsc() - start < wait_tsc) {
                            __asm__ volatile("pause");
                        }
                        /* Exclude this deliberate idle wait from the COSTHIST
                         * loop-body accounting, so that metric keeps measuring
                         * only real per-exit work (VMRUN vs handler code), not
                         * time we intentionally spent halted. */
                        g_fw_1_prev_post_tsc = hype_rdtsc();
                    }
                }
            }
            continue;
        }

        break;
    }

    /* M4-6d4: restore the firmware's exception vectors now that the guest
     * loop is done -- the subsequent Boot Services calls (GetMemoryMap,
     * ExitBootServices) run under firmware's own handlers again. */
    fw_1_remove_exception_catcher();

    /* Flush any console text the guest emitted right before it idled. */
    fw_1_drain_uart_console(&g_fw_1_uart, &uart_filter, uart_line, &uart_line_len,
                             (unsigned int)sizeof(uart_line));
    fw_1_drain_uart_console(&g_fw_1_uart2, &uart_filter2, uart_line2, &uart_line_len2,
                             (unsigned int)sizeof(uart_line2));

    /* M4-6b diagnostic: how many guest LAPIC-timer IRQs were actually
     * delivered, and whether one is still stuck in-service (never EOI'd
     * by the guest). timer_irqs == 1 with in_service == 1 means the
     * guest took one tick and never acknowledged it -> its clockevent
     * isn't live. A large count means it is servicing the timer. */
    hype_debug_print("fw-1: M4-6b diag: LAPIC timer IRQs=%llu (in_service=%d), PIT IRQ0 IRQs=%llu, "
                      "AHCI IRQs=%llu on line %u (master ISR=0x%x IMR=0x%x, slave ISR=0x%x IMR=0x%x, "
                      "AHCI ghc=0x%x p_ie=0x%x), host_tsc_hz=%llu\n",
                      (unsigned long long)timer_irqs, g_fw_1_lapic.timer_in_service,
                      (unsigned long long)pit_irqs, (unsigned long long)ahci_irqs,
                      (unsigned int)hype_pci_get_interrupt_line(&g_fw_1_pci, HYPE_FW_1_PCI_DEV_AHCI),
                      (unsigned int)g_fw_1_pic.master.isr, (unsigned int)g_fw_1_pic.master.imr,
                      (unsigned int)g_fw_1_pic.slave.isr, (unsigned int)g_fw_1_pic.slave.imr,
                      (unsigned int)g_fw_1_ahci.ghc, (unsigned int)g_fw_1_ahci.p_ie,
                      (unsigned long long)g_fw_1_host_tsc_hz);
    {
        /* M4-6d2 DIAG: interrupt-injection path breakdown -- if the timer
         * (or AHCI) IRQ delivery wedges, defer >> window or overwrite > 0
         * shows deferred injections stuck/lost via the VINTR path. */
        unsigned long long ei = 0, df = 0, wn = 0, ov = 0;
        hype_svm_vcpu_get_int_diag(&ei, &df, &wn, &ov);
        hype_debug_print("fw-1: M4-6d2 int diag: EVENTINJ=%llu, VINTR-defer=%llu, VINTR-window=%llu, "
                          "defer-overwrite=%llu\n", ei, df, wn, ov);
    }

    /* M4-6d3 real-HW diag: characterise WHY the loop gave up. On real
     * hardware the wrapping GOP console loses scrollback, so surface the
     * decisive facts on the final frame: the guest RIP + RFLAGS.IF at the
     * idle point (IF=0 => a dead `cli; hlt`, i.e. a kernel panic/BUG, not a
     * live idle -- the timer can't wake it, which is exactly what a 10s
     * giveup with master ISR=0 looks like), plus the last, still-buffered
     * partial console line (a panic header has no trailing newline, so the
     * line-buffered drain never emitted it). */
    if (booted) {
        hype_svm_intr_state_t gs;
        hype_svm_vcpu_get_intr_state(ctx, &gs);
        if (uart_line_len > 0) {
            uart_line[uart_line_len] = '\0';
            hype_debug_print("fw-1: last ttyS0 (unterminated): %s\n", uart_line);
            uart_line_len = 0;
        }
        if (uart_line_len2 > 0) {
            uart_line2[uart_line_len2] = '\0';
            hype_debug_print("fw-1: last ttyS1 (unterminated): %s\n", uart_line2);
            uart_line_len2 = 0;
        }
        hype_debug_print("fw-1: GIVEUP CAUSE: guest_rip=0x%llx IF=%d (%s) shadow=0x%llx eventinj=0x%llx "
                          "can_accept=%d pending=%d/vec0x%x last_exit_reason=0x%llx\n",
                          (unsigned long long)info.guest_rip, (int)((gs.rflags >> 9) & 1u),
                          (((gs.rflags >> 9) & 1u) ? "live idle -- waiting on an undelivered IRQ"
                                                   : "DEAD HALT -- cli;hlt, likely a guest panic/BUG"),
                          (unsigned long long)gs.interrupt_shadow, (unsigned long long)gs.eventinj,
                          gs.can_accept, gs.pending_valid, (unsigned int)gs.pending_vector,
                          (unsigned long long)info.reason);
        /* What was waiting to be delivered when it wedged: a set IRR bit
         * (unmasked) means an IRQ was pending in the PIC but never
         * injected; PxIS/PxCI show an AHCI completion pending or a command
         * still outstanding. */
        hype_debug_print("fw-1: PEND-STATE: mIRR=0x%x mIMR=0x%x sIRR=0x%x sIMR=0x%x "
                          "ahci_p_is=0x%x ahci_p_ci=0x%x ahci_irq_pending=%d\n",
                          (unsigned int)g_fw_1_pic.master.irr, (unsigned int)g_fw_1_pic.master.imr,
                          (unsigned int)g_fw_1_pic.slave.irr, (unsigned int)g_fw_1_pic.slave.imr,
                          (unsigned int)g_fw_1_ahci.p_is, (unsigned int)g_fw_1_ahci.p_ci,
                          hype_ahci_irq_pending(&g_fw_1_ahci));
        /* The guest is live-idle waiting on a timer IRQ that stopped
         * coming. Surface the timebase + both clockevent sources so we
         * can tell a miscalibrated host_tsc_hz (PIT/LAPIC advancing at the
         * wrong rate) from a tickless guest that stopped the periodic PIT
         * and armed a one-shot LAPIC timer we're not firing. */
        hype_debug_print("fw-1: TIMER-STATE: host_tsc_hz=%llu PIT0 mode=%u reload=%u counter=%u | "
                          "LAPIC lvt_timer=0x%x(%s) init=%u cur=%u pend=%d insvc=%d\n",
                          (unsigned long long)g_fw_1_host_tsc_hz,
                          (unsigned int)g_fw_1_pit.channels[0].mode,
                          (unsigned int)g_fw_1_pit.channels[0].reload,
                          (unsigned int)g_fw_1_pit.channels[0].counter,
                          (unsigned int)g_fw_1_lapic.lvt_timer,
                          (g_fw_1_lapic.lvt_timer & HYPE_GUEST_LAPIC_LVT_MASKED)
                              ? "masked"
                              : ((g_fw_1_lapic.lvt_timer & HYPE_GUEST_LAPIC_LVT_PERIODIC) ? "periodic"
                                                                                          : "1shot"),
                          (unsigned int)g_fw_1_lapic.init_count,
                          (unsigned int)g_fw_1_lapic.current_count,
                          g_fw_1_lapic.timer_irq_pending, g_fw_1_lapic.timer_in_service);
    }

    if (booted && key_reacted) {
        hype_debug_print(
            "fw-1: real OVMF BOOTED + INTERACTIVE (FW-1g) -- full DXE/BDS (LAPIC timer, PCI/ECAM, PS/2, "
            "AHCI CD-ROM) with %llu chars of console output forwarded, and the injected PS/2 Enter was "
            "READ by the guest's keyboard driver at its interactive prompt (the guest polled 0x64, we set "
            "OBF, it read scancode 0x1C off 0x60) and REACTED with %llu productive exits + new console "
            "output -- guest ConIn confirmed end-to-end. guest_rip=0x%llx.\n",
            (unsigned long long)console_chars, (unsigned long long)(productive_exits - inject_productive),
            (unsigned long long)info.guest_rip);
        return;
    }
    if (booted) {
        hype_debug_print(
            "fw-1: real OVMF BOOTED -- full DXE/BDS (LAPIC timer, PCI/ECAM, PS/2, AHCI CD-ROM), %llu "
            "chars of console output forwarded above, idle after %llu productive VM-exits. With FW-1h's "
            "bootable CD present, BDS auto-discovers and starts it (the forwarded console shows the booted "
            "image's own output -- e.g. the UEFI Shell from the test ISO); with no bootable media it "
            "instead idles at the Boot Manager prompt. Any injected keystroke did not visibly register "
            "(that input path is FW-1g). guest_rip=0x%llx.\n",
            (unsigned long long)console_chars, (unsigned long long)productive_exits,
            (unsigned long long)info.guest_rip);
        return;
    }

    hype_fatal("fw-1: real OVMF exited the initial dispatch loop (reason=0x%llx qual=0x%llx guest_rip=0x%llx)",
               (unsigned long long)info.reason, (unsigned long long)info.qualification,
               (unsigned long long)info.guest_rip);
    }
}

/* Runs every test guest in sequence -- what actually gets dispatched
 * (inline on the BSP, or onto a pinned AP; see efi_main) so each new
 * milestone's test guest still exercises real 1:1 vCPU-to-pCPU
 * pinning (M3-2) rather than only the first one ever tested. */
/* M4-6d4 deliverable #3: prove the host can reclaim control from a guest
 * busy-waiting on PAUSE -- the mechanism behind the host-preemption fix for
 * the real-HW 40s no-exit VMRUN (a spinning guest starves its own timer
 * tick because we only regain control on a voluntary exit). Bounded spin
 * `mov cx,0xFFFF; pause; loop $-2; hlt` ALWAYS terminates via HLT, so a
 * hypervisor that ignores pause-filtering just shows the HLT with zero
 * PAUSE intercepts -- no hang. With filtering armed we expect one or more
 * EXITCODE_PAUSE before the HLT. Reuses the m2-7 below-4GB guest pages
 * (free once run_test_guest finished). SVM-only. */
static void run_pause_filter_test(const hype_vmm_ops_t *ops, hype_vmm_kind_t kind) {
    uint8_t *guest_code;
    hype_vcpu_ctx_t *ctx;
    hype_vmexit_info_t info;
    uint64_t npt_root_phys;
    unsigned long long pause_exits = 0;
    unsigned long long iters = 0;
    unsigned int i;
    static const uint8_t spin[] = {0xB9, 0xFF, 0xFF, 0xF3, 0x90, 0xE2, 0xFC, 0xF4};

    if (kind != HYPE_VMM_KIND_SVM || ops->vcpu_create == 0 || ops->vcpu_run == 0) {
        return;
    }
    if (!hype_cpu_has_pause_filter(hype_cpu_svm_feature_edx())) {
        hype_debug_print("pause-test: SKIP -- this CPU/hypervisor does not expose SVM "
                          "pause-filtering (no host-preemption path here)\n");
        return;
    }

    guest_code = (uint8_t *)(uintptr_t)g_m2_7_guest_code_phys;
    hype_guest_ram_zero(guest_code, 4096);
    hype_guest_ram_zero((void *)(uintptr_t)(g_m2_7_guest_stack_top_phys - 4096), 4096);
    for (i = 0; i < sizeof(spin); i++) {
        guest_code[i] = spin[i];
    }

    hype_npt_build_identity(g_npt_pml4, g_npt_pdpt, g_npt_pd, HYPE_NPT_MAX_GB);
    npt_root_phys = (uint64_t)(uintptr_t)g_npt_pml4;

    ctx = ops->vcpu_create(g_m2_7_guest_code_phys, g_m2_7_guest_stack_top_phys, npt_root_phys);
    if (ctx == 0) {
        hype_fatal("pause-test: vcpu_create failed");
    }
    /* count=500 => trips ~131 times over the 65535-pause spin; threshold=4096
     * >> the few-cycle gap between pauses in this tight loop, so the count
     * decrements steadily to 0 (on CPUs without PFTHRESHOLD the field is
     * ignored and it decrements every pause -- trips either way). */
    hype_svm_vcpu_enable_pause_filter(ctx, 500, 4096);

    for (;;) {
        if (ops->vcpu_run(ctx, &info) != 0) {
            hype_fatal("pause-test: VM-entry failed (reason=0x%llx)", (unsigned long long)info.reason);
        }
        if (info.reason == HYPE_SVM_EXITCODE_PAUSE) {
            pause_exits++;
            if (++iters > 1000000ULL) {
                break; /* safety bound -- never expected */
            }
            continue; /* resume the spin */
        }
        if (info.reason == HYPE_SVM_EXITCODE_HLT) {
            break; /* the bounded spin finished */
        }
        hype_fatal("pause-test: unexpected exit reason=0x%llx guest_rip=0x%llx",
                   (unsigned long long)info.reason, (unsigned long long)info.guest_rip);
    }
    hype_debug_print("pause-test: %s -- %llu PAUSE intercepts before HLT (%s)\n",
                      (pause_exits > 0) ? "PASS" : "NO-TRIP", pause_exits,
                      (pause_exits > 0)
                          ? "host reclaimed control from a spinning guest -- preemption mechanism works"
                          : "filter never fired -- nested pause-filtering not honored here");
}

static void EFIAPI run_all_test_guests(void *arg) {
    hype_test_guest_args_t *args = (hype_test_guest_args_t *)arg;
    run_test_guest(arg);
    run_m3_5_linux_shim_test(args->ops, args->kind);
    run_m4_3_pflash_mmio_test(args->ops, args->kind);
    run_m4_4_fw_cfg_test(args->ops, args->kind);
    run_m4_5_ahci_test(args->ops, args->kind);
    run_video_2_ramfb_test(args->ops, args->kind);
    run_cpumsr_test(args->ops, args->kind);
    run_int_test(args->ops, args->kind);
    run_input_1_test(args->ops, args->kind);
    run_input_2_test(args->ops, args->kind);
    run_ram_1_test(args->ops, args->kind);
    run_pci_1_test(args->ops, args->kind);
    run_pci_2_test(args->ops, args->kind);
    run_iso_2_test(args->ops, args->kind);
    run_video_3_test(args->ops, args->kind);
    run_m5_1_test(args->ops, args->kind);
    run_m5_2_test(args->ops, args->kind);
    run_pause_filter_test(args->ops, args->kind); /* M4-6d4 #3: preemption mechanism proof */
    run_fw_1_test(args->ops, args->kind);
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

    /* M4-6d4: disable the UEFI watchdog timer. Firmware arms it (5-minute
     * default) when it hands control to a boot application and force-RESETS
     * the machine if that app runs the whole period without calling
     * ExitBootServices(). Our FW-1 guest loop deliberately runs for many
     * minutes of wall-clock BEFORE ExitBootServices (the log flush needs
     * Boot Services), so the watchdog fired mid-boot -- the real-HW reset
     * at ~5 min (both a 5950x and a Zen2 laptop reset in the 250-280s wall
     * window) with NO panic and uncatchable by our exception handler,
     * because it is a firmware reset, not a CPU fault. Timeout=0 disarms
     * it. Harmless on QEMU (whose OVMF watchdog never fired in-window). */
    if (SystemTable->BootServices->SetWatchdogTimer != 0) {
        SystemTable->BootServices->SetWatchdogTimer(0, 0, 0, 0);
    }

    status = hype_memmap_get(SystemTable->BootServices, &map, &map_size, &desc_size, &map_key);
    if (status != EFI_SUCCESS) {
        hype_fatal("failed to get memory map: 0x%llx", (unsigned long long)status);
    }

    hype_memmap_dump(SystemTable, map, map_size, desc_size);
    /* RAM-1: computed here, before the map is freed, so the admission
     * check ahead of the guest-RAM allocation below is against this
     * machine's own real usable RAM, not a guess. */
    usable_ram_bytes = hype_memmap_usable_bytes(map, map_size, desc_size);
    g_usable_ram_bytes = usable_ram_bytes;
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
        /* Real-hardware perf fix (found via real-hardware FW-1
         * testing): draw into a shadow buffer in ordinary RAM, not
         * gop->Mode->FrameBufferBase directly -- the real framebuffer
         * is very likely mapped uncached, and gop_text.c's own one-
         * store-per-pixel drawing (plus a full-screen read+write on
         * every scroll) against uncached memory measured at 2-3
         * *seconds* per redrawn line on real AMD hardware (invisible
         * under QEMU's virtual GPU, which has no such caching-
         * performance cliff) -- catastrophic once dozens of test
         * guests each print several lines. hype_gop_flush() (core/
         * gop.h) pushes this shadow buffer to the real screen in one
         * Blt() call instead, typically hardware-accelerated/DMA'd by
         * the platform's own GOP driver. */
        uint64_t fb_bytes = (uint64_t)gop->Mode->Info->PixelsPerScanLine *
                             gop->Mode->Info->VerticalResolution * sizeof(unsigned int);
        uint64_t fb_pages = (fb_bytes + 0xFFFu) / 0x1000u;
        void *shadow_fb = (void *)(uintptr_t)hype_alloc_pages_any(SystemTable->BootServices, (UINTN)fb_pages);

        hype_gop_console_init(&g_gop_console, shadow_fb, gop->Mode->Info->HorizontalResolution,
                               gop->Mode->Info->VerticalResolution,
                               gop->Mode->Info->PixelsPerScanLine,
                               0xFFFFFFu, 0x000000u);
        hype_gop_console_clear(&g_gop_console);
        hype_fatal_set_gop(&g_gop_console);
        hype_fatal_set_gop_protocol(gop, (void *)gop->Mode->FrameBufferBase);
        hype_gop_print(&g_gop_console, "hype: running self-tests...\n");
        hype_gop_flush(gop, &g_gop_console, (void *)gop->Mode->FrameBufferBase);
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
        /* M4-6d4: does THIS environment expose SVM PAUSE-filtering? Decides
         * whether the preemption mechanism (#3/#4) is provable here or only on
         * real HW. QEMU/KVM nested SVM may not pass it through. */
        {
            uint32_t svm_edx = hype_cpu_svm_feature_edx();
            hype_debug_print("cpu: svm_feature_edx=0x%08x pause_filter=%d pause_threshold=%d\n",
                             svm_edx, hype_cpu_has_pause_filter(svm_edx),
                             hype_cpu_has_pause_threshold(svm_edx));
        }

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

        /*
         * FW-1: read this project's own vendored guest firmware from
         * the same ESP hype.efi was itself booted from, into one
         * ordinary, freely-allocated host-physical buffer (see
         * g_fw_1_combined_host_phys's own comment for why NOT the
         * literal top-of-4GB guest-physical address) -- VARS.fd first,
         * CODE.fd immediately after, matching their real relative
         * guest-physical layout.
         */
        {
            EFI_FILE_PROTOCOL *root = 0;
            EFI_STATUS fw_status;
            uint64_t content_size, mapped_size, content_start;

            fw_status = hype_file_locate_root(ImageHandle, SystemTable->BootServices, &root);
            if (fw_status != EFI_SUCCESS) {
                hype_fatal("fw-1: hype_file_locate_root failed: 0x%llx", (unsigned long long)fw_status);
            }

            fw_status = hype_file_get_size(root, SystemTable->BootServices,
                                            (CHAR16 *)L"\\EFI\\hype\\OVMF_CODE.fd", &g_fw_1_code_size);
            if (fw_status != EFI_SUCCESS) {
                hype_fatal("fw-1: hype_file_get_size(OVMF_CODE.fd) failed: 0x%llx",
                           (unsigned long long)fw_status);
            }
            fw_status = hype_file_get_size(root, SystemTable->BootServices,
                                            (CHAR16 *)L"\\EFI\\hype\\OVMF_VARS.fd", &g_fw_1_vars_size);
            if (fw_status != EFI_SUCCESS) {
                hype_fatal("fw-1: hype_file_get_size(OVMF_VARS.fd) failed: 0x%llx",
                           (unsigned long long)fw_status);
            }

            /* hype_npt_map_range() requires a whole-2MB-multiple size
             * -- round up rather than assume this vendored build's own
             * file sizes happen to add up to one exactly (they
             * currently do, but a rebuilt OVMF might not). Any padding
             * goes at the START of the allocated/mapped region (the
             * lowest addresses) -- real file content is always placed
             * flush against the END, so guest-physical 4GB - 16 (the
             * reset vector) keeps landing on CODE's own real last
             * bytes regardless of padding. */
            content_size = g_fw_1_code_size + g_fw_1_vars_size;
            mapped_size = (content_size + HYPE_PAGING_2MB - 1) & ~(HYPE_PAGING_2MB - 1);
            g_fw_1_combined_size = mapped_size;

            g_fw_1_combined_host_phys =
                hype_alloc_pages_any_2mb_aligned(SystemTable->BootServices, mapped_size);
            hype_guest_ram_zero((void *)(uintptr_t)g_fw_1_combined_host_phys, mapped_size);

            content_start = g_fw_1_combined_host_phys + (mapped_size - content_size);
            fw_status = hype_file_read_into(root, (CHAR16 *)L"\\EFI\\hype\\OVMF_VARS.fd",
                                             (void *)(uintptr_t)content_start, g_fw_1_vars_size);
            if (fw_status != EFI_SUCCESS) {
                hype_fatal("fw-1: hype_file_read_into(OVMF_VARS.fd) failed: 0x%llx",
                           (unsigned long long)fw_status);
            }
            fw_status = hype_file_read_into(root, (CHAR16 *)L"\\EFI\\hype\\OVMF_CODE.fd",
                                             (void *)(uintptr_t)(content_start + g_fw_1_vars_size),
                                             g_fw_1_code_size);
            if (fw_status != EFI_SUCCESS) {
                hype_fatal("fw-1: hype_file_read_into(OVMF_CODE.fd) failed: 0x%llx",
                           (unsigned long long)fw_status);
            }

            hype_debug_print(
                "fw-1: OVMF_VARS.fd+OVMF_CODE.fd (%llu bytes content, %llu bytes mapped) loaded at "
                "host-physical 0x%llx\n",
                (unsigned long long)content_size, (unsigned long long)mapped_size,
                (unsigned long long)g_fw_1_combined_host_phys);
        }

        /*
         * FW-1a: allocate + zero the guest's low RAM as a real,
         * contiguous host buffer (NPT-mapped at guest-physical
         * [0, HYPE_FW_1_GUEST_RAM_BYTES) in run_fw_1_test), on the BSP
         * before MP dispatch -- same ordering/reasoning as the firmware
         * buffer above. Zeroed so OVMF's reset-vector page-table build
         * at guest-physical 0x800000 and its SEC/PEI temp RAM start from
         * clean memory. */
        g_fw_1_ram_host_phys =
            hype_alloc_pages_any_2mb_aligned(SystemTable->BootServices, HYPE_FW_1_GUEST_RAM_BYTES);
        hype_guest_ram_zero((void *)(uintptr_t)g_fw_1_ram_host_phys, HYPE_FW_1_GUEST_RAM_BYTES);
        hype_debug_print("fw-1: guest RAM %llu MiB backed at host-physical 0x%llx\n",
                          (unsigned long long)(HYPE_FW_1_GUEST_RAM_BYTES / (1024ULL * 1024ULL)),
                          (unsigned long long)g_fw_1_ram_host_phys);

        /*
         * ISO-1: reads a real installer ISO (\iso\test.iso -- a real
         * ISO9660 image the Makefile's own `run` target copies onto the
         * ESP, per task.md's own "does not need M5" scoping) from the
         * same ESP hype.efi was booted from, reusing FW-1's own
         * core/file_io.h (already generic, not OVMF-specific). Verifies
         * both the read succeeded at the file's own real size (not just
         * a fixed/guessed buffer) and that a real ISO9660 Primary Volume
         * Descriptor's "CD001" standard identifier
         * (ECMA-119 SS7.1.1/7.1.2, always at byte offset 32769 -- the
         * 2nd byte of the 17th 2048-byte sector) is genuinely present
         * in what was read back -- proof this is real ISO content, not
         * garbage/a short read that happened to return success.
         */
        {
            EFI_FILE_PROTOCOL *root = 0;
            EFI_STATUS iso_status;
            UINT64 iso_size;
            uint64_t iso_host_phys;
            UINTN iso_pages;
            const uint8_t *iso_bytes;

            iso_status = hype_file_locate_root(ImageHandle, SystemTable->BootServices, &root);
            if (iso_status != EFI_SUCCESS) {
                hype_fatal("iso-1: hype_file_locate_root failed: 0x%llx", (unsigned long long)iso_status);
            }

            iso_status = hype_file_get_size(root, SystemTable->BootServices, (CHAR16 *)L"\\iso\\test.iso",
                                             &iso_size);
            if (iso_status != EFI_SUCCESS) {
                hype_fatal("iso-1: hype_file_get_size(test.iso) failed: 0x%llx",
                           (unsigned long long)iso_status);
            }
            if (iso_size < 32769 + 5) {
                hype_fatal("iso-1: test.iso is too small to be a real ISO9660 image (%llu bytes)",
                           (unsigned long long)iso_size);
            }

            iso_pages = (UINTN)((iso_size + 4095ULL) / 4096ULL);
            iso_host_phys = hype_alloc_pages_any(SystemTable->BootServices, iso_pages);

            iso_status =
                hype_file_read_into(root, (CHAR16 *)L"\\iso\\test.iso", (void *)(uintptr_t)iso_host_phys,
                                     iso_size);
            if (iso_status != EFI_SUCCESS) {
                hype_fatal("iso-1: hype_file_read_into(test.iso) failed: 0x%llx",
                           (unsigned long long)iso_status);
            }

            iso_bytes = (const uint8_t *)(uintptr_t)iso_host_phys;
            if (iso_bytes[32769] != 'C' || iso_bytes[32770] != 'D' || iso_bytes[32771] != '0' ||
                iso_bytes[32772] != '0' || iso_bytes[32773] != '1') {
                hype_fatal("iso-1: test.iso is missing the ISO9660 \"CD001\" standard identifier");
            }

            hype_debug_print(
                "iso-1: read a real %llu-byte ISO9660 image from \\iso\\test.iso, \"CD001\" "
                "identifier verified at offset 32769\n",
                (unsigned long long)iso_size);

            g_iso_host_phys = iso_host_phys;
            g_iso_size = iso_size;
        }

        /* M4-6b1: calibrate the real host TSC frequency once, while Boot
         * Services (and its Stall) are still available, so the FW-1 guest
         * can drive its PIT/LAPIC timebase from real elapsed TSC. Stall
         * busy-waits the given microseconds against the platform timer;
         * 20 ms is long enough to average out its granularity. TSC is
         * invariant across cores on any SVM-capable CPU, so a value taken
         * on the BSP here is valid on the pinned AP the guest runs on. */
        {
            /* core/efi_types.h leaves Stall as an untyped void* -- cast
             * to its real signature: EFI_STATUS (EFIAPI *)(UINTN us). */
            EFI_STATUS(EFIAPI * stall_fn)(UINTN) =
                (EFI_STATUS(EFIAPI *)(UINTN))SystemTable->BootServices->Stall;
            uint64_t t0 = hype_rdtsc();
            stall_fn(20000);
            g_fw_1_host_tsc_hz = (hype_rdtsc() - t0) * 50ULL; /* *(1e6/20000) */
            hype_debug_print("fw-1: host TSC calibrated at %llu Hz (%llu MHz)\n",
                              (unsigned long long)g_fw_1_host_tsc_hz,
                              (unsigned long long)(g_fw_1_host_tsc_hz / 1000000ULL));
        }

        /* Cache the boot-volume root + register the crash-time log flush,
         * here on the BSP where Boot Services file I/O is proven (the ISO
         * was just read from this same volume). Lets the FW-1 loop (which
         * may run on a pinned AP) and hype_fatal() flush \hype-log.txt
         * without a first-time locate from a non-BSP context. */
        fw_1_log_init(ImageHandle, SystemTable->BootServices);

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

    /* Real-hardware debugging (serial-less): flush the captured console
     * log to a file on the volume hype.efi was loaded from, while Boot
     * Services file I/O is still available. The tester reads the
     * complete, exact log off the USB stick instead of photographing a
     * wrapping framebuffer. Best-effort -- a read-only or non-FAT boot
     * volume just prints a status line and boot continues. Everything
     * printed up to here (including the FW-1 guest console and the
     * giveup diagnostics) is in the buffer; this trailing status line
     * is not (it reports the write that just happened). */
    fw_1_flush_log(); /* final flush via the BSP-cached root (fw_1_log_init) */
    hype_debug_print("hype: console log (%u bytes%s) -> \\hype-log.txt on the boot volume: %s\n",
                      hype_logbuf_len(), hype_logbuf_truncated() ? ", TRUNCATED" : "",
                      (!g_fw_1_log_disabled && g_fw_1_log_root != 0)
                          ? "written"
                          : "unavailable (read-only or non-FAT volume?)");

    /* Real-hardware debugging: a hang with this as the last serial
     * line means ExitBootServices() itself never returned control --
     * a real firmware quirk QEMU/OVMF's own implementation might not
     * reproduce. */
    hype_debug_print("about to call ExitBootServices...\n");
    status = hype_exit_boot_services(ImageHandle, SystemTable->BootServices);
    if (status != EFI_SUCCESS) {
        hype_fatal("ExitBootServices failed: 0x%llx", (unsigned long long)status);
    }
    /* Blt() (hype_gop_flush()'s own preferred path) is a Boot-Services-
     * era protocol call, unsafe to use from here on -- clear the
     * registered GOP protocol handle so every subsequent
     * hype_debug_print()/hype_fatal() falls back to a direct memcpy
     * into the real framebuffer instead (still valid indefinitely; see
     * core/gop.h's own hype_gop_flush() doc comment). The real
     * framebuffer address itself, already registered above, is left
     * untouched. */
    hype_fatal_set_gop_protocol(0, hype_fatal_get_real_fb());
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
    /* Firmware can place the GOP framebuffer BAR in high MMIO space far
     * above the low identity map (e.g. 0x4000000000 = 256GB on an Intel
     * i5-13420H). The old code assumed the framebuffer was "just memory,
     * identity-mapped by our own paging" -- true only when it sits under
     * HYPE_PAGING_MAX_GB. When it doesn't, the CR3 load below unmaps the
     * console and the very next print faults (with no IDT yet loaded) ->
     * hard hang. Map the framebuffer's own physical range before the
     * switch so the console survives it. */
    if (have_gop && gop->Mode != 0) {
        uint64_t fb_base = (uint64_t)gop->Mode->FrameBufferBase;
        uint64_t fb_size = (uint64_t)gop->Mode->FrameBufferSize;
        if (fb_size != 0 &&
            fb_base + fb_size > (uint64_t)HYPE_PAGING_MAX_GB * HYPE_PAGING_1GB) {
            unsigned int mapped = hype_paging_map_region_2mb(g_pdpt, g_fb_pd, fb_base, fb_size);
            hype_debug_print("paging: framebuffer BAR 0x%llx+0x%llx is above the %uGB map -- "
                              "mapped %u extra GB slot(s)\n",
                              (unsigned long long)fb_base, (unsigned long long)fb_size,
                              HYPE_PAGING_MAX_GB, mapped);
            if (mapped == 0) {
                hype_debug_print("paging: WARNING framebuffer BAR out of PML4[0] range -- "
                                  "console will go dark after CR3 load\n");
            }
        }
    }
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
        /* Post-ExitBootServices: Blt() is no longer safe (gop=0 here
         * falls back to hype_gop_flush()'s own direct-memcpy path). */
        hype_gop_flush(0, &g_gop_console, hype_fatal_get_real_fb());
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
    /* INPUT-3: host-level keyboard ownership -- must happen here too,
     * strictly before sti, same "nothing can fire on a vector we
     * haven't wired up yet" discipline this whole block already
     * follows for the timer. Does not call hype_pic_remap_and_mask_all()
     * again (see ps2_host.h's own top comment for why: IRQ1 already
     * landed on HYPE_TIMER_VECTOR+1 from the timer's own remap above). */
    hype_host_kbd_init();
    hype_serial_print("about to enable interrupts (sti)...\n");
    hype_sti();
    hype_serial_print("interrupts enabled -- waiting for timer ticks\n");

    {
        /* INPUT-4: leader-chord recognition (plan.md §6b). No dashboard
         * or VM-switching consumer exists yet (that's M8's job) -- this
         * just drains whatever hype_host_kbd_isr() (INPUT-3) buffered,
         * feeds it through the pure hype_chord_feed_scancode() decoder,
         * and reports any recognized action via serial, as the only
         * currently-observable proof the chord was decoded correctly. */
        uint64_t target = hype_timer_get_ticks() + 1000; /* ~1s at 1000Hz */
        hype_chord_state_t chord_state;
        hype_chord_state_reset(&chord_state);
        while (hype_timer_get_ticks() < target) {
            uint8_t scancode;
            hype_wait_for_interrupt();
            while (hype_host_kbd_poll_scancode(&scancode)) {
                hype_chord_result_t result = hype_chord_feed_scancode(&chord_state, scancode);
                if (result.action != HYPE_CHORD_ACTION_NONE) {
                    hype_serial_print("leader-chord: action=%d vm_index=%u\n",
                                       (int)result.action, (unsigned)result.vm_index);
                }
            }
        }
    }
    hype_serial_print("timer: %llu ticks (PIT @ 1000Hz)\n",
                       (unsigned long long)hype_timer_get_ticks());

    hype_halt_forever();
}
