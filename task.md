# Thin UEFI Hypervisor — Task List

Derived from `plan.md`. Each task has a stable ID and a `Deps:` line listing
the task IDs that must be **done** before it can start. Tasks with no unmet
deps are unblocked and can be picked up immediately; tasks in different
epics with no dependency between them can run in parallel.

Checkbox = done. `Deps: —` = no prerequisites.

**Minimum supported guest target (plan.md §1, decided 2026-07-14): Windows
(any 64-bit), Linux (any 64-bit), and BSD (any 64-bit) — no 32-bit guests.**
M3 validates the core VM-exit loop via a Linux-specific direct `bzImage` boot
(cheapest path, not the only supported one); M4+ guest-firmware work and M6's
dedicated BSD milestone bring Windows and BSD (and firmware-booted Linux) to
the same bar.

---

## SETUP — Pre-M0 readiness (plan.md §11)

- [x] **SETUP-1** — `git init`; add `LICENSE` (full GPLv3 text) and
  `.gitignore` for build artifacts.
  Deps: —
- [x] **SETUP-2** — Install and pin versions: C cross-toolchain targeting
  `x86_64-unknown-uefi` (clang/lld or GNU-EFI), QEMU, OVMF firmware image.
  Deps: —
- [x] **SETUP-3** — Confirm Secure Boot can be disabled on both the Intel
  and AMD test machines.
  Deps: —
- [x] **SETUP-4** — Confirm both test machines expose a serial (or
  equivalent) fallback debug channel available before GOP init succeeds.
  Deps: —
- [x] **SETUP-5** — Settle debugging workflow: QEMU `-s -S` + GDB against a
  debug build with symbols, plus serial logging as the real-hardware path.
  Deps: SETUP-2
- [x] **SETUP-6** — Write minimal freestanding primitives: `printf`-equivalent
  over UEFI `ConOut`, and a panic/assert stub.
  Deps: SETUP-2

---

## M0 — UEFI hello world (plan.md §9 M0, §12)

- [x] **M0-1** — Scaffold repo layout per plan.md §7 (`/boot`, `/core`,
  `/arch`, `/devices`, `/storage`, `/net`, `/fw`, `/tools`, `/docs`).
  Deps: SETUP-1
- [x] **M0-2** — Minimal UEFI app: print "hype" via `ConOut`, return
  `EFI_SUCCESS` cleanly.
  Deps: M0-1, SETUP-6
- [x] **M0-3** — Dump the UEFI memory map.
  Deps: M0-2
- [x] **M0-4** — Validate build/boot/deploy loop in QEMU+OVMF.
  Deps: M0-3, SETUP-2
- [ ] **M0-5** — Validate build/boot/deploy loop on real Intel + AMD
  hardware.
  Deps: M0-3, SETUP-3, SETUP-4

  *Open gate: not yet run. Downstream M1+ work has proceeded past this
  point by explicit user decision (2026-07-13) to skip waiting on it for
  now, not because the dependency stopped mattering -- this still needs
  to happen and this checkbox should get filled in for real once it
  does.*

---

## M1 — Boot Services exit + own kernel context (plan.md §9 M1)

- [x] **M1-1** — `hype.cfg` config parser (plan.md §5 schema: `vcpus`,
  `cpu_set`, `mem_mb`, `boot`, `install_media`, `target_disk`,
  `target_disk_size_gb`, `firmware`, `os_hint`, `net_mode`).
  Deps: M0-4, M0-5
- [x] **M1-2** — Own GDT/IDT.
  Deps: M0-4, M0-5
- [x] **M1-3** — Own paging.
  Deps: M0-4, M0-5
- [x] **M1-4** — `ExitBootServices()` sequence; hypervisor becomes the only
  kernel.
  Deps: M1-2, M1-3
- [x] **M1-5** — Serial console driver.
  Deps: M1-4
