# Thin UEFI Hypervisor — Task List

Derived from `plan.md`. Each task has a stable ID and a `Deps:` line listing
the task IDs that must be **done** before it can start. Tasks with no unmet
deps are unblocked and can be picked up immediately; tasks in different
epics with no dependency between them can run in parallel.

Checkbox = done. `Deps: —` = no prerequisites.

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
- [ ] **M3-2** — 1:1 vCPU-to-pCPU pinning, including explicit `cpu_set`
  support.
  Deps: ADM-3, M2-7
- [ ] **M3-3** — Basic Linux boot protocol shim (direct `bzImage` boot, no
  firmware).
  Deps: M3-1
- [ ] **M3-4** — Minimal guest-visible device stubs: PIC/IOAPIC, PIT/HPET.
  Deps: M3-1
- [ ] **M3-5** — Boot a minimal Linux kernel end-to-end; validate
  APICv/AVIC interrupt delivery and the VM-exit loop under real device I/O.
  Deps: M3-3, M3-4, M2-4
- [ ] **M3-6** — Real-hardware validation (Intel + AMD).
  Deps: M3-5, SETUP-3, SETUP-4

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

- [ ] **M4-1** — EDK2 build pipeline for the guest firmware blob (separate
  from `hype.efi`'s own toolchain).
  Deps: SETUP-2
- [ ] **M4-2** — Vendor/strip an OVMF build as the guest firmware base.
  Deps: M4-1
- [ ] **M4-3** — Emulated flash/varstore, persisted to disk.
  Deps: M4-2
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