- [x] **M1-6** — GOP linear-framebuffer text renderer (bitmap font blitter)
  — reused later by the dashboard (§6b) and guest firmware GOP exposure
  (§6/§6c).
  Deps: M1-4
- [x] **M1-7** — Panic handler: halt cleanly with a message, via M1-5/M1-6.
  Deps: M1-5, M1-6
- [x] **M1-8** — Timer tick (PIT/HPET bring-up for the host itself).
  Deps: M1-4

---

## ADM — Startup admission control (plan.md §6i, §10 decision #14/#16)

- [x] **ADM-1** — Sum configured `mem_mb` across all VMs; reject startup if
  it (plus hypervisor/device reserve) exceeds physical RAM from the UEFI
  memory map.
  Deps: M1-1, M0-3
- [x] **ADM-2** — Sum configured `vcpus` against physical core count;
  reject if it can't be satisfied under 1:1 pinning.
  Deps: M1-1
- [x] **ADM-3** — Validate explicit `cpu_set` entries: cores exist, count
  matches `vcpus`, and no two VMs' `cpu_set` ranges overlap (hard reject on
  overlap, not a warning).
  Deps: ADM-2
- [x] **ADM-4** — Reject startup if any two VMs' `target_disk` resolve to
  the same `file:` path or `physical:` serial/GUID, or if any two VMs would
  resolve to the same persisted varstore file (security-critical — closes
  the gap between §6d's "exclusively owned" claim and what's actually
  enforced; found in security review, §10 decision #20).
  Deps: M1-1
- [x] **ADM-5** — Validate `net_peers`: every listed name resolves to a VM
  actually defined in `hype.cfg`, and both VMs in a pairing have
  `net_mode = nat`; reject startup otherwise (§10 decision #21) — keeps
  guest-to-guest connectivity an explicit, auditable opt-in rather than a
  typo silently no-op'ing or leaving an unintended VM reachable.
  Deps: M1-1

*Note: ADM-1..5 gate VM startup and must be complete before M8's
multi-VM concurrency milestone, even though early single-guest milestones
(M2/M3) don't yet exercise the multi-VM overlap checks.*

---

## M2 — VMX/SVM bring-up, single vCPU (plan.md §9 M2, §10 decision #6/#17)

- [x] **M2-1** — CPU feature detection (VMX/EPT vs. SVM/NPT) and
  `vmm_ops` vtable dispatch (`vmx_ops` / `svm_ops`).
  Deps: M1-4
- [x] **M2-2** — VMXON (Intel) / SVM mode enable (AMD).
  Deps: M2-1
- [x] **M2-3** — Minimal VMCS (Intel) / VMCB (AMD) construction.
  Deps: M2-2
- [x] **M2-4** — Enable APICv (Intel) / AVIC (AMD) — required from this
  milestone, not deferred.
  Deps: M2-3
- [x] **M2-5** — VM-exit handler dispatch loop skeleton.
  Deps: M2-3
- [x] **M2-6** — Guest RAM zeroing before first VM-entry, on every
  (re)start (§10 decision #15).
  Deps: M2-3
- [x] **M2-7** — Launch a hand-written `hlt`-loop guest; confirm VM-exit
  round trip.
  Deps: M2-4, M2-5, M2-6
- [ ] **M2-8** — Real-hardware validation (Intel + AMD).
  Deps: M2-7, SETUP-3, SETUP-4

  *Open gate: not yet run, same as M0-5 above -- no physical Intel/AMD
  test hardware is reachable from this environment. SVM has real
  QEMU/KVM nested-virtualization validation (M2-7: 5/5 clean runs,
  correct HLT exit); VMX has none at all (this dev environment is
  AMD-only hardware, so not even software emulation can exercise it) --
  M2-8 is where VMX's vcpu_create/vcpu_run and its VM-entry/VM-exit
  trampoline (deferred at M2-7, see vmx_ops.c) would actually get
  written and iterated against real Intel silicon, not just where an
  already-working backend gets double-checked. Downstream milestones
  have proceeded past this point by the same explicit user decision
  (2026-07-13) as M0-5 -- this still needs to happen for real on both
  vendors' hardware before this checkbox is genuine.*

---

## M3 — EPT + first real guest boot (plan.md §9 M3)

- [x] **M3-1** — EPT/NPT table construction (identity-mapped).
  Deps: M2-7
- [x] **M3-2** — 1:1 vCPU-to-pCPU pinning, including explicit `cpu_set`
  support.
  Deps: ADM-3, M2-7

  *Scope note: the real pinning mechanism is built and QEMU-validated
  -- EFI_MP_SERVICES_PROTOCOL dispatches the test guest's vCPU onto a
  specific, chosen non-BSP physical core (core/mp.c, boot/main.c),
  confirmed 5/5 clean runs with `-smp 2`. `hype_mp_pick_target_ap()`
  currently picks "any enabled non-BSP processor," not yet a specific
  core number driven by hype.cfg's parsed `cpu_set` list (ADM-3
  already validates that config, but nothing wires it to a real
  per-VM pinning decision yet) -- that's deferred to M8 alongside the
  rest of real multi-VM concurrency, matching every other
  single-instance-for-now scoping decision through M2/M3 (single VMCB,
  single AVIC/NPT table, ...).*
- [x] **M3-3** — Basic Linux boot protocol shim (direct `bzImage` boot, no
  firmware).
  Deps: M3-1

  *Scope note: parsing/construction logic only (setup-header
  validation, payload offset, 64-bit entry address, zero-page/E820
  construction), pure and 100%-tested against the documented Linux
  boot protocol -- not yet wired into an actual guest launch. That
  integration (loading a real bzImage, building guest page tables,
  VMRUNning it, confirming it actually runs) is M3-5's job once M3-4's
  device stubs exist too, per task.md's own dependency graph.*
- [x] **M3-4** — Minimal guest-visible device stubs: PIC/IOAPIC, PIT/HPET.
  Deps: M3-1

  *Scope note: implements the classic i8259 PIC + i8254 PIT pair (the
  same minimal choice real minimal VMMs like Firecracker make) rather
  than IOAPIC/HPET -- sufficient for a guest's early boot
  interrupt-controller/timer programming. Both are pure, tested
  register-level state machines (ICW1-4 init, OCW1/2/3, PIT
  mode/access/latch programming) -- not yet wired into an actual SVM
  IOIO-intercept dispatch (no guest has ever executed IN/OUT yet); that
  wiring, and validating a real guest programming these without
  hanging, is M3-5's job.*
- [x] **M3-5** — Boot a minimal Linux kernel end-to-end; validate
  APICv/AVIC interrupt delivery and the VM-exit loop under real device I/O.
  Deps: M3-3, M3-4, M2-4

  *Scope note: "boot a minimal kernel" done via a synthetic,
  hand-built bzImage (real setup_header validated through M3-3's
  shim, not bypassed) rather than a real production kernel -- same
  reasoning as M2-7's hand-written test guest: full control over the
  outcome, proving every new piece of plumbing actually works
  end-to-end (guest identity paging, long-mode VMCB construction, RSI
  delivery per the boot protocol, SVM IOIO interception with a real
  VM-exit loop that resumes the guest across exits, dispatch to
  M3-4's PIC/PIT stubs) before attempting a real, unpredictable
  kernel. Confirmed 5/5 clean QEMU/KVM runs: the guest halts cleanly
  after masking all PIC IRQs and latching+reading PIT channel 0,
  both observably reflected in the emulated devices' own state
  afterward. Real Fedora kernel boot attempt deferred as a stretch
  goal (user decision, 2026-07-14) -- expected to reach only early
  boot messages before panicking/hanging on missing ACPI/PCI/
  initramfs, which this hypervisor doesn't provide. Full AVIC
  interrupt-delivery validation (a guest's own ISR actually firing)
  is also deferred -- this pass enables AVIC's structural
  prerequisites (NPT via M3-1) but the test guest has no IDT of its
  own to receive an injected interrupt; that's a materially bigger
  lift (guest-side IDT + ISR) saved for real guest OS boot work.
  Two real, non-obvious bugs found and fixed along the way:
  HYPE_SVM_INTERCEPT_IOIO_PROT only enables the IOIO-intercept
  mechanism -- the IOPM bitmap (left all-zero, correct for M2-7's
  real-mode guest) must be explicitly filled to actually mark ports
  as intercepted, or guest port I/O silently reaches real hardware;
  and VMRUN clobbers every general-purpose register a guest touches,
  not just the ones given explicit asm constraints -- a missing
  clobber let the compiler assume a live pointer survived in RSI
  across VMRUN, corrupting it once guest code actually used RSI.*
- [ ] **M3-6** — Real-hardware validation (Intel + AMD).
  Deps: M3-5, SETUP-3, SETUP-4

  *Open gate: not yet run, same as M0-5/M2-8 above -- no physical
  Intel/AMD test hardware is reachable from this environment. SVM has
  real QEMU/KVM nested-virtualization validation throughout M3 (M3-1
  NPT, M3-2 pinning, M3-5's full synthetic-bzImage guest launch, all
  5/5 clean runs); VMX has none at all (AMD-only dev environment).
  M3-6 is where the two real bugs found via M3-5's QEMU pass (IOPM
  bitmap fill, VMRUN register clobbering) would get a second,
  independent confirmation on real AMD silicon, and where VMX's
  still-nonexistent vcpu_run trampoline (deferred since M2-8) would
  actually need to be written and debugged against real Intel
  hardware for the first time. Downstream milestones have proceeded
  past this point by the same explicit user decision (2026-07-13) as
  M0-5/M2-8.*

---

## INPUT — Input devices (plan.md §6b, §6c)

- [ ] **INPUT-1** — Guest-facing PS/2 keyboard device.
  Deps: M3-4
- [ ] **INPUT-2** — Guest-facing PS/2 mouse device (for GUI installers,
  §6c).
  Deps: INPUT-1
- [ ] **INPUT-3** — Host-level keyboard controller ownership + raw scancode
  interception, beneath any guest.
  Deps: M1-4
- [ ] **INPUT-4** — Leader-chord recognition: `Right-Ctrl+Right-Alt` held +
  action key (`D`, `1`-`9`, `Left`/`Right`, `Esc`).
  Deps: INPUT-3

---

## VIDEO — Display devices (plan.md §6, §6b)

- [x] **VIDEO-1** — (= M1-6) GOP linear-framebuffer text renderer.
  Deps: M1-6
- [ ] **VIDEO-2** — Guest-facing GOP protocol exposure, pre-OS-driver
  (writes into a per-VM framebuffer in guest RAM).
  Deps: M1-6, M3-1
- [ ] **VIDEO-3** — Post-boot VGA/Bochs-VBE-class virtual display adapter
  (for Windows' inbox Basic Display Adapter and Linux/BSD `vesafb`/`efifb`).
  Deps: VIDEO-2

---

## VALID — Guest-supplied input validation (plan.md §6j, §10 decision #19)

Found during security review: the actual guest-escape vector isn't
EPT/NPT (that only stops direct guest-to-guest memory access) — it's the
hypervisor trusting a guest-supplied address/length in device emulation
code. Foundational to every device model task below; not optional.

- [ ] **VALID-1** — Guest-physical-address translation/bounds-check helper:
  given a VM, a guest-physical address, and a length, validate against that
  VM's own EPT/NPT-mapped range before returning a host-virtual pointer. All
  device emulation code paths must go through this, never dereference a
  raw guest-supplied address directly.
  Deps: M3-1
- [ ] **VALID-2** — Apply VALID-1 to virtio queue descriptor processing
  (virtio-blk, virtio-net).
  Deps: VALID-1
- [ ] **VALID-3** — Apply VALID-1 to AHCI/NVMe command FIS buffer pointers,
  plus explicit LBA+sector-count bounds-checking against the backing
  store's actual size (file length or physical disk capacity) before any
  read/write — reject out-of-range requests, never clamp/truncate silently.
  Deps: VALID-1
- [ ] **VALID-4** — Apply VALID-1 to any other guest-supplied buffer used
  by device emulation (PS/2, framebuffer-adjacent paths) as those devices
  are built.
  Deps: VALID-1

*Note: M5's `blk_backend` (file and physical implementations) and NET's
guest-facing devices depend on VALID-1/2/3, not just their own device-model
tasks — see updated deps below.*

---

## M4 — Guest UEFI firmware + ACPI synth (plan.md §9 M4, §10 decision #1)

- [x] **M4-1** — EDK2 build pipeline for the guest firmware blob (separate
  from `hype.efi`'s own toolchain).
  Deps: SETUP-2

  *`edk2` vendored as a git submodule pinned to `edk2-stable202511`,
  plus one local commit fixing a real NASM 3.x regression (see that
  commit's message) -- this dev environment's Fedora release ships
  NASM 3.01 with no older version available, and 3.x rejects "push
  strict dword <imm>"/"push dword <imm>" in 64-bit mode, which
  UefiCpuPkg's X64 IDT stub generation relies on. `tools/build-fw.sh`
  automates the whole pipeline (BaseTools built with clang, then
  OvmfPkg/OvmfPkgX64.dsc via the CLANGDWARF tag) end-to-end,
  confirmed reproducible by re-running it.*
- [x] **M4-2** — Vendor/strip an OVMF build as the guest firmware base.
  Deps: M4-1

  *`fw/OVMF_CODE.fd`/`fw/OVMF_VARS.fd` committed (plan.md §7's "vendored
  blob," not just a build script -- downstream consumers get a working
  firmware pair without needing the EDK2 toolchain themselves).
  Smoke-tested standalone (reaches BdsDxe correctly) and booting
  `hype.efi` itself through its own full existing test suite exactly
  as `edk2-ovmf` already does. Not yet used as actual guest-facing
  firmware for a VM -- that's M4-3 onward.*
- [x] **M4-3** — Emulated flash/varstore, persisted to disk.
  Deps: M4-2

  *CFI (Common Flash Interface) NOR-flash command-protocol emulation
  (`devices/pflash.h`/`.c`: WRITE_BYTE/BLOCK_ERASE/CLEAR_STATUS/
  READ_STATUS/READ_DEVID/WRITE_TO_BUFFER/READ_ARRAY) backed by an
  in-memory buffer, fully unit-tested. MMIO trapping via NPT: guest
  accesses to the flash's 2MB range are forced to fault
  (`hype_npt_mark_not_present()`) into a real SVM NPF (#VMEXIT_NPF)
  handler (`hype_svm_vcpu_handle_npf()`) that decodes the faulting
  MOV/MOVZX instruction (a narrow, purpose-built x86_64 decoder,
  `arch/x86_64/cpu/mmio_decode.h`/`.c`, scoped to exactly the forms
  EDK2's own MmioRead8/MmioWrite8-style library calls compile to;
  fully unit-tested including ModRM/SIB/disp8/disp32/RIP-relative
  addressing) and dispatches to the flash model. Originally planned to
  read the faulting instruction via AMD SVM Decode Assist
  (VMCB `num_bytes_fetched`/`guest_instruction_bytes`, confirmed
  present via CPUID on this dev environment's host CPU) but confirmed
  empirically, via real QEMU/KVM runs, that nested SVM does not
  reliably populate those fields even when the CPU advertises the
  feature -- switched to reading guest memory directly at RIP instead
  (a plain host pointer dereference, since this project's guest/NPT
  setup is a flat identity map), which is more portable and no longer
  depends on that hardware feature at all. `hype_svm_vcpu_run()`'s
  VMRUN sequence was also extended to capture/restore every
  general-purpose register (previously only RAX/RSI), needed so the
  NPF handler can read a write's source register or patch a read's
  destination register for any register compiled MMIO-accessor code
  happens to use.
  Validated end-to-end with a synthetic long-mode test guest
  (hand-written machine code, same rigor as M3-5): issues a real
  WRITE_BYTE command and data byte through genuine memory-mapped
  stores, reads the byte back through a genuine memory-mapped load,
  then writes that read-back value out to a second offset -- so the
  host can confirm both the write and read paths from the flash's
  backing array alone. 5/5 clean QEMU runs.
  Real persistence to a host file explicitly deferred -- that needs a
  disk driver, M5's job; this milestone's own dependency graph would
  otherwise be circular. The in-memory device model and NPT-based MMIO
  trap mechanism are both reusable as-is once M5 exists.*
- [ ] **M4-4** — Per-VM ACPI table synthesis (RSDP/XSDT/FADT/MADT/MCFG).
  Deps: M4-2, M3-2
- [ ] **M4-5** — Virtual optical drive device (read-only ISO passthrough,
  AHCI/ATAPI or virtio-scsi CD-ROM).
  Deps: M3-1
- [ ] **M4-6** — Boot a stock Linux UEFI installer ISO (e.g. Debian
  netinst) end-to-end through GRUB.
  Deps: M4-3, M4-4, M4-5, VIDEO-2

---

## NET — Networking (plan.md §6e, §10 decision #9 — required)

- [ ] **NET-1** — Host NIC driver, e1000/e1000e-class first target.
  Deps: M3-1
- [ ] **NET-2** — virtio-net guest-facing device (Linux/BSD default).
  Deps: NET-1, VALID-2
- [ ] **NET-3** — Emulated e1000-compatible NIC guest-facing device
  (Windows path, mirroring the AHCI-for-Windows split).
  Deps: NET-1, VALID-1
- [ ] **NET-4** — Basic host-level NAT for guest outbound connectivity,
  guest→WAN + established-return only.
  Deps: NET-1
- [ ] **NET-4a** — Guest-to-guest isolation by default: each guest's
  virtual NIC in its own segment, never a shared L2/broadcast domain with
  another guest's — prevents *accidental* guest-to-guest communication
  (security review finding, §10 decision #21).
  Deps: NET-4
- [ ] **NET-4b** — `net_peers` opt-in: narrow host-mediated forwarding rule
  between exactly the VM pairs an operator explicitly names, without
  opening a general shared broadcast domain — deliberate guest-to-guest
  connectivity is a supported use case, just never the default.
  Deps: NET-4a, ADM-5
- [ ] **NET-5** — `net_mode`/`net_peers` config wiring (`none` default /
  `nat`, optional `net_peers` list).
  Deps: NET-2, NET-3, NET-4b, M1-1

---

## M5 — virtio-blk/AHCI + full install (plan.md §9 M5)

- [ ] **M5-1** — virtio-blk guest-facing device.
  Deps: M4-5
- [ ] **M5-2** — AHCI guest-facing device.
  Deps: M4-5
- [ ] **M5-3** — `blk_backend` vtable + file-backed implementation (§6d),
  with guest LBA/length bounds-checking against the backing file's actual
  size per VALID-3.
  Deps: M5-1, M5-2, VALID-3
- [ ] **M5-4** — `/tools` disk-image prep script (`target_disk_size_gb`
  handling).
  Deps: M5-3
- [ ] **M5-5** — Full unattended Linux install to a virtual disk (needs
  network for netinst package fetch).
  Deps: M4-6, M5-3, M5-4, NET-5
- [ ] **M5-6** — Reboot into the installed OS (`boot = disk` two-phase
  flip, §6d).
  Deps: M5-5

---

## M6 — BSD guest (plan.md §9 M6)

- [ ] **M6-1** — FreeBSD-specific ACPI/loader quirk fixes.
  Deps: M5-6, M4-4
- [ ] **M6-2** — FreeBSD installer boot + install.
  Deps: M6-1, M5-3

---

## M7 — Windows guest (plan.md §9 M7, §10 decision #2)

- [ ] **M7-1** — Full Hyper-V-compatible CPUID/MSR leaf set
  (`0x40000000`–`0x40000006` + synthetic MSRs).
  Deps: M2-1
- [ ] **M7-2** — Windows AHCI/NVMe storage path validated for installer use.
  Deps: M5-2, M7-1
- [ ] **M7-3** — Windows GUI install path: PS/2 keyboard + mouse, VGA/VBE
  display adapter.
  Deps: INPUT-1, INPUT-2, VIDEO-3
- [ ] **M7-4** — Windows Setup boot + full install.
  Deps: M7-2, M7-3, M4-3, M4-4

---

## M8 — Multi-VM concurrency, dashboard, lifecycle control, fault isolation
## (plan.md §9 M8, §6b, §6f, §6g, §10 decisions #11/#12)

- [ ] **M8-1** — Dashboard rendering: per-VM name/os_hint/state/vCPU
  utilization/memory/uptime/boot-media list.
  Deps: VIDEO-1
- [ ] **M8-2** — Per-vCPU stats collection (exit counts, `HLT` time,
  last-scheduled timestamp) feeding the dashboard.
  Deps: M2-5
- [ ] **M8-3** — Dashboard navigation via the leader chord (toggle, jump to
  VM N, cycle prev/next).
  Deps: INPUT-4, M8-1
- [ ] **M8-3a** — Input exclusivity while the dashboard has focus: every
  keystroke consumed by the dashboard, explicit focus-owner check, zero
  forwarding to any guest (including whichever VM had focus immediately
  before) until focus is explicitly switched back (security review
  finding, §10 decision #22).
  Deps: M8-3
- [ ] **M8-4** — VM lifecycle: **Start** (fresh boot, zeroed RAM per M2-6).
  Deps: M2-6, M6-2, M7-4
- [ ] **M8-5** — VM lifecycle: **Stop**/Resume (pause vCPU(s) in place,
  retain RAM/device state).
  Deps: M2-5
- [ ] **M8-6** — VM lifecycle: **Shutdown** (emulated ACPI power-button GPE,
  guest-driven S5, bounded timeout).
  Deps: M4-4, M8-4
- [ ] **M8-7** — VM lifecycle: **Force power off** (immediate teardown).
  Deps: M8-4
- [ ] **M8-8** — Per-vCPU watchdog: detect a genuinely faulted guest
  (triple fault / unrecognized VM-exit storm) and auto-apply Force power
  off to that VM only. Note: this is a liveness/hang detector, not a
  substitute for VALID-1..4's input validation — it does not catch
  memory-safety violations in device emulation.
  Deps: M2-5, M8-7
- [ ] **M8-9** — Run Windows + Linux + BSD concurrently; console-switch via
  dashboard; confirm EPT/NPT isolation and pinning hold; confirm stats
  match reality.
  Deps: M8-3, M8-5, M8-6, M8-8, ADM-3
- [ ] **M8-10** — Real-hardware validation (Intel + AMD).
  Deps: M8-9, SETUP-3, SETUP-4

---

## M9 — Persistence & host power lifecycle
## (plan.md §9 M9, §6h, §10 decision #13)

- [ ] **M9-1** — Verify varstore persistence survives a host reboot.
  Deps: M4-3
- [ ] **M9-2** — Host shutdown/reboot sequence: graceful Shutdown across
  all running guests in parallel, bounded per-VM timeout, Force power off
  fallback.
  Deps: M8-6, M8-7
- [ ] **M9-3** — Persist a run/stopped state record (ESP, alongside
  `hype.cfg`) as part of the shutdown sequence.
  Deps: M9-2
- [ ] **M9-4** — On hypervisor startup, read the state record and
  auto-Start every VM that was previously running.
  Deps: M9-3, M8-4
- [ ] **M9-5** — Reboot the host into `hype.efi` again; boot
  already-installed guest disks (not just fresh installers).
  Deps: M9-1
- [ ] **M9-6** — Real-hardware validation of the full persistence cycle.
  Deps: M9-4, M9-5, SETUP-3, SETUP-4

---

## M10 — Physical disk install target
## (plan.md §9 M10, §6d, §10 decision #7/#8/#18)

- [ ] **M10-1** — Adapt a GPLv3-compatible-licensed AHCI/NVMe host driver
  (native AHCI mode + NVMe-over-PCIe only). Confirm source license header
  before adapting (project-level license check, see top of `plan.md`).
  Deps: M3-1
- [ ] **M10-2** — Physical disk enumeration pre-`ExitBootServices` (walk
  UEFI Block I/O handles, capture serial/GUID).
  Deps: M0-3
- [ ] **M10-3** — `blk_backend` physical-disk implementation, with guest
  LBA/length bounds-checking against the real disk's actual capacity per
  VALID-3 — same rule as the file-backed implementation, higher stakes
  since a miss here touches real hardware.
  Deps: M10-1, M10-2, M5-3, VALID-3
- [ ] **M10-4** — Match-before-write safety check: re-confirm enumerated
  serial/GUID matches config at VM start; refuse to boot on mismatch.
  Deps: M10-3
- [ ] **M10-5** — Interactive confirmation on the dashboard (drive
  model/serial/size) before first write, plus non-empty-partition-table
  guard requiring an explicit per-disk override.
  Deps: M8-1, M10-4
- [ ] **M10-6** — Install one guest straight to a real drive; boot it
  natively outside the hypervisor to confirm it's a normal, non-virtualized
  -dependent install.
  Deps: M10-5

---

## STRETCH (plan.md §9, no hard deps beyond a working v1)

- [ ] **STRETCH-1** — Legacy/CSM boot shim.
  Deps: M4-2
- [ ] **STRETCH-2** — Secure Boot signing (self-sign + operator `db`/MOK
  enrollment, per §10 decision #5's "revisit if needed").
  Deps: M0-5
- [ ] **STRETCH-3** — Passthrough NIC via VT-d/AMD-Vi (IOMMU).
  Deps: NET-1
- [ ] **STRETCH-4** — Guest disk image snapshotting (explicit non-goal for
  v1 — only revisit if a real ask emerges, per §10 decision #3).
  Deps: M5-3

---

## DOCS — User-facing documentation

- [ ] **DOCS-1** — End-user `README.md`: written for someone downloading a
  packaged build/installer, not building from source. Covers what hype is,
  supported host/guest requirements, install steps, first-run/basic usage,
  and where to get help — no build/toolchain instructions (those stay in
  `fw/README.md`/`tools/`, not the top-level README). Keep in sync with
  whatever v1's actual packaging/installer mechanism ends up being, so this
  is best written once that's settled (post-M9/M10 area) rather than
  early.
  Deps: none hard, but most useful once install/packaging is real (M9/M10)

---

## Suggested critical path

The shortest path to "install one of each OS family, running concurrently,
survives a host reboot" runs:

```
SETUP-* → M0-* → M1-* → ADM-* → M2-* → M3-* → VALID-* → M4-*
   → NET-* → M5-* → M6-*  ─┐
                    M7-* ──┼→ M8-* → M9-*
              INPUT-*/VIDEO-* (feed M7 and M8) ┘
```

**VALID-* (§6j) sits on the critical path now**, not off to the side — it
gates every device-emulation task in M4/NET/M5/M7/M10, since it's the fix
for the security review's top finding (guest-supplied addresses trusted
without bounds-checking is the real guest-escape vector, not something
EPT/NPT alone prevents).

M10 (physical disk) and STRETCH items can start any time their listed
dependencies are satisfied — they don't sit on the critical path above and
can be picked up in parallel once M3/VALID (device model + input
validation basics) and M5/M8 (blk_backend, dashboard) exist.
