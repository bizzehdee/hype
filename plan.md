# Thin UEFI Type-1 Hypervisor — Project Plan

**License**: this project is **GPLv3**. Any third-party code adapted into it
(see §10 decision on the AHCI/NVMe driver, and §6h on the network driver)
must be GPLv3-*compatible* — it does not need to itself be GPLv3 (MIT/BSD,
Apache-2.0-in-some-configurations, or "GPLv2-or-later" are all fine to pull
in and relicense under our GPLv3 terms) — but plain **GPLv2-only** code
(no "or later" clause) is *not* GPLv3-compatible per the FSF's own
compatibility guidance, and must be avoided as a source to adapt from even
though it's tempting (e.g. large parts of the Linux kernel are GPLv2-only).
Check the specific file/module's license header, not just the project's
overall stated license, before adapting anything.

## 1. Goal

Build a **thin bare-metal (type-1) hypervisor** that boots as a UEFI application
(really, a UEFI OS-loader-style payload that never returns to the firmware boot
manager), takes ownership of the CPU/VMX or SVM extensions, and hosts multiple
guest VMs — each capable of running a fresh OS installer (Windows, Linux, or
BSD) or a previously installed OS. It is not a general-purpose hypervisor like
Xen/KVM/Hyper-V — it is deliberately minimal: just enough virtualization,
device emulation, and boot plumbing to install and run guest operating systems,
with no aspirations to advanced scheduling, live migration, or nested
virtualization in v1.

**Minimum supported guest target: Windows (any 64-bit version), Linux (any
64-bit distribution), and BSD (any 64-bit variant) — no 32-bit guests of any
kind. See §10 decision #23.**

Non-goals (v1): live migration, GPU passthrough/SR-IOV, nested virtualization,
nice management UI, nested-paging tricks beyond basic EPT/NPT, VM memory
snapshotting (live RAM state), VirtIO ballooning.

**Out of scope entirely**: making `hype.efi` the machine's default UEFI boot
target (writing `BootOrder`/`Boot####` NVRAM variables, or otherwise
configuring the firmware to launch it automatically on power-on) is a
deployment/operational task performed by the operator using standard
firmware tools (the platform's boot menu, `efibootmgr`-equivalent, etc.),
not something this project needs to implement. This plan assumes `hype.efi`
is already what the firmware boots — everything from that point on is in
scope.

## 2. High-Level Architecture

```
 Firmware (UEFI) -> hype.efi (our loader/hypervisor image)
                       |
                       |-- Phase 0: UEFI application context — deliberately
                       |   minimal: ONLY what Boot Services alone can do
                       |   (§10 decision 37)
                       |     - Locate GOP; record the framebuffer base, size
                       |       and mode (a firmware protocol; there is no
                       |       post-ExitBootServices equivalent)
                       |     - GetMemoryMap; enumerate CPU features
                       |       (VMX/EPT or SVM/NPT)
                       |     - Calibrate the host TSC against Boot Services'
                       |       Stall() — hype's own xHCI driver needs a real
                       |       timebase before it can enumerate (§10
                       |       decision 37)
                       |     - Enumerate UEFI Block I/O handles to capture
                       |       each drive's serial/GUID identity (§6d) — free
                       |       here, gone afterwards
                       |     - Reserve host-physical memory with
                       |       AllocatePages: ONE large guest-RAM pool sized
                       |       from the memory map (NOT from hype.cfg), plus
                       |       the few address-constrained pages firmware
                       |       alone can place (<1MB AP trampoline, <4GB AP
                       |       page-table root). This is the one deliberate
                       |       exception to "move it post-EBS" — see §10
                       |       decision 37
                       |     - Call ExitBootServices
                       |
                       |-- Phase 1: take ownership + hype's own I/O
                       |     - Become the only kernel: own paging, IDT/GDT,
                       |       APIC, timers
                       |     - Bring up hype's OWN host storage stack: PCI
                       |       enumeration, AHCI + NVMe + xHCI/USB-MSC, and
                       |       the FAT32/exFAT/ext/NTFS/ISO9660 readers
                       |     - Locate and mount hype's own boot volume,
                       |       whichever bus it is on (USB, SATA or NVMe —
                       |       one shared locator, §10 decision 38)
                       |     - Read hype.cfg through that stack and parse it;
                       |       run §6i admission control
                       |     - Read the guest firmware images and the §6h
                       |       run-state record through that stack
                       |     - Carve each VM's guest RAM out of the Phase 0
                       |       pool at the parsed sizes, and zero every page
                       |       before that VM's first instruction runs (§6f)
                       |       — on every (re)start, not just the first
                       |     - Resolve each VM's install media and stream it
                       |       from its host device (§6d) — never preloaded
                       |
                       |-- Phase 2: VMM core (thin hypervisor proper)
                       |     - Enable VMX/SVM on all cores (VMXON / mode set)
                       |     - Build VMCS/VMCB per vCPU
                       |     - EPT/NPT tables per guest (identity or sparse)
                       |     - VM-exit handler dispatch loop
                       |     - Minimal virtual chipset: PIC/IOAPIC, PIT/HPET,
                       |       serial (debug), virtio-blk, virtio-net,
                       |       a synthetic UEFI variable/firmware surface for
                       |       guest UEFI boot (OVMF-lite) OR legacy BIOS/CSM
                       |       shim if targeting BIOS installers
                       |
                       |-- Phase 3: Guest boot
                             - Guest sees either UEFI (own minimal guest
                               firmware/GOP+fw-vars, à la cut-down OVMF) or
                               legacy INT13/VGA BIOS shim
                             - Installer media (ISO/VHD/raw img) presented as
                               a virtio-blk / AHCI device
                             - Windows/Linux/BSD installers run unmodified
                             - Multiple guests run concurrently, each with
                               1..N vCPUs on dedicated or shared cores (§3),
                               own EPT/NPT address space
```

The boundary between Phase 0 and Phase 1 is drawn as early as the hardware
allows, not as late as convenience allows (§10 decision 37). Phase 0 uses a
Boot Services facility only where no post-`ExitBootServices` equivalent
exists: locating GOP, reading the memory map, capturing Block I/O drive
identity, and `AllocatePages`. **hype does not read its own files through
firmware.** `hype.cfg`, the guest firmware images, the run-state record and
all install media are read in Phase 1 by hype's own host storage stack —
the same AHCI/NVMe/USB drivers and FAT32/exFAT/ext/NTFS/ISO9660 readers
that already serve guest-facing disk I/O. Which bus hype booted from does
not matter; all three are equally valid boot media, exactly as `media_disk`
already treats them for media reads (§6d, §10 decision 25).

Everything after `ExitBootServices()` is our own tiny kernel: no dependency
on firmware runtime except UEFI Runtime Services we explicitly keep mapped
(time, variable services optionally, reset).

## 3. Why "thin"

- No general device driver model — a small, fixed board of virtual devices.
- No general-purpose process/thread scheduler. hype schedules **vCPUs and
  nothing else**, across two operator-chosen tiers (§10 decision 39):
    - **Dedicated** (the default) — exclusive 1:1 vCPU-to-pCPU pinning. No
      scheduler on the path at all. The operator can pin a VM to an explicit
      **subset** of host cores (§5 `cpu_set`) rather than the hypervisor
      auto-assigning whichever are free. Exclusivity — no two dedicated VMs'
      pinned sets overlap — is a hard invariant of *this tier*, checked at
      startup (§6i).
    - **Shared** — a pool of cores that several **sCPUs** ("scheduled CPUs")
      are time-sliced onto, so a host can run far more of them than it has
      cores. An sCPU is a *share* of scheduler time rather than hardware, and
      a guest with N sCPUs sees exactly N CPUs for compatibility (§10
      decision 47). A VM opts in per-VM.
  On both tiers the unit of **execution** is a hardware thread and the unit
  of **allocation** is a physical core (§10 decision 40), and **a vCPU IS a
  physical core** (decision 47). A core is granted whole: every SMT sibling
  thread of a granted core may run a thread of the VM that owns it, so a
  dedicated VM given one 2-thread core gets **one vCPU that the guest sees as
  two logical CPUs** — SMT is a bonus the guest was never promised, and on a
  non-SMT host the same config costs the same core and yields one. A sibling
  thread is never left idle to satisfy an isolation rule; what the isolation
  rule forbids is two *distrusting* owners occupying one physical core at the
  same time (§6g).
  Fault isolation (§6g) is preserved on both tiers, but by different
  mechanisms: by construction on the dedicated tier, and by **mandatory
  preemption** on the shared one. That difference is the whole substance of
  the scheduler work — see §6g and §10 decision 39.
- No filesystem in the hypervisor beyond what's needed to read hype's own
  files and the guest images off a host volume — but that reader is the
  **primary** path, not a contingency. hype's own FAT32/exFAT/ext/NTFS/
  ISO9660 drivers, running post-`ExitBootServices` over hype's own AHCI/
  NVMe/USB block drivers, are how hype reads `hype.cfg`, the guest firmware
  images, the run-state record and every installer ISO (§2, §10 decision
  37). UEFI's Simple File System protocol is not used for any of them. The
  scope limit still holds: read plus the narrow in-place/append writes §6h
  and §10 decisions 24/30 already define, not a general filesystem.
- Guest firmware is the minimal amount needed to satisfy each OS installer's
  expectations (see §6).

## 4. Target CPU support

- **Phase A (primary): Intel VT-x + EPT** (VMX, unrestricted guest, EPT,
  **APICv/posted interrupts required from the start** — see §10 decision on
  interrupt/IPI model).
- **Phase B: AMD-V + NPT** (SVM, NPT, **AVIC required from the start**, same
  rationale).
- Abstract behind a `struct vmm_ops` vtable (`vmx_ops` / `svm_ops`) chosen at
  init based on CPUID, so the VM-exit dispatch loop and device model are
  vendor-agnostic.
- Require: VT-x/AMD-V + EPT/NPT + unrestricted guest (Intel) or equivalent
  (AMD always has this). No support for ancient CPUs lacking EPT/NPT — shadow
  paging is out of scope for a "thin" hypervisor.
- IOMMU (VT-d/AMD-Vi) required only if we ever add passthrough — out of scope
  for v1, but reserve the design space.

## 5. Multi-guest / multi-OS-family model

- Config file (`hype.cfg`, simple key=value / TOML-ish, parsed by our own
  <!-- Full extensible schema: docs/hype-cfg-spec.md (CONFIG-1 #220) -->
  tiny parser — no libc) enumerates guest definitions:
  ```
  [vm.win11]
  vcpus = 4                 ; PHYSICAL CORES, not threads (decision 47); on an SMT
                            ; host the guest sees 4 x threads_per_core logical
                            ; CPUs -- SMT is a bonus, not extra vCPUs
  cpu_set = 4-7             ; explicit host PHYSICAL core subset to pin to
                            ; (optional; auto-assigned from whatever's free if
                            ; omitted). One entry per vCPU, since a vCPU IS a
                            ; core; a core is granted whole, so its threads all
                            ; go to this VM
  cpu_mode = dedicated      ; dedicated | shared (§3, §10 decision 39).
                            ; default dedicated = today's exclusive 1:1 pinning
  isolation_group = payroll ; VMs naming the same group may share cores and SMT
                            ; siblings with each other. Default: the VM's own
                            ; name, i.e. shares with nobody (default-deny)
  mem_mb = 8192
  boot = installer        ; installer | disk | kernel (§10 decision 45)
                          ; kernel = <path> and cmdline = <string> apply to that mode
  install_media = \EFI\hype\win11.iso
  target_disk = file:\hype\disks\win11.img   ; file:<path> | physical:<serial-or-guid>
  target_disk_size_gb = 128                  ; only used when creating a new file: target
  firmware = uefi          ; uefi | legacy
  os_hint = windows         ; windows | linux | bsd | none
  net_mode = nat            ; none | nat (§6e), default none

  [vm.debian]
  vcpus = 2
  mem_mb = 4096
  boot = installer
  install_media = \EFI\hype\debian-netinst.iso
  target_disk = physical:SN-WDC-1234567890    ; installs straight to a real drive
  firmware = uefi
  os_hint = linux
  net_mode = nat
  net_peers = freebsd       ; explicit opt-in: debian <-> freebsd may talk
                            ; directly; every other VM stays isolated from both

  [vm.freebsd]
  vcpus = 2
  mem_mb = 4096
  boot = installer
  install_media = \EFI\hype\FreeBSD.iso
  target_disk = file:\hype\disks\freebsd.img
  target_disk_size_gb = 64
  firmware = uefi
  os_hint = bsd
  net_mode = nat            ; net_peers not listed here — debian's listing
                            ; above already makes the pairing bidirectional
  ```
- `os_hint` only tunes small compat knobs (e.g. Windows wants ACPI + HPET +
  a handful of specific PCI IDs recognized by inbox drivers; Linux/BSD are
  more tolerant and work well with pure virtio). It never becomes a fork in
  the hypervisor core — the same VM-exit loop and device model serve all.
- Any number of VMs of any OS family may be defined; total vCPUs/RAM bounded
  by host resources. Console switching between VMs via a simple text menu
  (serial + GOP framebuffer) — think "which VM's console am I looking at,"
  not true window management.

## 6. Guest firmware / boot path per OS family

This is the trickiest part of "install Windows/Linux/BSD" and deserves
explicit design:

- **UEFI guests (default)**: ship a minimal guest firmware blob, conceptually
  a stripped OVMF: PI/DXE-less, just enough to expose
  - EFI Boot Services subset needed by installers pre-ExitBootServices
    (bootx64.efi loaders call very little: Simple File System, Block IO,
    Graphics Output, memory map, variable services stubs).
  - EFI Runtime Services variable store backed by an emulated flash region
    the hypervisor persists to disk (needed by Windows setup, GRUB, and
    BSD's loader — all expect to set/read boot variables).
  - ACPI tables (RSDP/XSDT/FADT/MADT/MCFG) synthesized per-VM at boot,
    reflecting the virtual device set and vCPU count.
  This lets Windows Setup, GRUB/systemd-boot, and the FreeBSD/OpenBSD EFI
  loader all boot unmodified, since they all just want a standards-compliant
  UEFI environment plus ACPI.
- **Legacy/BIOS guests (optional, later)**: a small SeaBIOS-like CSM shim,
  only if we want to support installers that insist on legacy boot. Treat as
  stretch goal; UEFI-only is enough for a first release given all three OS
  families support UEFI installers today.
- **Storage presented to guest**: **three** guest-facing front-ends, and the
  operator chooses per disk — `virtio-blk` (Linux/BSD inbox drivers, fastest,
  but Windows needs the virtio-win driver injected — see §6a), `ahci-sata`
  (Windows-friendly, no drivers at install time, more emulation complexity),
  and `nvme` (also inbox on Windows, and what a modern machine looks like).
  `os_hint` supplies only the **default** (`windows` → `ahci-sata`, otherwise
  `virtio-blk`); an explicit `bus =` in `hype.cfg` always wins. A VM may have
  **any number of disks, each with its own bus** — see §6d and §10 decision
  26. `docs/hype-cfg-spec.md` is the authority for the config surface itself
  and already specifies these keys.
- **Network**: virtio-net, optional; not required for offline installs.
- **Video**: two phases. Pre-OS-driver, the hypervisor registers QEMU's
  `etc/ramfb` fw_cfg file for each VM. The vendored OVMF `QemuRamfbDxe`
  driver installs GOP and allocates a linear framebuffer in that VM's own
  RAM. The hypervisor validates the complete guest-written surface range
  against that VM's GPA map before copying focused pixels to the host display.
  Post-boot, we present a plain
  VGA/Bochs-VBE-class virtual display adapter (the interface QEMU's
  `stdvga`/`bochs-display` and VirtualBox use) so Windows' inbox Microsoft
  Basic Display Adapter driver "just works" with zero driver install, and
  Linux/BSD's `vesafb`/`efifb`-class drivers do too. Either way the guest is
  only ever writing pixels into its own per-VM framebuffer, oblivious to
  whether it currently has focus (§6b) — no 3D/GPU accel in v1.

### 6a. Windows-specific accommodation

Windows Setup's boot-critical drivers only know ACPI/PCI + AHCI/NVMe (inbox)
and standard PS/2 or USB HID. Two supported paths:
1. **AHCI + emulated NVMe path (no injected drivers)** — simplest, works
   out of the box, slightly slower emulation.
2. **virtio-blk + slipstreamed virtio-win driver** into the installer's
   boot.wim (via offline WIM injection tooling, done once by the operator,
   not by the hypervisor at runtime) — faster, but requires prep.
v1 default: (1), because "thin" and "just works" beats raw performance.

**CPUID/MSR compatibility (see §10 decision)**: implement the **full
Hyper-V-compatible CPUID leaf set** (`0x40000000`–`0x40000006`, vendor
signature, interface signature, feature/recommendation leaves) and the
associated synthetic MSRs, matching what KVM's `hyperv-compat` mode exposes
— not just the minimal vendor-string leaf. Chosen deliberately up front
rather than deferred, so Windows gets its fast paths (paravirt TLB flush,
reference TSC page, etc.) from the first Windows milestone (M7) instead of
needing a follow-up pass later.

## 6b. Local status dashboard

Since the hypervisor owns the physical GOP framebuffer and keyboard directly
(no host OS in the way), it can provide a built-in host-level status screen —
distinct from any guest's own console:

- **Trigger — leader chord**: `Right-Ctrl+Right-Alt` is the reserved
  **leader** combo, held down; a further keypress while it's held selects
  the specific action, so it's one chord with several bound actions rather
  than one hotkey per action:
  - `Right-Ctrl+Right-Alt+D` — toggle the host dashboard view.
  - `Right-Ctrl+Right-Alt+1`..`9` — jump directly to the Nth VM defined in
    `hype.cfg` (matches the dashboard's listed order).
  - `Right-Ctrl+Right-Alt+Left` / `Right` — cycle focus to the
    previous/next VM without going through the dashboard.
  - `Right-Ctrl+Right-Alt+Esc` — return focus to the dashboard from
    whichever VM currently has it (equivalent shortcut to `+D` from inside
    a VM).
  All of this is captured by the hypervisor's own keyboard handler *before*
  any scancode is routed to whichever guest currently owns the display —
  a true global hook in the sense that matters: the hypervisor owns the
  real keyboard controller beneath every guest, so the leader chord and its
  action keys are consumed at that layer and never delivered to the guest
  OS at all. Windows (or Linux, or a guest's firmware) has no way to
  intercept, suppress, or rebind any of it, because as far as the guest is
  concerned those keystrokes never occurred. Switching focus to another VM
  (or the dashboard) never pauses the VM losing focus — it keeps
  running/installing in the background.
- **Content per VM**: name, `os_hint`, state (installing / running / halted /
  crashed), vCPU count and rough utilization (fraction of time not in `HLT`
  since last sample), assigned memory and high-level usage if easily
  observable (EPT/NPT working-set approximation — best-effort, not exact),
  uptime, boot media in use.
- **Data source**: the per-vCPU stats the VM-exit dispatch loop already
  needs for scheduling/debugging (exit counts, `HLT` time, last-scheduled
  timestamp) — the dashboard just renders state that already exists, it
  doesn't add new instrumentation surface.
- **Rendering** (updated 2026-07-14 — v1 scope, not just a stopgap): the
  dashboard itself should read as **corporate-yet-modern**, not the raw
  bitmap-font text console used for the hypervisor's own boot/setup
  messages (§9 M1) — that renderer stays for early boot logging (it has
  to; nothing richer exists that early), but the dashboard is a distinct,
  later-stage UI layer built with an actual embedded GUI toolkit rather
  than hand-rolled text/pixel blitting. Leading candidate: **LVGL**
  (Light and Versatile Graphics Library) — MIT-licensed, C, designed for
  exactly this kind of bare-metal/no-OS/no-libc target with a raw linear
  framebuffer as its only display backend requirement, no guest
  involvement, no dependency on any guest's video state. Confirm LVGL's
  actual portability to this project's freestanding toolchain (no libc,
  custom allocator, no filesystem) before committing to it over
  hand-rolling; if it doesn't fit cleanly, evaluate alternatives with the
  same bare-metal-friendly shape rather than falling back to a plain text
  console by default. This decision needs revisiting when M8-1 is
  actually scoped/started, not decided in the abstract now.
- **Navigation**: arrow keys / number keys to select a VM from the list and
  switch the framebuffer + keyboard focus to that guest's own console;
  another hotkey press returns to the dashboard. **Input exclusivity while
  the dashboard has focus**: every keystroke is consumed by the dashboard
  itself and *none* are forwarded to any guest (not even the VM that had
  focus immediately before) until focus is explicitly switched back to a
  specific guest. This must be an explicit focus-owner check in the
  keyboard-routing code, not "forward to whichever VM last had focus by
  default" — the latter would let dashboard-navigation keystrokes leak into
  a backgrounded guest's virtual PS/2 input stream.
- **Operator terminal** (added 2026-08-17, TERM-9/#485): alongside the
  rendered per-VM consoles, the dashboard layer owns a host-level command
  line for operations that a list-and-hotkey UI expresses poorly. Its
  command set is the authoritative operator surface for changing the
  machine's configuration at runtime:
  - `create` — interactive VM-creation wizard (§6f Create; TERM-10).
  - `mkdisk` — create a qcow2 backing image, preallocated or thin, on a
    chosen host disk's existing filesystem (§6d, §10 decision 42;
    TERM-11).
  - `attach` / `detach` — bind or unbind a device (USB, physical disk,
    SATA) to a VM (§10 decision 41; TERM-12/TERM-13).
  - `set` — edit a VM's configuration, applying immediately what can be
    applied and queueing what needs a reboot, saying which is which
    (TERM-14).
  - `delete` — remove a VM with a two-step confirmation (§6f Delete;
    TERM-15).
  Every mutation these commands make is written back to `hype.cfg`
  (the unknown-key-retaining write-back contract of
  `docs/hype-cfg-spec.md`, #220/#221, is what makes that lossless), so
  the runtime set and the configured set never drift.
- Explicitly **local-only for v1** — no serial or network exposure. This
  keeps the feature inside the existing console-ownership model instead of
  adding a network stack or serial protocol to the trusted hypervisor core.
  Revisit serial/remote access as a stretch goal only if a real headless-host
  use case shows up.
  **Scope of this bullet, clarified (decision #36):** it means the *dashboard*
  is not remotely administrable — no listening service in the hypervisor for
  anything outside it to connect to. It is not a ban on hype owning a NIC:
  §6e's host NIC drivers and NAT forwarding plane are separately required
  (decision #9) and do not conflict with this. The rule that follows from
  both is a listener, not a driver: hype never accepts an inbound connection.

## 6c. Interactive / GUI installs (non-headless OSes)

Some installers can't run headless — Windows Setup's GUI, most Linux desktop
"live" installers, GhostBSD, etc. all expect a display and pointer, not just
a serial/text console. This works through the same console-ownership model
as §6b, with one addition:

- **Input devices**: emulate a PS/2 keyboard (already needed) **and a PS/2
  mouse** in the device model. PS/2 over USB HID/xHCI is the deliberate
  choice — every mainstream installer has inbox PS/2 mouse support, so it
  covers graphical installers without adding a USB host controller emulation
  to the device model just for pointer input.
- **Focus model**: exactly one guest owns the physical display + keyboard +
  mouse at a time — whichever VM the operator last switched to via the
  dashboard hotkey (§6b). Switching away doesn't pause the VM; it just stops
  routing input/output to it, so a guest mid-file-copy keeps running in the
  background while you interact with another VM's installer.
- **Practical implication for running many installs at once**: you can only
  be hands-on with one GUI installer at a time — concurrent GUI installs
  queue behind console switches rather than blocking each other. If the goal
  is installing several OSes with no babysitting, prefer **unattended
  installs** instead of GUI interaction: Windows `autounattend.xml`, Linux
  preseed/kickstart/autoinstall, FreeBSD `bsdinstall` script mode — all just
  ride along on the same ISO and ask the hypervisor for nothing beyond what
  it already provides (boot the ISO, expose the target disk). This is the
  recommended default for any VM defined in `hype.cfg` that doesn't need
  operator interaction, and sidesteps the mouse/focus limitations entirely.

## 6d. Installation workflow: ISO → virtual disk or physical disk

Two independent axes: where the *install media* comes from (always an ISO in
v1), and where the *install target* lives. Every target is exposed to the
guest through the same block-device front-end abstraction, so the installer
never knows or cares which backing it is writing to.

A VM may be given **any number of disks**, and each disk independently
specifies:

- its **backing** — one of four, all presenting identically to the guest:
  a whole **physical disk** (SATA/AHCI or NVMe), a **partition** on such a
  disk, a **raw file**, or a **qcow2 file** (§10 decision 3), where a file
  lives on a host volume formatted **FAT32, exFAT or ext**;
- its **bus** — the guest-facing front-end: `virtio-blk`, `ahci-sata` or
  `nvme`, defaulting from `os_hint` and overridable per disk (§6, §10
  decision 26).

The backing and the bus are fully independent: a qcow2 file on an ext
volume can be presented as NVMe, and a physical partition as `ahci-sata`.
That independence is what `struct blk_backend` buys — see below.
`docs/hype-cfg-spec.md`'s `[disk.<id>]` stanza is the authoritative config
surface for all of this.

- **Install media (ISO)**: read from a host block device, exposed to the
  guest as a virtual optical drive (AHCI/ATAPI CD-ROM, or a virtio-scsi
  CD-ROM for Linux/BSD). Placed first in guest firmware boot order whenever
  `boot = installer`.
  The ISO is **always streamed** from its host device, never preloaded into
  hypervisor RAM: the RAM/chunked load existed only until streaming worked,
  and it caps media size at what `AllocatePages` can serve contiguously.
  The media device is **not** required to be the device hype booted from.
  The intended deployment is hype installed on a **USB stick**, with the
  installer ISOs *and* the file-backed guest disk images on a **separate
  internal drive**; the machine may have **any number** of SATA or NVMe
  disks, formatted **FAT32, exFAT or ext**. So, per VM, the operator
  specifies both **which disk** the media lives on (by drive **serial/GUID**
  from `hype.cfg`, matched the same way a `physical:` target is, with
  auto-detection as the fallback) and **which ISO** on it
  (`install_media`) — and reads go through the same host-side block driver
  set physical targets use (**AHCI + NVMe + USB**, §10 decisions 25/31; USB
  disks are named by the identity #340 captures). A host **ATAPI** path so a
  real DVD-ROM can be used as media directly is a further option, not a
  requirement.
  Guest-facing, the drive presents as a **DVD-ROM with a data disc in it** —
  `GET CONFIGURATION`'s current profile is DVD-ROM — so a guest treats it
  exactly as a physical DVD-ROM presenting a real disc. See §10 decision 25.
- **Virtual disk target (`target_disk = file:<path>`)**: a raw (or qcow2,
  §10 decision 3) file on host storage. Created ahead of time by
  `tools/make-disk-image.sh`, **or by hype itself** via the terminal's
  `mkdisk` command (added 2026-08-17, TERM-9/#485; TERM-11): a qcow2 image on
  a filesystem that already exists on a chosen host disk. **Preallocated is
  the default** — every cluster allocated and its metadata written at create
  time — and **thin provisioning is in scope too** (§10 decision 42): a
  backing file holding only the clusters the guest has written, grown on
  demand, on a host filesystem that can represent a hole. hype creates *files
  on existing volumes*; it does not partition disks or make filesystems.
  `target_disk_size_gb` is a **declaration of intent that hype validates**,
  not a creation instruction: hype compares it against the resolved image's
  real size and reports a mismatch, which catches a VM pointed at a stale or
  truncated image. The host filesystem controls whether the backing file can
  remain sparse (§10 decision 29). ext supports true holes. FAT32 and exFAT do not;
  their writers allocate and zero-fill any logical gap before publishing the
  new file size. Reads and writes from the guest use the host filesystem
  driver already needed to load ISOs and guest firmware/varstore.
- **Physical disk target (`target_disk = physical:<serial-or-guid>`)**: the
  guest's writes go straight to a real drive. This needs the hypervisor to
  own a minimal **host-side block driver** (AHCI + NVMe covers the vast
  majority of real hardware) so it can issue raw reads/writes after
  `ExitBootServices()`, since UEFI's Block I/O protocol is gone by then. The
  block backend abstraction (`struct blk_backend` — `file`, `image`,
  `physical` (AHCI/NVMe/USB) and `qcow2` implementations behind one vtable)
  is what lets **every** front-end serve **every** backing unmodified: a
  front-end takes a `blk_backend *` and nothing else, which is why adding a
  bus costs no backend work and adding a backing costs no front-end work.
  The single security-critical bounds check (guest LBA+count against the
  backend's real capacity, §6j) lives in `blk_backend`'s dispatcher rather
  than in any implementation, so no backing can forget it.
- **Partition target (`partition = <n>`)**: a physical target may be scoped
  to one GPT partition instead of the whole drive, so a machine's spare
  partition can host a guest without dedicating the disk. Scoping is a base
  LBA plus a **clamped capacity** in the physical backend, which means the
  existing bounds check confines the guest to that partition for free. The
  non-empty-partition-table guard (below) must become partition-aware to
  match — checking LBA 0/1 of the whole disk says nothing about whether the
  target partition holds data.
- **Disk identification safety**: physical targets are matched by drive
  **serial number or GUID**, captured during a pre-`ExitBootServices`
  enumeration pass (walking UEFI Block I/O handles, which is available for
  free before boot services exit), never by a positional index like "disk 0"
  — index-based addressing is fragile against cable/port reordering and
  makes a config typo capable of silently wiping the wrong drive. At VM
  start, the hypervisor re-confirms the enumerated serial matches the
  configured one before allowing any write; on mismatch it refuses to boot
  that VM's installer rather than guessing.
- **Interactive confirmation + non-empty-disk guard (decided, §10)**: serial/
  GUID matching alone is not sufficient given how destructive this operation
  is. Before the *first* write to any `physical:` target, the hypervisor
  shows an interactive confirmation on the local dashboard/console (§6b) —
  drive model, serial, size — that the operator must explicitly accept, and
  separately refuses to write to any physical disk that already contains a
  non-empty partition table unless a distinct explicit "allow overwrite"
  flag is set for that specific disk. A `physical:` entry in `hype.cfg`
  alone is never sufficient to trigger a wipe.
- **Two-phase boot**: exactly as in §5's `boot` field — `boot = installer`
  for the initial run (CD-ROM first in boot order, target disk attached but
  not bootable-first), then the operator flips the same VM's config to
  `boot = disk` for subsequent boots once install completes. No automatic
  "is this disk bootable now" detection in v1 — keep it explicit and simple.
- **Concurrency note**: a physical disk target is exclusively owned by the
  one VM it's attached to for the duration that VM is defined/running — the
  hypervisor does not attempt to share a physical drive (or one of its
  partitions) across multiple guests in v1.

## 6e. Networking (required)

Required, not optional, per decision: many Linux net-installers need network
to fetch the base system, and any online-account/update flow does too.

- **Host NIC drivers**: minimal host-side drivers so the hypervisor can drive
  the physical network adapter directly after `ExitBootServices()`, the same
  way §6d's AHCI/NVMe driver does for storage. Intel e1000/e1000e-class is the
  first target — broad hardware support, simple register interface,
  well-documented. The supported families for v1 are **Intel e1000e/igb,
  Realtek r8169, Intel igc, Broadcom bnxt/tg3, and Marvell/Aquantia
  atlantic** (decision #36); each is a separate driver behind the single NIC
  vtable of decision #34, sitting on the shared host-PCI bind + DMA-ring +
  IRQ/poll facility that decision grows from this work — same isolation
  principle as `blk_backend`, and no second PCI enumerator alongside
  `core/host_pci.c`.
- **Forwarding plane, never an endpoint** (decision #36): host NAT genuinely
  requires protocol code — Ethernet framing, ARP for the uplink, IPv4 header
  parsing, UDP/TCP address/port rewriting with checksum fixup, and a
  connection-tracking table — and all of that is in scope. What is **not** in
  scope is hype acting as a network *peer*: no sockets API, no TCP state
  machine hype drives as an endpoint, no reassembly (over-large fragments are
  dropped, not reassembled), and **no listening socket of any kind** in the
  hypervisor core. hype may rewrite a packet passing between a guest and the
  wire; hype is never the address a packet is sent to. The single exception is
  the uplink **DHCP client**, which NAPT needs to obtain hype's own uplink
  address: outbound-initiated only, one transaction at a time, replies matched
  against hype's own outstanding transaction ID, with a static-address setting
  in `hype.cfg` for operators who prefer to disable it.
- **Guests may listen; hype may not.** The rule above constrains the
  hypervisor, not the guests. A guest running a server on its own virtual NIC
  is expected and supported — it runs its own TCP/IP stack, and hype only
  moves frames. Guest-to-guest reachability works today through `net_peers`
  (decision #21) or an opt-in virtual switch (NET-6). Reaching a guest's
  listening port from **outside** the host additionally needs an inbound
  destination-NAT rule (port forwarding), which is deliberately **not** part
  of the baseline NAT (NET-4 is outbound plus established-return only) and is
  tracked separately as NET-8. Port forwarding is still forwarding: hype
  rewrites the packet and the *guest* is the endpoint, so it is consistent
  with the rule above — the thing that is banned is a socket hype itself owns.
- **Guest-facing device**: virtio-net as the default frontend (Linux/BSD
  inbox support); Windows needs virtio-win's network driver injected if
  virtio-net is used for it, or an emulated e1000-compatible NIC (Windows
  has an inbox-ish driver path for this via Basic/standard NIC classes,
  mirroring the AHCI-for-Windows / virtio-for-Linux-BSD split already used
  for storage in §6a).
- **Connectivity model**: a simple host-level NAT — guests get outbound
  connectivity through the one physical NIC the hypervisor owns, with the
  hypervisor doing basic NAT/port translation. **Guest-to-guest traffic is
  default-deny, but explicitly configurable per pair** (not "whatever NAT
  happens to allow," and not an absolute ban either): each guest's virtual
  NIC sits in its own isolated segment on the hypervisor side by default —
  never a shared L2/broadcast domain with other guests' virtual NICs, so
  there's no ARP-spoofing or accidental sniffing surface between VMs that
  weren't deliberately connected. An operator who *wants* two specific
  guests to talk to each other opts in explicitly via `net_peers` (below);
  the hypervisor then adds a narrow host-mediated forwarding rule between
  exactly that pair — still not a shared broadcast domain, so every VM not
  named in that pairing stays fully isolated from both of them. This pairwise
  `net_peers` mechanism covers point-to-point use cases (e.g. a database VM and
  an app-server VM that are meant to talk to each other).
- **Opt-in shared L2 segment (virtual switch, NET-6 #223)**: for the case where
  *several* VMs must genuinely share one network (broadcast/ARP/DHCP among all
  members — not O(N²) pairwise rules), an operator may define a named virtual
  switch and place specific VMs' NICs on it. This is a **deliberate opt-in that
  supersedes the earlier "no virtual switch" stance**: per-guest isolation
  remains the default (a VM is never on a shared switch unless configured onto
  one), so the inter-VM ARP-spoof/sniffing surface a shared broadcast domain
  reintroduces is scoped to exactly that switch's members and opt-in — every VM
  not on it stays fully isolated. Each switch is either fully private
  (`uplink = none`) or NAT-uplinked to the WAN via the host NIC (`uplink = nat`,
  reusing the NAT path above). L3 routing *between* switches, and VLAN tagging,
  are deferred (a future NET-7). No always-on general-purpose switch — kept
  minimal, matching "thin," but the shared-network use case is now expressible.
- **Config**: per-VM `net_mode = none | nat` in `hype.cfg` (default `none`
  for VMs that don't need it, to avoid the NIC driver and NAT path
  mattering for purely offline installs), plus an optional `net_peers =
  <vm-name>[,<vm-name>...]` listing which *other* VMs (by their `hype.cfg`
  name) this one is explicitly allowed to exchange traffic with directly.
  Listing a peer on either side of a pair establishes it bidirectionally —
  no need to list it on both. Empty/omitted `net_peers` (the default) means
  no guest-to-guest connectivity for that VM at all, regardless of
  `net_mode`. `net_peers` entries require both named VMs to have
  `net_mode = nat`; validated at startup (§6i).

## 6f. VM lifecycle control

The dashboard (§6b) lets the operator switch which VM's console has focus;
it also needs explicit power-state control over each VM, independent of
which one currently has focus:

- **Start** — boot a defined VM that isn't currently running (fresh boot per
  its `boot` setting — installer or disk — not a memory-state resume; see
  §6h for the distinction). Guest RAM is explicitly **zeroed before the VM's
  first instruction ever executes** — every page backing that VM's EPT/NPT
  is cleared, not just reused as-is, so no guest (and no leftover host
  hypervisor scratch data) can ever observe stale contents from whatever
  previously occupied that memory. This applies on every fresh boot, not
  only the very first one — a VM that was Force-powered-off and then
  Started again gets freshly zeroed RAM, not whatever was left behind.
- **Stop** — pause a running VM: freeze its vCPU(s) in place, keep guest RAM
  and device state resident, do not power it off. Resumable instantly via
  Start-equivalent ("resume"), with no guest-visible interruption beyond the
  paused wall-clock time.
- **Shutdown** — request a graceful guest power-off: the hypervisor raises
  an emulated ACPI power button event (GPE), the guest OS runs its normal
  shutdown sequence and eventually signals S5, at which point the
  hypervisor tears the VM down. Bounded by a timeout (configurable,
  sensible default e.g. 90s) in case the guest never responds.
- **Force power off** — immediate, unconditional VM teardown regardless of
  guest state, equivalent to pulling the plug. Used when Shutdown's timeout
  expires, or the operator explicitly wants it (e.g. a hung installer).
- All four are available per-VM from the dashboard, independent of console
  focus — e.g. force-power-off a hung background VM without switching to
  its console first.

Two further verbs change the VM *set* rather than a VM's power state
(added 2026-08-17, TERM-9/#485 — this reverses part of §10 decision 33,
recorded there):

- **Create** — define a new VM at runtime from the terminal (§6b) and
  start it with no host reboot. Gated by the same admission check as a
  boot-time definition (§6i, #453): a VM that would have been refused at
  startup is refused at create time, with the real numbers in the
  message. The new definition is written back to `hype.cfg`, so a later
  host reboot produces the same VM set.
- **Delete** — remove a defined VM. If it is running, Delete
  force-powers-off first (same path as above, same guarantees), then
  removes the definition from `hype.cfg`. Backing disk images are
  **never touched** — deleting a VM deletes configuration, not data;
  reclaiming the image is a separate, explicit operator act.

## 6g. Fault isolation between guests

A misbehaving guest must not be able to affect other guests or the
hypervisor itself:

- **Memory isolation is inherent to the architecture already chosen**: each
  VM has its own EPT/NPT address space (§2/§4), so a guest cannot read or
  corrupt another guest's memory regardless of what it does to itself. This
  holds on **both** scheduling tiers (§3): memory isolation comes from
  nested paging, not from pinning. On the shared tier the scheduler swaps
  the NPT/EPT root and the ASID/VPID on every vCPU switch, so a descheduled
  vCPU's memory is unaddressable by whichever vCPU is running. Register and
  MSR isolation is ordinary context-switch hygiene — the same save/restore
  that already runs on every exit.
- **CPU isolation — the same guarantee, two different mechanisms.** A hung
  or spinning guest must never starve another of CPU time. How that is
  assured depends on the tier, and the difference matters:
    - **Dedicated tier: by construction.** A vCPU spinning forever occupies
      only its own exclusively-pinned core. Nothing else can be affected,
      because nothing else is there. No mechanism has to work correctly for
      this to hold.
    - **Shared tier: by mandatory preemption.** Cores are shared, so the
      construction argument is gone and an actual mechanism replaces it —
      the scheduler forcibly preempts a running vCPU at the end of its
      slice, whatever the guest is doing (interrupts disabled, inside an
      interrupt shadow, spinning in a tight loop). A guest that could defer
      its own preemption indefinitely would void this guarantee, so the
      preemption path must be **proven, not assumed** — that proof is a
      deliverable (SMP-20), not an implementation detail.
- **What the shared tier genuinely weakens: microarchitectural side
  channels.** vCPUs taking turns on one core share caches, TLBs and branch
  predictors, and SMT siblings share L1D and execution ports concurrently
  (the L1TF/MDS class). Pinning eliminates this by construction; sharing
  reintroduces it. This is bounded, not eliminated, by default-deny trust
  groups: distrusting groups never occupy one physical core simultaneously,
  and µarch state (L1D + IBPB) is flushed when a core changes group. Within
  a trust group there is no restriction and no flush, because two vCPUs of
  one VM already share a core in the SMP case and mutually-trusting VMs are
  the same situation. A VM that must never share a core with anyone uses the
  dedicated tier. See §10 decision 39.
- **SMT siblings are not the hazard; distrusting co-residency is.** The rule
  above forbids two distrusting owners on one physical core *at the same
  time*. It does not forbid using sibling threads. Within one owner — one
  dedicated VM, or one trust group in the pool — every thread of a granted
  core is dispatchable, with no restriction and no flush. The enforcement is
  therefore **core-granular allocation**, not disabling SMT: a core is
  handed to one owner whole, and is drained and flushed (L1D + IBPB) before
  it is handed to another. Disabling SMT for the pool would idle half the
  machine even when every pool VM is in one trust group, which no isolation
  requirement asks for. See §10 decision 40.
- **What still needs building**: a per-vCPU **watchdog** in the VM-exit
  dispatch loop that detects a guest that's actually gone wrong (not just
  busy) — repeated unhandled/unrecognized VM-exit reasons, a triple fault,
  or an unresponsive state that isn't simply "guest is doing legitimate
  work" — and responds by automatically applying **Force power off** (§6f)
  to that specific VM only, logging the event to the dashboard/serial, and
  leaving every other VM completely unaffected. No hypervisor-wide halt or
  reset in response to a single guest's fault.

## 6h. Host power lifecycle & guest state persistence

When the *host* reboots or shuts down (operator-initiated from the
dashboard, or an external power event the hypervisor can catch), and when
it starts back up again:

- **On host shutdown/reboot**: the hypervisor attempts a **clean shutdown of
  every running guest first** — the same graceful ACPI-power-button sequence
  as §6f's Shutdown action, run across all VMs (in parallel, each with its
  own timeout), falling back to Force power off for any guest that doesn't
  respond in time — before the host itself actually resets/powers off via
  UEFI Runtime Services. Best-effort, bounded, never blocks the host action
  indefinitely on one stuck guest.
- **State persisted across the host power event**: a small state record
  (which VMs were running vs. stopped at the moment shutdown began) is
  written to persistent storage (the ESP, alongside `hype.cfg`) as part of
  the shutdown sequence. Both the write and the next boot's read go through
  hype's own storage stack via the shared boot-volume locator (§10 decisions
  37/38), not through firmware. Implemented (#176) as `\hype-state.txt`, a
  versioned plain-text record **keyed by VM name, not by index** — an
  operator who reorders their `[vm.*]` sections between boots would
  otherwise have the record start a different machine than the one that was
  up. A record whose version this build does not know, or that is damaged,
  is refused whole rather than read in part: a half-restored host cannot be
  told apart from a correctly restored one.
- **On next hypervisor startup**: read that state record and automatically
  re-**Start** every VM that was previously running (a fresh boot from its
  disk/target per §6f's Start semantics), leaving previously-stopped VMs
  stopped. This is explicitly a **restart-to-the-same-run-state**
  mechanism, not a live-memory snapshot/resume — guest RAM contents are not
  preserved across the host power event (that would be VM
  snapshotting/hibernation, an explicit non-goal, §1). What's restored is
  "was this VM supposed to be running," not "exactly where it was."

## 6i. Startup admission control

At hypervisor startup, before launching any VM, validate `hype.cfg` against
actual host resources and refuse to start any VM that would overcommit them
— **required**, not best-effort:

- Sum every defined VM's `mem_mb`; if the total (plus a reserved margin for
  the hypervisor's own memory, device buffers, and guest firmware/varstore
  regions) exceeds physical RAM as reported by the UEFI memory map, fail
  fast with a clear diagnostic (dashboard + serial) naming which VMs would
  need to be reduced or disabled, rather than starting some VMs and running
  out of memory later during arbitrary guest operation.
- Same principle for CPU, but the rule is now **per tier** (§3):
  every **dedicated**-tier vCPU needs a whole physical core of its own
  (§10 decision 47), so the sum of dedicated `vcpus` may not exceed the
  **cores** available to them, with the BSP's core reserved. Threads do not
  enter it — a granted core supplies all of its threads to the one VM that
  owns it, which changes what that guest *sees* and never what it costs.
  **Shared**-tier sCPUs are deliberately over-committed onto a pool, so a
  core count is not the limit there; what is checked instead is that the
  shared pool is non-empty whenever any shared VM is configured, and that
  over-commit stays within a configured maximum ratio.
- **Explicit `cpu_set` validation**: `cpu_set` names **physical cores**, and
  each listed core is granted whole. Since a vCPU *is* a physical core
  (decision 47), the rule is simply **`cpu_set` entry count == `vcpus`** —
  the operator is naming exactly the cores they are asking for. Confirm every
  listed core actually exists on this host. Nothing about SMT enters this
  check: the threads of those cores are what the guest ends up seeing, not
  what it is charged for. Overlap rules follow the tier:
    - Two **dedicated** VMs' `cpu_set` ranges must not overlap. Refused
      outright (not just warned about), exactly as before: exclusive pinning
      is what §6g's by-construction argument relies on.
    - Two **shared** VMs sharing pool cores is the entire point, and is
      allowed.
    - A core appearing in both a dedicated `cpu_set` and the shared pool is
      the new refusal — it would silently break the dedicated VM's
      exclusivity, which is the one thing that tier promises.
  VMs without an explicit `cpu_set` are auto-assigned only from cores no
  `cpu_set` entry has claimed.
- **Target-disk and varstore uniqueness (security-critical, not just
  hygiene)**: reject startup if any two VMs' `target_disk` resolve to the
  same `file:` path or the same `physical:` serial/GUID, and likewise if
  any two VMs would resolve to the same persisted varstore file. §6d's
  "exclusively owned by one VM" claim for physical disks is only true if
  this is actually enforced — without it, a config mistake (not even an
  attack) lets one guest read or corrupt another guest's disk or UEFI
  variables. Varstore file names are derived from the VM's name in
  `hype.cfg` specifically so they can't collide by construction, but the
  check still runs in case of manual file manipulation.
- **`net_peers` validation**: every name listed in any VM's `net_peers`
  must refer to another VM actually defined in `hype.cfg`, and both VMs in
  a pairing must have `net_mode = nat` — reject startup otherwise. This is
  what keeps guest-to-guest connectivity an explicit, auditable opt-in
  rather than a typo silently no-op'ing (or, worse, a config that *should*
  have isolated two VMs failing open some other way).
- This check runs once at startup against the full `hype.cfg`; it does not
  attempt to handle configs changing at runtime (no hot-reload — out of
  scope, consistent with §1's non-goals).

## 6j. Guest-supplied input validation (device emulation trust boundary)

**This is the actual guest-to-host/guest-to-guest attack surface**, and it's
distinct from — and not covered by — the EPT/NPT memory isolation in §2/§4.
EPT/NPT stops a guest from directly addressing another guest's or the host's
memory. It does **not** stop the *hypervisor itself* from being tricked into
touching the wrong memory on a guest's behalf, which is exactly what happens
if device emulation trusts guest-supplied addresses/lengths without
checking them:

- **Hard rule**: every emulated device (virtio-blk, virtio-net, AHCI/NVMe
  command processing, PS/2) that takes a guest-supplied address, offset, or
  length (virtio queue descriptors, AHCI command FIS buffer pointers, block
  I/O LBA + sector count) **must validate it against that specific VM's own
  guest-physical-address range and the backing resource's actual size**
  before the host ever dereferences it or performs the corresponding host
  I/O. No raw guest pointer is ever trusted directly — translate
  guest-physical to host-virtual through that VM's own EPT/NPT mapping, and
  bounds-check both the address and the length.
- **Applies to storage explicitly**: guest-supplied LBA + sector count must
  be checked against the backing store's real size — the file-backed
  image's actual length, or the physical disk's real capacity (§6d) —
  before either a `file:` or `physical:` `blk_backend` implementation
  performs the read/write. An out-of-range request is rejected, not
  clamped or silently truncated.
- **Why this matters more than usual here**: the hypervisor runs at the
  most privileged level, with no OS underneath and no process boundary to
  contain a bug (§10 decision #17's tradeoff — C was chosen over Rust for
  the VMX/SVM core, so this validation is not memory-safety-by-default; it
  has to be deliberately written and reviewed for). A missed bounds check
  in device emulation is a full guest-to-host (and transitively
  guest-to-guest) compromise, not a crash.
- **Not covered by the fault-isolation watchdog (§6g)**: that watchdog
  detects hangs and exit-reason anomalies, not memory-safety violations —
  it's a liveness mechanism, not a substitute for input validation here.
- **No guest gets direct hardware access in v1**: physical disk (§6d) and
  physical NIC (§6e) access are always mediated through the hypervisor's
  own host-side driver plus an emulated guest-facing frontend
  (virtio-blk/AHCI, virtio-net/e1000-class) — never PCI passthrough or
  direct DMA from a guest to real hardware. This is why no IOMMU (VT-d/
  AMD-Vi) is required for v1 (§4); it also means the validation above is
  the *only* thing standing between a guest and the host for storage/
  network I/O, since there's no hardware-enforced DMA remapping backing it
  up. If passthrough is ever added later (explicitly out of scope for v1),
  this invariant must be revisited alongside an IOMMU requirement.

## 6k. Scripted guest input (headless validation)

§6c covers a *human* driving a guest through the console. This covers the
other case: hype driving a guest itself, with nobody watching.

The need is a validation one, not an install one. Several claims about hype
can only be settled from *inside* a guest — "these two VMs cannot see each
other's files", "the installed system boots from the target disk", "this
distro's installer completed" — and every one of them currently requires an
operator at a keyboard. That makes them expensive, unrepeatable, and in
practice skipped, which is exactly how a two-guest test comes to prove
liveness and quietly not prove isolation.

**Model.** Each VM may have an expect-style script dropped on the ESP as
`\input\vm0.txt`, `\input\vm1.txt`, mirroring the existing per-VM
`\iso\vm1.iso` convention. Absent file means no scripting and byte-identical
behaviour to today — the capability is inert unless deliberately armed, and
nothing in the normal boot path depends on it.

**Language.** Line-oriented and deliberately small; this is a test fixture,
not a shell:

```
timeout 120000                       # ms, applies to each expect
expect  localhost login:
send    root\n
expect  ~#
send    echo vm0-marker > /tmp/m\n
expect  ~#
send    cat /tmp/m\n
expect  vm0-marker
fail-if vm1-marker                   # seeing the OTHER guest's marker is the bug
pass    two-vm-isolation-vm0
```

`expect` waits for a substring in that VM's console output; `send` types into
it; `delay` pauses; `pass`/`fail` end the script with a verdict; `fail-if`
arms a substring that fails the run if it ever appears. An expect that times
out is a failure, not a hang — a validation harness that can wedge is one that
reports nothing, which is the failure mode being designed out.

**Transport.** v1 injects into the per-VM guest UART receive path, because
that is where the guests actually take input (`ttyS0`). PS/2 scancode
injection comes later and, when it does, retires the `HYPE_FW_1_AUTO_KEY_INJECT`
one-shot hack (GLADDER-6b) that exists today only to clear a firmware prompt.

**Verdict.** `pass`/`fail` produce a definitive log line and per-VM dashboard
state. This is the part that makes it a validation tool rather than a typing
gadget: a headless run must self-report, because on the cold-boot-only test
laptop the log is the only telemetry there is (§6b), and silence is not
evidence of success.

**Safety.** A script can type into a root shell, so: it is per-VM and cannot
address another guest; it is armed only by a file the operator put on the ESP;
and it grants no new authority — the physical-write confirmation (§6d) and the
target-matching guard stay in force, so a script cannot cause a destructive
write that an operator has not separately confirmed.

## 7. Repository layout (proposed)

```
/hype
  /boot        - UEFI application entry (PE32+ image), Boot Services glue,
                  memory map handoff, ExitBootServices sequence
  /core        - arch-independent VMM core: vCPU abstraction, scheduler,
                  VM-exit dispatch, config parser, console/menu
  /arch/x86_64
    /vmx       - Intel VMX backend (VMCS setup, VM-exit handlers, EPT)
    /svm       - AMD SVM backend (VMCB setup, VM-exit handlers, NPT)
    /cpu       - GDT/IDT/paging/APIC/MSR bring-up for the hypervisor itself
  /devices     - virtual chipset: PIC/IOAPIC, PIT/HPET, serial, ACPI table
                  synth, virtio-blk, virtio-net, AHCI, GOP framebuffer,
                  emulated flash/varstore
  /storage     - host-side block backends: blk_backend vtable, file-backed
                  implementation, and host AHCI/NVMe drivers needed to
                  read/write physical disks post-ExitBootServices (§6d) —
                  adapted from an existing small, GPLv3-compatible-licensed
                  AHCI/NVMe driver (decided, §10) rather than written from
                  scratch, scoped to native AHCI mode + NVMe-over-PCIe only
  /net         - host-side NIC driver (e1000/e1000e-class first target) and
                  the basic NAT layer required for guest networking (§6e)
  /fw          - minimal guest UEFI firmware image (own DXE-lite or reuse
                  a stripped EDK2/OVMF build as a vendored blob)
  /tools       - image-build scripts (mkfat, cfg validator, disk image prep)
  /docs
  plan.md
```

## 8. Toolchain

**Decided (§10 decision #17): C, via two separate build pipelines** — not
Rust, and not one single build system for everything:

- **`hype.efi` itself**: C, built with a **lightweight freestanding UEFI
  toolchain** — clang/lld targeting `x86_64-unknown-uefi` (or GNU-EFI) —
  rather than a full EDK2 workspace. Fast iteration, no EDK2 build-system
  overhead for something that's just our own PE32+ binary. Inline asm for
  VMX/SVM instructions, MSR access, and VM-exit trampolines is more mature
  and better documented in C than in Rust's still-novel low-level VMX/SVM
  crate ecosystem, which is why C won out over Rust for the hypervisor core
  specifically, despite Rust's memory-safety appeal.
- **Guest firmware blob** (§10 decision #1): built separately via **EDK2**,
  since that's what's needed to vendor/reconfigure the stripped OVMF build —
  this pipeline is independent of the one that builds `hype.efi`, and only
  matters starting at M4 (§9), not M0.
- Target: `x86_64-unknown-uefi` (PE32+) for `hype.efi`. Unsigned for v1, per
  §10 decision #5 (Secure Boot disabled on test hardware).

## 9. Milestones

1. **M0 — UEFI "hello world" + memory map dump.** Confirm build/boot/deploy
   loop on real hardware and QEMU+OVMF.
2. **M1 — Boot Services exit + own kernel context.** Own GDT/IDT/paging,
   serial console, panic handler, timer tick.
3. **M2 — VMX bring-up, single vCPU, no guest yet.** VMXON, minimal VMCS,
   **including APICv (Intel) / AVIC (AMD) enabled from this milestone**
   (decided, §10 — not deferred as a later optimization), launch into a tiny
   hand-written guest payload (e.g. `hlt` loop), confirm VM-exit round trip
   works.
4. **M3 — EPT + first real guest boot.** Identity-mapped guest RAM, boot a
   minimal Linux kernel (bzImage) directly (no firmware) via a basic Linux
   boot protocol shim — cheapest way to validate the VM-exit loop, APICv/AVIC
   interrupt delivery, and device stubs before investing in guest UEFI
   firmware.
5. **M4 — Guest UEFI firmware + ACPI synth.** Boot a stock Linux UEFI
   installer ISO (e.g. Debian netinst) end-to-end through GRUB.
6. **M5 — virtio-blk + AHCI device models solid enough for installers to
   partition/format/write disk images.** Full unattended Linux install to a
   virtual disk, reboot into installed OS.
7. **M6 — BSD guest.** FreeBSD installer boot + install, reusing M4/M5
   plumbing; fix up any FreeBSD-specific ACPI/loader quirks.
8. **M7 — Windows guest.** AHCI/NVMe path, Windows Setup boot + install,
   exercising the full Hyper-V-compatible CPUID/MSR leaf set implemented per
   §6a/§10 (not a minimal stopgap — built in full up front); this is usually
   where the most ACPI/timer/CPUID fidelity bugs surface regardless, so
   validate here rather than assuming the leaf set is correct.
9. **M8 — Multi-VM concurrency + status dashboard.** Run one of each
   (Windows + Linux + BSD) simultaneously, console-switch between them via
   the local dashboard (§6b), confirm isolation (EPT/NPT faults don't cross
   VM boundaries, each tier's core-allocation rule holds -- §3) and that
   reported stats match reality.
10. **M9 — Persistence.** Reboot the *host* into hype.efi again and boot
    already-installed guest disks (not just fresh installers) — validates
    the varstore persistence and disk-image reuse path.
11. **M10 — Physical disk install target.** Host AHCI/NVMe driver (§6d),
    serial/GUID-based disk enumeration and match-before-write safety check,
    install one guest straight onto a real drive and boot it natively
    outside the hypervisor afterward to confirm the resulting install is a
    normal, non-virtualized-dependent OS install.
12. **Stretch** — legacy/CSM boot shim, Secure Boot signing, basic
    passthrough NIC via VT-d, simple snapshot of guest disk images.

## 10. Key decisions

Each item below started as an open question; all are now decided. Kept here
as a log of the decision plus the alternatives considered, so the reasoning
isn't lost.

1. **Guest firmware scope — decided: vendor a stripped/reconfigured OVMF
   build**, patched only for our varstore persistence + ACPI hand-off,
   rather than writing a UEFI firmware from scratch or a from-scratch/EDK2
   hybrid. Reinventing a UEFI stack is a bigger undertaking than the
   hypervisor core itself and buys nothing over a proven implementation.
   "Thin" describes the hypervisor, not a mandate to also write firmware
   from scratch.
2. **Windows CPUID/MSR expectations — decided: full Hyper-V-compatible leaf
   set** (`0x40000000`–`0x40000006` + synthetic MSRs, matching KVM's
   `hyperv-compat` mode), not the minimal vendor-string-only leaf, and not
   deferred to a follow-up pass. See §6a for detail and §9 M7 for where it's
   validated.
3. **Disk image format — decided: raw sparse file**, over a custom minimal
   COW format or qcow2. Simplest, hardest to get wrong, no format-parsing
   surface inside the trusted hypervisor. Revisit only if snapshotting
   becomes a real ask (currently a stretch goal).
   **SUPERSEDED (#200, M5-9): qcow2 is now supported alongside raw.** Kept
   above rather than rewritten, because the reasoning was sound and the
   trade-off is what changed, not the analysis. What changed: images
   pre-created by `tools/make-disk-image.sh` must be **fully allocated**
   anyway (a sparse hole is a sector the filesystem has not assigned, and
   hype's post-`ExitBootServices` writer cannot assign one — §6d), so "raw
   sparse" lost the space advantage that motivated it, while operators
   already have qcow2 images from other tools. The format-parsing surface
   this entry worried about was contained by refusing rather than guessing:
   compressed clusters, any encryption, `refcount_order != 4`, any v3
   incompatible-feature bit, snapshot-shared clusters (COPIED clear) and
   refcount-table growth are all rejected outright. Also note the format is
   **sniffed from the header magic, not configured** — deliberately, so an
   operator swapping a raw image for a qcow2 one does not also have to
   remember to edit `hype.cfg`; a raw image cannot be mistaken for a qcow2
   because none begins with `QFI\xfb` *and* passes the header validation.
   Raw remains the default and the recommended format.
   **Settled by #336:** `docs/hype-cfg-spec.md`'s `format = raw | qcow2` key
   and this sniffing are not in conflict — the key is an **assertion**, not a
   selector. Detection stays authoritative, and a declared `format` that
   disagrees with the image makes hype **refuse** rather than sniff on. That
   keeps the swap-without-editing property above, and gives the key a real
   job: catching a VM pointed at the wrong *file*, which is much more likely
   a stale path than a conversion anyone wanted. Rejected making the key
   authoritative: its only argument is that a corrupt qcow2 header would
   silently read as raw, and it would not — `hype_qcow2_init` refuses rather
   than guessing, so a broken qcow2 already fails loudly. Rejected dropping
   the key: the raw-declared-but-actually-qcow2 direction is worth catching,
   since writing a qcow2 through the raw path would corrupt it.
   **Amended by decision 42:** the premise above that a pre-created image
   "must be fully allocated anyway" no longer holds. hype's writer *can* now
   assign a missing range where the host filesystem can represent one
   (decision 29), so a backing file may be thin and grow at runtime. Raw
   still remains the default and the recommended format.
4. **Testing strategy — decided: QEMU/KVM nested virtualization
   (`-cpu host,+vmx`) for fast day-to-day iteration through M0–M6, plus a
   mandatory real-hardware validation pass at every milestone gate** — not
   QEMU alone and not real-hardware-only. Nested VMX/SVM emulation doesn't
   faithfully reproduce every edge case (some VM-exit reasons, EPT violation
   nuances), so QEMU is necessary but not sufficient.
5. **Secure Boot / signing — decided: ship unsigned, require Secure Boot
   disabled, for now.** Getting a Microsoft UEFI CA signature is
   disproportionate effort at this project's current scale; self-signing
   plus operator-side `db`/MOK enrollment (shim-style) is the realistic
   longer-term path **if/when** Secure Boot retention becomes a real
   requirement — revisit then, not before.
6. **Interrupt/IPI model for SMP guests — decided: hardware-accelerated
   APICv (Intel) / AVIC (AMD) from the start**, not a trap-and-emulate
   software model with hardware acceleration added later. Built in at M2
   (§9) rather than as a subsequent optimization pass.
7. **Host storage driver scope — decided: adapt an existing small,
   GPLv3-compatible-licensed AHCI/NVMe driver** rather than write one fully
   from scratch, scoped to native AHCI mode + NVMe-over-PCIe only (no
   legacy IDE/RAID modes), and kept behind the `blk_backend` vtable (§6d,
   §7) as an isolated module the VM-exit core doesn't depend on. Dropping
   physical-disk support entirely was rejected — it's an explicit
   requirement (§6d). The source driver must itself be MIT/BSD, Apache,
   GPLv3, or "GPLv2-or-later" — plain GPLv2-only code is not GPLv3-compatible
   and must not be adapted from, per the project license (see top of file).
8. **Destructive-write safety on physical targets — decided: serial/GUID
   matching (§6d) PLUS a mandatory interactive confirmation on the local
   dashboard (§6b) before the first write to any `physical:` target, PLUS a
   refusal to write to any disk that already has a non-empty partition
   table unless a separate explicit per-disk "allow overwrite" flag is set.**
   Serial/GUID matching alone was judged insufficient given how destructive
   this operation is — a `physical:` config entry alone must never be
   sufficient to trigger a wipe.
9. **Guest networking — decided: required, not optional** (§6e). A host NIC
   driver and basic NAT are in scope for v1, not deferred, because several
   Linux net-installers genuinely need it. e1000/e1000e-class chosen as the
   first supported real NIC family for the same reason AHCI was chosen for
   storage: broad hardware support and a simple, well-documented interface.
10. **`hype.efi` as the default boot target — decided: explicitly out of
    scope.** Configuring UEFI `BootOrder` NVRAM to auto-launch `hype.efi` is
    left to the operator via standard firmware tooling; this project starts
    from "firmware is already booting `hype.efi`."
11. **Per-VM power control — decided: four explicit operations** (§6f) —
    Start, Stop (pause/resume in place), Shutdown (graceful ACPI-driven),
    Force power off (immediate teardown) — available per-VM from the
    dashboard independent of which VM currently has console focus.
12. **Fault isolation between guests — decided: required.** A misbehaving
    guest must never affect others. Memory isolation falls out of the
    already-chosen EPT/NPT-per-guest architecture unconditionally; CPU-time
    isolation comes from exclusive pinning on the dedicated tier and from
    mandatory preemption on the shared one (§6g, decision 39). On top of
    that, a per-vCPU watchdog (§6g) detects genuinely faulted guests
    (triple fault, unrecognized VM-exit storm) and auto-applies Force power
    off to that VM alone, never a hypervisor-wide response.
13. **Host power lifecycle — decided: best-effort clean shutdown of all
    guests on host shutdown/reboot, then restore each VM to its prior
    run/stopped state on the next hypervisor startup** (§6h). Explicitly a
    restart-to-same-run-state mechanism (a persisted "was it running"
    record), not a live-memory snapshot/resume — guest RAM is not preserved
    across the host power event, consistent with snapshotting being a
    non-goal (§1).
14. **Startup admission control — decided: required, not best-effort**
    (§6i). Total configured `mem_mb` and `vcpus` across all VMs in
    `hype.cfg` are checked against actual physical RAM and the physical
    **core** count at hypervisor startup, with the BSP's core reserved (core,
    not thread — decision 47); any VM that would overcommit either is refused
    with a clear diagnostic rather than allowed to start and fail later.
15. **Guest RAM zeroing on boot — decided: required, on every (re)start.**
    Every page of a VM's reserved guest RAM is zeroed immediately before
    that VM's first instruction executes (§2, §6f) — including restarts
    after Force power off, not only the hypervisor's own initial boot.
    Prevents any guest from ever observing stale contents left behind by a
    prior occupant of that memory (a previous guest, or hypervisor scratch
    data). Does not apply to Stop/Resume (§6f), which intentionally
    preserves guest RAM across the pause.
16. **CPU pinning granularity — decided: operator-specified core subsets
    (`cpu_set`), not hypervisor-auto-assigned-only.** A VM can be pinned to
    an explicit set of host cores (e.g. reserve cores for the host/dashboard,
    or split a large machine's cores deliberately across VMs) rather than
    always taking whatever the hypervisor picks. Exclusivity — no two VMs
    ever share a pinned core — remains mandatory **for the dedicated tier**
    and is enforced at startup admission control (§6i), since §6g's
    by-construction fault-isolation argument depends on it. *Amended by
    decision 39*, which added a second, explicitly-shared tier: that
    sentence read "no two VMs, ever" when pinning was hype's only model.
17. **Language/toolchain — decided: C**, via two separate build pipelines
    (§8) — a lightweight freestanding UEFI toolchain (clang/lld or GNU-EFI)
    for `hype.efi` itself, and EDK2 solely for the vendored guest firmware
    blob (decision #1). Rust was considered and rejected for the hypervisor
    core specifically due to less mature low-level VMX/SVM crate support.
18. **Real-hardware validation coverage — decided: both Intel (VT-x/EPT)
    and AMD (AMD-V/NPT) hardware are available**, so §10 decision #4's
    real-hardware gate can validate both CPU vendor code paths in parallel
    from the start rather than Phase B (AMD-V) being blocked pending
    hardware acquisition.
19. **Device-emulation input validation — decided: required, explicit hard
    rule** (§6j). Every guest-supplied address/length used by device
    emulation must be bounds-checked against that VM's own memory/backing
    store before the host acts on it — found missing from the plan during
    a security review focused on guest/host isolation; this is the actual
    guest-escape vector, not EPT/NPT (which only stops direct guest-to-guest
    memory access, not the hypervisor misusing an untrusted guest address on
    the guest's behalf).
20. **`target_disk`/varstore uniqueness — decided: enforced at admission
    control** (§6i), not left as an unstated assumption. Two VMs sharing a
    `target_disk` or varstore path was a real gap between §6d's claimed
    "exclusively owned" guarantee and what was actually being checked.
21. **Guest-to-guest network isolation — decided: default-deny, with
    explicit per-pair opt-in via `net_peers`** (§6e), not "whatever NAT
    happens to allow" and not an absolute ban either. Accidental
    guest-to-guest communication is what's being prevented — deliberate,
    operator-configured communication between two specific named VMs is a
    legitimate use case (e.g. an app-server VM and a database VM) and is
    supported via a narrow host-mediated forwarding rule between exactly
    that pair, validated at admission control (§6i) so a typo can't
    silently no-op it or leave an unintended VM reachable.
22. **Dashboard input exclusivity — decided: required, explicit
    focus-owner check** (§6b) — keystrokes while the dashboard has focus
    are never forwarded to any guest, including whichever VM had focus
    immediately before, closing a potential input-leak path between the
    dashboard and a backgrounded guest.
23. **Minimum guest OS target — decided: Windows (any 64-bit version),
    Linux (any 64-bit distribution), and BSD (any 64-bit variant —
    FreeBSD/OpenBSD/NetBSD); no 32-bit guest support of any kind, on any of
    the three families.** BSD was already in scope via §5's `os_hint` enum
    and §9's dedicated M6 milestone; this decision makes the "minimum bar,
    all three families, 64-bit only" framing explicit rather than implicit.
    Windows and BSD both boot via guest UEFI firmware (§6, §7 `/fw`); Linux
    additionally gets a direct `bzImage` boot path (M3) as the cheapest way
    to validate the core VM-exit loop before guest firmware exists — not as
    its only supported boot path, a firmware-booted Linux guest is equally
    valid. Guests may still pass transiently through real-mode/protected
    mode as part of their own normal boot sequence (e.g. a Linux bzImage's
    own decompression stub); that's the guest's own boot code, not a
    32-bit-guest support path this project needs to build.
24. **Writable host exFAT (#198) — decided: FAT-chained growth, an up-case
    table hype can reproduce exactly, and no directory creation.** The
    read-only host FS reader (#181) resolves a path to extents; persisting
    guest writes to a backing file that lives *on* a filesystem needs a
    writer, and exFAT's allocation bitmap, entry-set checksums and up-case
    table each force a choice:
    - **Growth always produces a FAT-chained (`NoFatChain == 0`)
      allocation**, and a file that was contiguous gets its FAT chain
      materialised the first time it grows. The alternative — keep files
      contiguous by demanding the next physical cluster — is less
      bookkeeping but fails whenever that cluster is already taken, which
      is precisely the "backing file on a stick that already holds data"
      case this work exists to serve. In-place writes to a pre-allocated
      file never move or relink anything, so the contiguous fast path is
      preserved for the case that matters most.
    - **The up-case table is checksum-verified against its own
      `TableChecksum` at mount, and only its first 256 code points are
      decompressed and cached.** A name containing a character outside that
      cache is *refused at creation* rather than written with a `NameHash`
      derived from a guessed identity mapping — a hash other exFAT
      implementations disagree with leaves the file unfindable to them.
      Decompressing the whole 65536-entry table would cost 128 KiB of
      hypervisor memory for names hype itself generates (ASCII log and
      image paths), which is disproportionate. Refusing to mount a volume
      whose table fails its checksum is deliberate for the same reason:
      hype must fold names the way every other driver does, or not at all.
    - **A volume whose allocation bitmap is not physically contiguous is
      refused at mount.** No formatter produces one, and hype indexes the
      bitmap with plain sector arithmetic; rejecting beats silently reading
      the wrong sector.
    - **Directory creation, removal and rename stay out of scope.** hype
      writes to a path the operator or the installer has already created.
    - **`VolumeDirty` is set on the medium before the first structural
      change and cleared, together with `PercentInUse`, by an explicit
      flush.** Both fields sit at the only boot-sector offsets the
      boot-region checksum excludes, so neither update requires
      recomputing that checksum — which is why an interrupted write can be
      made visible to the next mounter for free.
    - The reader was tightened to match: it now **enforces the
      directory-entry-set checksum, honours the active-FAT selection when a
      volume has two FATs, and honours `NoFatChain` on a directory's own
      allocation** (a contiguous multi-cluster directory has no FAT chain
      to follow, so the previous FAT walk found nothing past its first
      cluster). A volume with a corrupt entry set, an allocation outside
      the cluster heap, or a chain shorter than the recorded file size is
      now refused rather than half-parsed.

25. **Media/image source device (#319) — decided: an explicitly selected
    host block device, over AHCI *and* NVMe *and* host ATAPI, matched by
    serial/GUID with auto-detection as the fallback.** hype's host-side
    media lookup was written when the ISO always lived on the disk hype
    booted from: `hostdisk_read()`/`fatvol_read()` are hardwired to a single
    `(abar, port)` pair found by taking the **first** SATA port whose
    signature is a non-ATAPI hard disk, and both the ISO scan and the
    file-backed guest-disk-image resolve read through it. The intended
    deployment (§6d — hype on a USB stick, ISOs *and* images on a separate
    internal drive) breaks each of those assumptions at once, so all three
    are being changed together rather than one at a time:
    - **NVMe must be a media source, not just a `physical:` target.** The
      plan already committed to "AHCI + NVMe covers the vast majority of
      real hardware" for the target-disk axis (§6d), and
      `hype_nvme_host_read()` exists and works — but it is used only for
      probing and target matching, never to read media. On a modern machine
      the separate data drive is likely NVMe, which today means neither the
      ISOs nor the disk images on it are reachable at all. Rejected
      alternative: require the media on a SATA disk. That makes the
      supported hardware a function of an implementation detail rather than
      a decision, and would be invisible to an operator until nothing was
      found.
    - **The device is chosen by serial/GUID from `hype.cfg`**, reusing the
      matching `physical:` targets already do, with today's auto-detection
      kept as the fallback so existing single-disk setups keep working
      untouched. Rejected alternative: positional selection ("disk 1").
      §6d already rules that out for physical targets because it is fragile
      against port reordering; the same reasoning applies to a read-only
      source, where a wrong guess is a confusing failure rather than a
      dangerous one but is just as hard to diagnose.
    - **Selection order: the configured device, else a disc, else the boot
      disk.** An explicit `hype.cfg` device wins; failing that a host ATAPI
      device with a readable disc in it; failing that today's behaviour (the
      ISO file on a host volume, then raw partition 2). The disc is tried
      *before* the file deliberately: the first implementation had it after,
      so an operator who inserted a disc silently got whatever
      `\iso\test.iso` happened to be sitting on the boot medium instead. An
      empty tray falls straight through, so nothing that works today
      regresses, and the log always names which source won -- "it booted
      something" is not the same as "it booted what you inserted".
      Rejected alternative: file-first with the disc as a fallback. That
      makes the meaning of inserting a disc depend on the unrelated contents
      of another drive.
    - **A host ATAPI (DVD-ROM) device is a first-class media source.** The
      port scan currently *requires* the non-ATAPI signature `0x00000101`,
      so a real optical drive is skipped by construction — an operator with
      a bootable disc has to copy it onto a partitioned disk first, which
      is a worse story than the hardware hype emulates. hype models the
      guest side of ATAPI already (`devices/atapi.c`), but its host AHCI
      driver has only ever spoken to a hard disk, so host-side `READ(10)`
      via the packet protocol is genuinely new code.
    - **Every media filesystem the operator may reasonably use is
      supported: FAT32, exFAT and ext.** The ISO resolve tries only the two
      FAT variants today while the guest-disk-image resolve beside it
      already tries all three, which is an accident rather than a decision
      (#320).
    - **Streaming must handle a fragmented ISO.** The one-extent restriction
      the ISO path carries is stricter than the 64-extent contract the rest
      of the block stack uses, and a multi-GB ISO on a volume that is not
      freshly formatted will fragment; on ext, `core/ext.h` notes large
      indirect-mapped files are structurally fragmented, so ext support
      without multi-extent streaming would be nearly useless (#327).
    - **The RAM/chunked preload is retired, not kept as a fallback** (#326).
      Keeping it would preserve the very duplication that let the streaming
      path never learn to honour `install_media`, and it silently caps media
      size. It may only be deleted once streaming covers configured paths,
      device selection, NVMe and multi-extent files — otherwise capability
      is lost rather than consolidated. **Done** (`3b35b64`): the deletion
      also had to make stream resolution **per-VM** (per-VM media existed
      only in the RAM loader), move the `boot = disk` check into the
      resolver, and un-gate resolution from AHCI discovery — it ran only
      inside the AHCI-port branch, so an NVMe-only host would have been left
      with no media at all.
    - **The boot medium itself is a media source** (#326). The simplest
      deployment is one USB stick holding hype *and* the ISOs, and it was
      the one drive hype could not take media from: sources were AHCI/NVMe
      only, and the RAM path had hidden this by reading the ISO through UEFI
      before `ExitBootServices`, while firmware still owned the USB stack.
      No new block or filesystem code was needed — post-EBS hype already
      drives the stick through its own xHCI + USB-MSC backend and already
      mounts a FAT32 volume on it to write `\HYPEFULL.LOG` (#216/#230), so
      this only exposes that backend as a media device. It is registered but
      not made *active*, so a machine with a real internal disk is
      unchanged. Rejected alternative: declare that ISOs must live on a
      non-boot drive. That is a restriction imposed by an implementation
      gap, not by anything about the hardware, and it would have made the
      cheapest possible setup the unsupported one.
      Caveat, tracked as #340: hype captures no USB identity yet, so the
      stick is registered with an empty serial and can be **auto-detected
      but not named** by `media_disk` — which fails safe (it is also never
      substituted for another drive's name), but leaves the one-stick
      deployment unable to be stated explicitly in config.
    Implementation is sequenced as an enabling refactor first — replace the
    two hardwired globals with one selected media device behind the
    `(ctx, lba, count, dst)` callback `core/iso_stream.h` and
    `core/nvme_host.h` *already* share — so that each backend afterwards is
    additive and independently testable. The refactor is behaviour-preserving
    by construction and is the only step that touches existing paths.
    No change is needed to how the drive is presented to the guest: hype
    already reports DVD-ROM as the current MMC profile.

26. **Guest disk front-end selection and multi-disk VMs — decided: any
    number of disks per VM, each naming its own bus (`virtio-blk` |
    `ahci-sata` | `nvme`), with `os_hint` supplying only the default.**
    §6 previously had the front-end *derived* from `os_hint` with no way to
    override it, and named only two front-ends. Three things forced a
    decision:
    - **Derivation is not selection.** An operator installing Windows to one
      disk and keeping a Linux data disk on another needs different buses on
      the same VM, which a per-VM `os_hint` cannot express. So `os_hint`
      becomes the default (`windows` → `ahci-sata`, else `virtio-blk`) and an
      explicit per-disk `bus =` always wins. `docs/hype-cfg-spec.md` already
      specified exactly this, including the defaults — the spec was ahead of
      both the code and this plan, and this entry brings the plan level.
    - **A guest NVMe front-end is required, and does not exist.** §6a
      mentioned "AHCI + emulated NVMe" once, in passing; there is no guest
      NVMe model in the tree at all. It is the largest single piece of work
      in this area, and it is worth it because NVMe is inbox on every
      supported guest OS *and* is what a modern machine looks like, so it is
      the least surprising thing to present.
    - **v1 ships the NVMe front-end INTx-only**, not MSI/MSI-X. hype's PCI
      model has no capability-list support beyond hand-written vendor caps,
      256-byte config space, 32-bit BARs and INTx delivery only — and
      virtio-blk already declines MSI-X (`0xFFFF` NO_VECTOR) and works.
      NVMe 1.4 permits pin-based interrupts. Rejected alternative: build
      MSI/MSI-X first. That is a guest-LAPIC message-write path plus
      capability modelling, i.e. a second large piece of work stacked in
      front of this one, on the *assumption* that INTx is insufficient.
      Whether OVMF's `NvmExpressDxe` and Windows' `stornvme` bind over INTx
      is to be settled by **experiment before any of it is written** — #262's
      history is a direct warning against guessing what EDK2 will accept.
    Sequencing: the `[disk.<id>]` config parser lands first, because every
    other item needs somewhere to be configured from; the AHCI-SATA
    front-end is made *selectable* before the NVMe one is *written*, since
    it already exists and only lacks a switch; partition-scoped physical
    targets are independent of both and can land in parallel.

27. **Guest boot framebuffer (#350) — decided: QEMU ramfb supplies firmware
    GOP; Bochs VBE remains the separate post-boot adapter.** The vendored OVMF
    already contains `QemuRamfbDxe`. Each VM therefore receives its own
    writable `etc/ramfb` fw_cfg file. OVMF allocates the framebuffer inside
    that VM's RAM, publishes GOP, and writes the big-endian surface description
    back through fw_cfg. hype validates the full address, stride and height
    against that VM's GPA map before reading pixels. This makes the GOP handoff
    available to kernels such as OpenBSD's `efifb` without adding another PCI
    device. Rejected using Bochs VBE for firmware GOP: that duplicates an OVMF
    driver path already present and couples boot firmware to the later OS video
    model. Rejected host-allocated or shared framebuffer memory: either choice
    weakens the per-VM memory boundary. Bochs VBE remains the PCI-discoverable
    adapter for a guest OS driver after boot; ramfb remains the firmware and EFI
    framebuffer path.

28. **Post-boot diagnostic persistence — decided: USB log files are the only
    persistent live-run channel; the RT-3 EFI-variable tail is retired.** RT-3
    wrote the last 16 KiB of the in-memory log through UEFI Runtime Services and
    recovered it on the next boot. The implementation calculated a checksum over
    the entire growing log on every VM exit before testing its 60-second write
    deadline. A 2026-08-11 AMD run measured 174.8 of 177 seconds in that path.
    Rate-limiting the checksum would remove that immediate cost, but would retain
    a second persistence mechanism, firmware flash writes, a post-EBS Runtime
    Services dependency, and shared writer state beside the BSP-owned USB log.
    The operator chose to retire RT-3. `HYPE.LOG` and the per-VM USB logs remain
    the supported persistent record and are drained in bounded slices by the BSP.
    A panic still paints its message directly to the framebuffer and halts. A
    cold power loss before the next USB slice may therefore lose the newest
    in-memory tail; this is accepted in exchange for one persistence path with
    measured bounded latency. Rejected alternative: keep RT-3 but move its
    checksum and `SetVariable` call to the BSP at 60-second intervals. That fixes
    the measured scan loop but preserves the redundant firmware-dependent path.

29. **Sparse host files — decided: one sparse-aware logical range contract,
    with format-specific allocation semantics.** The old shared extent list
    represented only physically allocated sectors. It therefore rejected ext
    holes and unwritten extents, and it could not state whether a writer may
    allocate a missing range. The filesystem refactor (#292/#293) will use a
    neutral logical range map whose entries distinguish allocated data, holes,
    and unwritten allocation. Callers read holes and unwritten allocation as
    zeroes. A writer advertises its allocation and growth capabilities rather
    than returning a short or false-successful write.
    - **FAT32 has no sparse-file representation.** A valid non-empty file is a
      FAT cluster chain sized to cover `DIR_FileSize`. A random write beyond
      EOF allocates every intervening cluster and zero-fills the logical gap
      before publishing the larger directory size. A chain shorter than the
      existing size remains corruption and is refused; it must never be
      reinterpreted as a hole.
    - **exFAT uses `ValidDataLength` but does not provide arbitrary holes.**
      `DataLength` still requires allocation through the logical end of the
      stream. Reads from `ValidDataLength` to `DataLength` return zeroes. A
      write beyond `ValidDataLength` allocates through the new `DataLength`,
      zeroes any gap needed to make the valid prefix contiguous, then advances
      `ValidDataLength`. A missing allocation inside `DataLength` is corruption,
      not sparseness.
    - **ext2/3/4 supports true holes and unwritten extents.** Reads synthesize
      zeroes without allocating. Writes into holes allocate filesystem blocks,
      update the inode mapping and allocation counters, and preserve all
      enabled metadata checksums. ext2 allocation may use ordered direct
      metadata updates while the volume is marked dirty. ext3/4 allocation
      requires a valid jbd2 metadata transaction and commit before the new
      mapping is exposed. Existing in-place writes to allocated blocks remain
      available without allocation.
    - **Crash ordering is part of the writer contract.** New blocks are zeroed
      or filled before metadata exposes them. Allocation bitmap, mapping,
      inode length, free counts, checksums, and filesystem dirty/clean state
      are committed in a format-valid order with block-device barriers. A
      failure leaves either the old mapping or recoverable allocated metadata;
      it must never expose stale contents or map one block to two files.

    Rejected treating a short FAT chain as sparse: FAT has no logical block
    indices in a chain, so there is no safe way to infer where a missing range
    belongs. Rejected keeping the old physical-only extent contract and adding
    writer-specific exceptions: every caller would then need filesystem
    knowledge, defeating #293 and recreating inconsistent bounds behavior.

30. **Host NTFS support (#337) — decided: FULL read/write, the fifth
    `hype_fs_ops_t` driver.**

    > **Revised (supersedes the original read + in-place-write-only scope).**
    > The first cut (#337, landed) shipped read + in-place write only, on the
    > reasoning that growth/allocation would turn the resolver into a full
    > journaling writer (`$Bitmap` + runlist + `$MFT` rewrites, jbd2-class
    > ordering) that hype did not then need. That scope is now lifted: NTFS is
    > a first-class read/write filesystem, on par with the ext2/3/4 writer
    > series. A read-only NTFS driver does not let hype own a Windows data
    > disk, which is the point of supporting it. The engineering cost noted
    > below is real and is why this is an epic of ordered slices, not the
    > reason to avoid it. The original in-place driver is the correctness
    > baseline the writer extends, not a competing design.

    NTFS runlists map onto the decision-29 logical range contract: an
    allocated run is `DATA`, a sparse run (no LCN) is `HOLE` reading as zeroes.
    The read model and the in-place writer are unchanged and stay the base.

    - **Full writer scope (the new work):** append/grow a `$DATA` stream;
      allocate into a `HOLE` (sparse fill) and advance initialized size;
      create / unlink files, mkdir / rmdir, rename; `$Bitmap` cluster
      allocation and release; `$MFT`/`$MFTMirr` record allocation and mirror
      consistency; and `$LogFile`/USN journal maintenance so the volume stays
      recoverable by Windows' own chkdsk. Each mutation is ordered so a crash
      leaves either the old state or a chkdsk-repairable one — never readable
      stale bytes — the same discipline the ext3/4 jbd2 writer proved
      (decision 29's fault-sweep-per-crash-point method applies verbatim).
    - **Dirty volumes are supported, not refused.** A dirty NTFS volume has
      pending `$LogFile` transactions from an unclean Windows unmount
      (fast-startup, hibernation, or a crash). The writer must process
      `$LogFile` — replay/rollback to a consistent state — before mounting
      writable, and maintain it on every mutation, exactly as a real NTFS
      driver does. (Read-only mounting of a dirty volume without touching
      `$LogFile` remains available as the conservative path.)
    - **Fixups are mandatory and verified.** Every MFT record and INDX block
      has its update sequence array checked and un-applied before any field is
      trusted, and re-applied on write; a fixup mismatch is a hard refusal
      (torn write). Getting this wrong reads plausible garbage, so it carries
      dedicated tests.
    - **Still refused:** compressed (LZNT1) or encrypted (EFS) `$DATA` —
      writing these needs the compression/encryption codecs, a separate body
      of work not in this scope; resident `$DATA` grows by conversion to
      non-resident (an explicit writer step) rather than rewriting bytes in
      place inside the MFT record.
    - **BitLocker stays out — it is not NTFS.** A BitLocker volume is an
      encrypted container; the only supported behaviour is detecting and
      refusing it (the Intel box's internal NVMe is one). Every NTFS write
      still sits behind the §6d `physical:` chain: a volume mounts writable
      only through this driver's own gates AND the serial/GUID + interactive
      confirmation every physical target requires.

    Rejected ZFS alongside this: 128 KB compressed records with multiple DVAs
    cannot be expressed as a flat logical range map, so it would need a second
    read model — out of scope. Validation mirrors the ext writer: mkfs.ntfs /
    Windows-created volumes populated through the real `ntfs-3g`/Windows
    driver, every writer path fault-swept per crash point, and `chkdsk`
    (or `ntfsfix`/`ntfsck`) clean plus a Windows/ntfs-3g remount reading back
    byte-exactly after every operation.

31. **Multiple USB mass-storage devices — decided: additional USB disks are
    claimable as media-only devices, behind per-device transfer rings.**
    Today hype claims exactly ONE USB MSC — its boot/log medium, first
    encountered wins (#241) — and the xHCI layer owns one bulk-ring pair per
    controller, so a second stick cannot be brought up at all: with #340's
    identity capture in place, naming a second stick in `media_disk` refuses
    cleanly but can never succeed. Decided:
    - **Per-claimed-device bulk transfer rings** (ring pair + BOT state per
      claimed slot, not per controller), so N MSC devices on one controller
      can be live. The controller-wide transfer lock stays: one transfer at
      a time per controller is the concurrency contract the #343/#377 work
      proved; per-device rings remove the *bring-up* limit, not the
      serialisation.
    - **The log-sink claim is unchanged**: the first MSC remains hype's
      boot/log medium (#241's actual concern was re-pointing the log sink
      mid-boot, and that stays forbidden). Each further MSC is claimed as a
      **media-only** device — registered via `media_add_dev` under its
      captured identity (#340), never touching the log sink, up to the
      existing `HYPE_MEDIA_MAX_DEVS` cap.
    - **Unidentified extra sticks stay unmatchable** (#323): they register
      with an empty serial, auto-detectable only.
    - Rejected: claiming extra sticks only when a configured serial matches
      (the sweep cannot know whether a later device matches, and an unnamed
      second stick would be invisible to auto-detection for no reason);
      re-pointing the log sink at a config-named stick (re-opens the exact
      mid-boot failure #241 closed).

32. **USB disks as `physical:` targets — decided: same chain, third bus.**
    #340 gives a USB disk the enumerated identity the destructive-write
    chain needs; the `blk_backend` plan already names `physical`
    (AHCI/NVMe/USB) as one backing (§6d). A USB `physical:` target goes
    through **exactly** the decision-8 chain — serial match at VM start,
    interactive dashboard confirmation before the first write, non-empty-
    partition-table guard — with `blk_usb` (which already reads, writes and
    SYNCHRONIZE-CACHEs) as the backend behind `phys_guard`. Two hard rules:
    - **hype's own boot/log medium is never a physical target**, whatever
      its serial: the inventory's `HYPE_USB_OWNER_HYPE` claim marks it and
      the admission check refuses it by identity, not by position. This
      makes decision 31 a prerequisite in practice: the only USB disk hype
      can drive today IS the boot medium.
    - **No identity, no target**: a stick reporting neither a VPD-0x80 nor
      an iSerialNumber serial cannot be named, matching the media rule —
      never positional.
    Rejected: a separate confirmation flow for USB ("it's removable, so
    softer rules") — a stick holds real data exactly like an internal disk,
    and two different destructive-write ceremonies is how one of them rots.

33. **VM count — decided: no compile-time cap; per-VM state is sized from
    the machine at boot.** §6 already promises "any number of VMs ...
    bounded by host resources", but the implementation grew a
    `HYPE_FW_MAX_VMS 2u` constant plus arrays, pools and 32-bit VM masks
    sized around it, and the config layer caps at 16 parsed VMs. Those are
    implementation stages, not design, and they are silently wrong on real
    targets: current AMD EPYC parts ship up to 192 cores and quad-socket
    boards exist, so a machine with 700+ usable cores is a legitimate host.
    The only real bounds are physical: the DEDICATED tier gives at most
    (usable cores - 1) VMs (the BSP keeps console/log duty); the shared tier
    is deliberately over-committed and bounded instead by the configured
    over-commit ratio (§6i, decision 39). Total guest RAM must fit either
    way. Decided:
    - **Per-VM state is allocated once at startup from the UEFI pool,
      before ExitBootServices, sized by the parsed config and validated
      against the detected CPU topology** — not a static array behind a
      constant. Admission (§6i) enforces VMs <= usable cores - 1 and the
      RAM budget, refusing at startup with the real numbers in the message.
    - **No 32-bit VM bitmasks.** Any per-VM mask (dashboard focus
      availability, isolation state) must scale with the VM count —
      a fixed `unsigned int` mask is the same silent clamp in disguise.
    - **Naming/UX surfaces must not assume single digits**: per-VM log
      files, the `\iso\vmN.iso` drop convention, dashboard rows (the
      dashboard needs paging/scrolling well before 700 rows, but it must
      never *misreport* the count).
    - Rejected: raising the constant (2 -> 4 -> 16 is whack-a-mole, and
      #237's silent 2-slot VMCB pool clamp is exactly the failure class
      this breeds).
    - ~~Rejected: runtime VM hotplug (out of scope — the set of VMs is
      fixed at boot from `hype.cfg`, only their power state changes,
      §6f).~~ **REVERSED 2026-08-17 (TERM-9, #485)** for operator-driven
      changes: an operator may define a new VM at runtime from the
      terminal and start it with no host reboot, and may delete one
      (§6f Create/Delete). The reasons this rejection gave do not vanish
      — they become requirements on the implementation: a free per-VM
      state slot must exist (#393's pool, not a compile-time constant),
      the new VM's guest RAM comes from the pre-reserved pool (#449),
      and the same admission check that gates startup gates creation
      (#453, §6i) — a create that would not have been admitted at boot
      is refused at runtime with the same real numbers in the message.
      What stays true: nothing *external* changes the VM set — no API,
      no guest, no file appearing on a disk. Only the local operator at
      the terminal (§6b), and every change is written back to `hype.cfg`
      so the set survives a host reboot exactly as configured.

34. **Driver interfaces — decided: a common vtable PER DRIVER TYPE where two
    or more real implementations share shape, plus one shared host-PCI-device
    facility (bind + DMA rings + IRQ/poll) grown from the NIC work; storage
    HBAs migrate onto it opportunistically. No single universal driver
    interface.** As the host-driver count grows (5 filesystems, 3 storage
    transports, 5+ NIC families incoming), the question is how much to unify.
    Decided by what already works:
    - **Per-type interfaces earn their place with concrete implementations,
      not in anticipation.** Two are already proven: `hype_fs_ops_t`
      (`core/fs_ops.h`) behind which FAT32/exFAT/ext/ISO9660/NTFS all sit, and
      `hype_blk_backend_t` (`core/blk_backend.h`) behind which file/image/
      qcow2/physical(AHCI/NVMe/USB) all sit — with the single guest-address
      bounds check (§6j) centralised in the block dispatcher so no backend can
      forget it. NIC drivers get the same treatment: one NIC vtable that the
      r8169/igc/e1000e-igb/bnxt/atlantic drivers implement (the NET epic's NIC
      device model). This is decision #17's "no premature abstraction" applied
      as a rule: abstract a type when it has ≥2-3 real implementations that
      genuinely share shape.
    - **The real shared layer is one level below the type vtables: the
      host-PCI-device scaffolding both storage HBAs and NICs need** — PCI bind
      (`core/host_pci.c` already does config space, BARs, bus-master, MSI/
      MSI-X), DMA descriptor rings + buffer pools, and an interrupt-or-poll
      model. AHCI, NVMe and xHCI each re-implement this today; the incoming
      NICs would re-implement it again. Build it ONCE, as the NET epic's
      DMA-ring and IRQ/poll slices, framed as a general host-PCI-device
      facility rather than NIC-only.
    - **Storage HBAs migrate opportunistically, never big-bang.** The working
      AHCI/NVMe/xHCI drivers are already unified where callers care (block I/O,
      via `hype_blk_backend_t`); they adopt the shared PCI/DMA/IRQ facility one
      at a time, only after it is proven on NICs and only where it comes out
      cleaner. A refactor of working storage drivers purely for symmetry is not
      a win (decision #17 / the §6j "one place to forget the check" reasoning).
    - **Rejected: a single universal driver interface** spanning fs + block +
      net + transport. Filesystems, block backends and packet NICs do not
      share a callable shape; forcing one vtable over them would be lossy
      abstraction with a translation layer at every leaf — the opposite of what
      `hype_fs_ops_t` and `hype_blk_backend_t` achieve by being type-specific.
    - **Rejected: a second "transport" vtable over AHCI/NVMe/xHCI + NIC.**
      Storage transports are already unified at the block layer that matters;
      a NIC is a packet device, not a block device. They share the PCI/DMA/IRQ
      scaffolding (the facility above), not a device-level interface.
    - Every type vtable and the shared facility are designed with a clean
      **registration seam** (a driver registry each driver self-registers into,
      plus a PCI-ID / probe match table per driver), because decision 35 wants
      to eventually load drivers as on-demand modules — and that is mechanical
      only if a driver already reaches the rest of hype solely through its
      vtable and the facility ABI, never through private cross-references.

35. **Loadable driver modules — decided: the eventual direction is on-demand
    module loading (load only the drivers the detected hardware needs, not
    every driver baked into `hype.efi`); deferred to v2+, with a design
    constraint on the interfaces now.** Today every host driver is compiled
    into the single `hype.efi` PE image. The goal is that, e.g., the NVMe
    driver is not loaded on a machine with no NVMe controller, and only the one
    host NIC family present is loaded.
    - **Why deferred:** post-`ExitBootServices` module loading is effectively a
      mini dynamic linker — a relocatable module format (PE/COFF or ELF), a
      loader that maps + relocates + resolves each module's imports against a
      table of symbols `hype.efi` exports, and detection→load wiring driven by
      the PCI enumeration. That is a substantial subsystem, and none of v1's
      milestones need it; a machine boots fine with unused drivers resident.
      So v1 stays a single image.
    - **What v1 does now to make it cheap later:** decision 34's registration
      seam. Each driver self-registers its type vtable (and a PCI-ID/probe
      match table) into a registry at init; hype selects drivers by matching
      enumerated hardware against those tables rather than by hardcoded calls.
      A driver that only ever reaches the rest of hype through its vtable + the
      shared facility ABI can later be split into a module with no logic
      change — the registry entry simply gets populated by the loader instead
      of a static initialiser. Keeping that boundary clean is the actionable v1
      cost; the loader itself is v2.
    - **Rejected for v1:** building the module loader now (no milestone needs
      it, and it is a large subsystem competing with guest-facing features);
      and its opposite, ignoring modularity entirely and letting drivers
      cross-reference each other freely (which would make the eventual split a
      rewrite instead of a mechanical extraction).

36. **Host networking scope, and whether a host TCP/IP stack belongs in the
    trusted core (#397 HNET-0) — decided: hype's host network code is a
    FORWARDING PLANE, never a network endpoint. Multiple real NIC families
    and the packet-translation logic NAT needs are ratified and in scope; a
    general host TCP/IP stack with sockets — and above all a LISTENING TCP
    socket (#406 HNET-9) — is rejected for v1.** This settles the conflict
    #397 filed against decision #9 and §6b, and supersedes decision #9's
    single-NIC-family wording without disturbing its substance.
    - **First, the §6b "conflict" is a misreading, and the record should say
      so once so it is not re-litigated.** §6b's "explicitly local-only for
      v1 — no serial or network exposure" is a statement about the *status
      dashboard*: the dashboard is not remotely administrable. It is not a
      ban on hype touching a NIC — decision #9 and §6e already put a host NIC
      driver and NAT in the trusted core, and both were ratified after §6b was
      written. So HNET-1..HNET-4 and the per-chip drivers never contradicted
      §6b at all. What §6b *does* forbid, in the clearest terms the document
      uses anywhere, is exactly the one thing HNET-9 proposes: a network
      service listening inside the hypervisor for connections from outside it.
      The conflict is real, but it is narrow, and it is one ticket wide.
    - **Multi-vendor NIC drivers: ratified, and already implied.** Decision
      #34 names "r8169/igc/e1000e-igb/bnxt/atlantic" as the NET epic's NIC
      set and builds the shared host-PCI/DMA/IRQ facility out of that work.
      Decision #9's "e1000/e1000e-class chosen as the *first* supported real
      NIC family" was always a starting point, not a cap. §6e's supported list
      is now explicit: Intel e1000e/igb (#80), Realtek r8169, Intel igc,
      Broadcom bnxt/tg3, Marvell/Aquantia atlantic — all behind the single NIC
      vtable of decision #34, all optional at runtime, none of them protocol
      code. A driver moves frames between a ring and a buffer; its blast
      radius is DMA correctness, which every host storage driver already
      carries. Adding a fifth NIC does not change hype's threat model. It is
      the coverage that makes §6e's "required, not optional" true on real
      hardware rather than only on emulated e1000e.
    - **The protocol layers are ratified only in their forwarding-plane form.**
      NAT is not free of protocol code, and pretending otherwise would just
      hide it: to masquerade several guests behind one physical port hype must
      parse Ethernet, answer and issue ARP for its own uplink address, parse
      IPv4 headers, rewrite addresses/ports and fix checksums for UDP and TCP,
      and keep a connection-tracking table. That is HNET-5..HNET-7's real
      content and it is in scope, because decision #9 already bought it. The
      distinction that matters, and the one this decision draws as a hard line,
      is **translate versus terminate**. hype may inspect and rewrite a packet
      that is passing between a guest and the wire. hype may not be the peer
      that a packet is addressed to. Concretely, in scope: header parse and
      rewrite, checksum fixup, conntrack, ARP for the uplink, fragment handling
      by dropping rather than reassembling. Out of scope: a sockets API, TCP
      reassembly or retransmission as an endpoint, any TCP state machine hype
      itself drives as a peer, and any bind/listen of any kind. A conntrack
      entry follows a TCP flow's state to know when to expire it; that is
      observation, not participation, and the difference is that hype never
      owns a receive queue that an attacker can drive.
    - **The one sanctioned endpoint is the DHCP client (#405 HNET-8), and it
      stays bounded.** NAPT needs the uplink to have an address, and on most
      real networks that address comes from DHCP, so refusing all endpoint
      behaviour would make decision #9 undeliverable. It is admitted as a
      narrow exception with its limits written down: outbound-initiated only,
      one transaction at a time, replies accepted only while a request of
      hype's own is outstanding and only if they match its transaction ID, no
      general UDP socket layer underneath it, and a static-address
      configuration path in `hype.cfg` so an operator who does not want even
      this can turn it off entirely.
    - **Rejected: HNET-9, TCP with listening sockets (#406).** No consumer for
      it exists. §6b rules out the remote dashboard, §1 lists a management UI
      as a non-goal, and no filed ticket asks for a management API — #397 asks
      the question ("state the consumer") and the epic itself does not answer
      it. Building it anyway would put pre-authentication, attacker-reachable
      parsing code inside a ring-0 payload that owns the IOMMU, every guest's
      NPT/EPT, and the physical disks. The hard invariant this project keeps
      is host↔guest isolation, and a listener is strictly worse than the guest
      surface hype already accepts: a guest is a party hype deliberately
      admitted, sandboxed and bounds-checks (§6j), whereas a LAN peer is
      unadmitted, unbounded and needs no guest at all to reach the code. A
      hypervisor is also the worst possible place to host a service, because
      it has no process to lose — there is no privilege left to drop and no
      component to restart. If a real remote-management use case ever appears,
      the answer is decision #9's own shape applied one level up: run the
      service in a guest that already has its own OS, its own hardened stack
      and its own blast radius, and give it whatever host-side hook it needs
      through a narrow, explicitly-modelled interface. That is a v2
      conversation and it starts with the consumer, not the socket.
    - **This constrains hype, not the guests.** A guest running a listening
      service on its own virtual NIC is expected and supported — the guest
      owns that TCP stack, inside its own sandbox, and hype only moves frames.
      Guest-to-guest reachability already works via decision #21's `net_peers`
      or NET-6's opt-in switch. Reaching a guest listener from outside the host
      needs an inbound DNAT/port-forward rule, which the baseline NAT does not
      include (NET-4 is outbound plus established-return only); that is tracked
      as NET-8 and is squarely *translate*, not *terminate* — hype rewrites the
      packet and the guest is the endpoint. Rejecting #406 therefore takes
      nothing away from guests; it removes a socket hype itself would own.
    - **Remote management of hype itself is a v2 goal, recorded in §13.** This
      decision rejects a listener in the v1 core; it does not reject the
      ambition. §13 states the two constraints the v2 design must satisfy
      (name the consumer first; host the service in a guest behind a narrow,
      validated control channel rather than in the trusted core). Promotion
      requires a new §10 decision, not a reopening of #406.
    - **Rejected: the alternative of cutting all protocol code and only
      bridging raw frames to a guest that runs its own stack.** It is the
      purest answer to the security question and it was considered seriously,
      but it silently repeals decision #9: with N guests behind one physical
      port and no NAPT, an L2 bridge needs the network to accept N MAC
      addresses on one link, which many switches and nearly all Wi-Fi links
      refuse, and it puts every guest directly on the operator's LAN — which
      contradicts decision #21's default-deny isolation posture as well. The
      forwarding-plane line above keeps the security benefit where it actually
      pays (no listener, no endpoint, no sockets) without breaking the feature.
    - **Board consequence:** the HNET tickets do not survive unchanged. The
      driver and facility slices proceed as filed; the protocol slices are
      re-scoped to translation-only and lose their "pending ratification"
      footers; #406 (HNET-9) is Rejected. Nothing was blocked by #406, so
      removing it strands no other work — which is itself evidence the
      listener had no consumer. #404 (HNET-7) loses its port-demux and
      datagram-API deliverables, since a bound local port is an endpoint.
      See §6e for the resulting supported-NIC list and the
      translate-versus-terminate rule in its normative form.

37. **Boot Services boundary (#449/#450/#451/#452/#453) — decided: hype reads
    its own files through
    hype's OWN storage stack, post-`ExitBootServices`; Boot Services are used
    only where no post-EBS equivalent exists. The single deliberate exception
    is memory reservation, which becomes ONE large pre-EBS pool sized from the
    memory map, carved post-EBS once the config is parsed.** Phase 0 grew into
    a second, firmware-shaped implementation of things hype already does
    better itself. Today `load_hype_cfg()` (`boot/main.c`) opens `\hype.cfg`
    through UEFI's Simple File System protocol; the guest firmware images are
    read the same way by the FW-1 block, which then keeps a per-VM *pristine
    copy in RAM* solely because "the ESP is unreachable post-EBS"; and a 64 KiB
    ISO-head read still goes through UEFI for a microtest. Meanwhile hype
    already drives AHCI, NVMe and xHCI/USB-MSC itself post-EBS, already mounts
    FAT32/exFAT/ext/NTFS/ISO9660 on them, and already streams multi-GB
    installer ISOs off any of the three — including off its own boot stick
    (decision 25 / #326). The firmware path is the weaker one, and it is the
    only reason the pre-EBS phase is large. The full list of remaining
    Simple-File-System callers is `load_hype_cfg()`, the FW-1 firmware load,
    `load_iso_head()`, `load_input_script()` and `fw_1_dump_prev_log()`; all
    five move.
    - **Config, guest firmware, run-state record and media all move to Phase 1
      (post-EBS), read through hype's own stack.** None of these is
      chicken-and-egg: each is storage I/O, and hype's storage stack needs
      nothing from Boot Services — it binds the controllers over raw PCI
      config space and MMIO, which is only *safer* after firmware has been
      shut out of the same controllers. Media already made this move and
      proved it (decision 25). Moving the firmware images with them also
      deletes the per-VM pristine RAM snapshot: a VM restart can re-read the
      images from disk instead of hoarding a copy of them per VM.
    - **Memory reservation cannot move, and is not asked to.**
      `AllocatePages` has no post-`ExitBootServices` equivalent — after EBS
      there is no allocator to ask, only the memory map hype captured before
      it. So the reservation stays in Phase 0. What changes is that it stops
      depending on the config: instead of "parse `hype.cfg` pre-EBS, then
      `AllocatePages` each VM's `mem_mb`" (`fw_1_resolve_guest_ram` →
      `hype_alloc_pages_any_2mb_aligned`, once per VM), Phase 0 reserves **one
      2MB-aligned pool** sized from `GetMemoryMap`'s usable RAM minus hype's
      own margin, and Phase 1 carves each VM's guest RAM out of that pool at
      the parsed size. That breaks the last real tie between the config and
      Boot Services. §6i's admission check moves with it and becomes a check
      against a known pool size rather than an estimate of free RAM.
      The handful of *address-constrained* pages stay as individual pre-EBS
      allocations, because only firmware can place them: the <1MB AP
      trampoline page and the <4GB AP page-table root
      (`hype_alloc_page_below_1mb`, `hype_alloc_pages_below_4gb`).
    - **One further Boot Services dependency stays, and it is not storage:**
      the host TSC is calibrated against `BootServices->Stall(20000)`, and
      hype's own xHCI driver consumes that frequency
      (`hype_xhci_set_tsc_hz`) before it can enumerate — a too-early Address
      Device is NAKed by High-Speed devices on real hardware. Since USB is
      one of the buses hype must read its own config from, the calibration
      must precede the storage bring-up, and Stall is the only timebase
      available that early. It stays in Phase 0. Replacing it with a
      Boot-Services-free calibration (PIT or HPET) is possible but is a
      separate change, not a prerequisite for this one.
    - **What is left in Phase 0** is then: locate GOP, `GetMemoryMap`,
      enumerate CPU features, calibrate the TSC via Stall, capture Block I/O
      drive serial/GUID identity (§6d — free before EBS, impossible after),
      the reservations above, and `ExitBootServices`. Loading `hype.efi`
      itself remains firmware's job by definition.
    - **Ordering hazard this fixes.** Today config parsing and every
      allocation are Boot-Services-only while media resolution is
      post-EBS-only, so hype commits guest RAM, firmware buffers and vdisk
      backing at a point where it cannot yet see which disks or ISOs exist.
      Carving from a pool after the storage stack is up removes that split:
      hype sizes what it commits from what it has actually found.
    - **This does not create a generic pluggable-backend abstraction.** No new
      framework, no new vtable, no new indirection layer. `hype_fs_ops_t` and
      `hype_blk_backend_t` (decision 34) already exist, already have their
      implementations, and already treat USB/SATA/NVMe as interchangeable
      candidates for media reads (#323). This decision points hype's own
      config/firmware/state reads at that same existing stack — one more
      caller, not a new mechanism. Decision 17's "no premature abstraction"
      is respected precisely because nothing is being abstracted.
    - **Isolation is unaffected.** All of this is host-side I/O performed by
      the host on its own behalf, before or between guest execution. It hands
      no guest a new path to host storage, adds no guest-supplied address to
      any host dereference, and therefore does not touch the §6j trust
      boundary or the host↔guest invariant in `AGENTS.md`. The guest-facing
      surface is unchanged: guests still see only emulated block front-ends
      backed by `hype_blk_backend_t`, with the §6j bounds check in the same
      dispatcher. Two second-order effects do need care and are called out in
      the tickets: a config-parse failure now happens after the point of no
      return, so it must degrade to built-in defaults with a loud diagnostic
      rather than panic; and guest RAM carved from a shared pool must still be
      zeroed per VM per (re)start, and no two VMs' carved ranges may overlap —
      the same admission-control property, now enforced by the carve.
    - **Rejected: keep reading `hype.cfg` via Simple File System because it
      works.** It works only when hype boots from a volume firmware can mount,
      and it forces every downstream decision (guest RAM size, media choice,
      firmware load) to be made before hype owns the machine. That ordering is
      what produced the RAM-preload cap decision 25 had to retire, and the
      per-VM pristine firmware copies above.
    - **Rejected: reserve guest RAM lazily post-EBS from the memory map,
      without a pre-EBS pool.** Technically possible — hype has the memory
      map — but it means hype second-guessing which "free" regions firmware
      and its own loaded image actually left free, with no allocator to
      arbitrate. A single `AllocatePages` pool is firmware's own answer to
      that question, obtained for free while it can still be asked.

38. **Locating hype's own boot volume (#447) — decided: ONE shared locator,
    used by
    both the read path and the write path, over every registered host block
    device.** hype needs to find the volume it booted from twice: to read
    `hype.cfg` and its firmware images (decision 37), and to write back
    `hype.cfg`, the §6h run-state record and the debug log (#447). Today only
    the write side has any implementation — `usb_log_setup()`'s
    superfloppy/MBR/GPT partition-base scan, wired to one already-identified
    USB backend. That scan is not USB-specific in substance: it already takes
    a generic `hype_blk_backend_t *`, its `(base + lba)` read/write shims are
    bus-neutral, and it already ends in `hype_fs_mount_auto()`. The only
    genuinely USB-bound part is its `sync` shim, hardcoded to
    `hype_blk_usb_sync(&g_usb_ubk)`, plus the single call site that passes the
    USB backend. Generalising it is a small, contained change, not a rewrite.
    - **Extract it once**, taking a `(read_fn, ctx)` pair, and run it over
      every registered host device the way media resolution already iterates
      candidates (#323) — so USB, SATA and NVMe boot volumes are found by the
      same code. Writing the same scan a second time for the read path is the
      failure mode this avoids; the owner's requirement is that the bus hype
      booted from is not a distinction hype's own code makes.
    - **The volume must be confirmed, not guessed.** A writable FAT volume is
      not evidence it is *hype's* volume. Confirmation is that `\hype.cfg` (or
      `\EFI\hype\`) is present on it, checked before the volume is trusted as
      either a config source or a save target — writing a fresh `hype.cfg` to
      the wrong disk is worse than not saving at all.
    - **The device registry must not silently drop hype's own boot volume.**
      `HYPE_MEDIA_MAX_DEVS` is 4 and registration order is fixed (NVMe, USB,
      ATAPI, AHCI), so a machine with three NVMe drives plus a stick already
      drops the AHCI disk with one log line. Once hype's own config lives
      behind the same registry, being dropped stops being a media
      inconvenience and becomes an unbootable configuration. The cap must be
      raised, or hype's boot volume registered ahead of the cap.
    - **Rejected: two locators, one per direction.** They would drift, and the
      read path would be the one without real-hardware exposure until a
      config change failed to load on somebody's NVMe machine.

39. **vCPU scheduling — decided: promoted from v2 to v1, as a second tier
    ALONGSIDE 1:1 pinning, not replacing it (2026-08-16).** Recorded here
    because this amends decision 16 and rewrites §6g's central argument;
    §13's own rule requires exactly that before any of it is built.

    v1 originally had no scheduler at all: one vCPU permanently and
    exclusively owned one pCPU (§3, decision 16), which is *why* §6g's
    fault-isolation guarantee held "by construction". The cost is that the
    host can never run more vCPUs than it has cores, and cores reserved for
    an idle VM are simply unavailable. Promoting the scheduler buys
    over-commit; the price is that one guarantee stops being free.

    **The model: two operator-chosen tiers, per VM** (`cpu_mode` in §5,
    validated by §6i).
    - **Dedicated** — today's path, byte for byte. Exclusive pinning, no
      scheduler on the dispatch path, no µarch sharing, no new failure mode.
      This remains the **default**, so an existing config behaves exactly as
      it did.
    - **Shared** — a pool of cores that several vCPUs are time-sliced onto.
      The "never share a core" rule is a property of the dedicated tier and
      does not govern shared-tier VMs; sharing a core is precisely what a
      shared VM opts into.

    A dedicated VM is then just a run queue of length one pinned to a core,
    so both tiers sit on one vCPU-run primitive rather than forking the
    dispatch path in two.

    **Why this is additive rather than a rewrite.** The VMRUN/exit engine
    and every emulation path are already per-vCPU and independent of the
    pinning model. A vCPU's switchable context already exists and is already
    saved and restored on every exit — VMCB/VMCS, FPU (#260), TSC_AUX
    (#277), GPR context. The one-shot LAPIC preemption timer (#364) is
    already a time-slice primitive. Decision 33's no-VM-cap work already
    decoupled the number of runtime vCPU contexts from the core count, which
    is exactly what a scheduler needs. What is genuinely new is the
    scheduler proper, context-switch orchestration across vCPUs, and holding
    a descheduled vCPU's interrupts until it runs again.

    **§6g re-derived (the required part).** Memory isolation is unchanged
    and unconditional: it comes from nested paging, not pinning, and the
    scheduler swaps the NPT/EPT root and ASID/VPID on switch. CPU isolation
    changes mechanism but not strength — by construction on the dedicated
    tier, by **mandatory preemption** on the shared one. That mechanism has
    to actually work: a guest able to defer its own preemption indefinitely
    would void the guarantee, so SMP-20 exists to prove it as executable
    tests rather than assert it. If that proof fails, this decision is
    wrong and comes back here.

    **What genuinely weakens: µarch side channels**, bounded by default-deny
    trust groups — distrusting groups never co-reside on a physical core,
    L1D + IBPB on cross-group switch, no restriction and no flush cost
    within a group. Default is one group per VM, so configuring nothing
    yields the strict behaviour. A VM that must never share a core with
    anyone uses the dedicated tier.

    **Also required, not optional:** NUMA-aware placement. Static pinning
    gave locality for free; scheduling removes it, and cross-node memory
    access is a measurable cliff this project should not reintroduce by
    accident.

    **Rejected: replacing pinning outright.** It would trade a guarantee
    that holds by construction for one that holds only if a mechanism is
    correct, on every VM, including the latency-sensitive and
    security-critical ones that have no need of over-commit.

    **Rejected: deferring to v2 as originally planned.** Guest SMP (this
    milestone) already builds most of the machinery — per-VM multi-vCPU
    contexts, per-vCPU LAPIC state, IPI delivery. Adding the scheduler while
    that work is open is far cheaper than retrofitting it afterwards, and
    doing it later would mean re-opening §6g a second time.

    Delivered by #466–#478.

    **Parameters settled 2026-08-16 (grilling session):**
    - **Over-commit ratio** is a global `[hype]` setting, `shared_overcommit_ratio`
      (default `4.0`), not per-VM or per-`isolation_group` — there is one shared
      pool per host today (§3). Admission (§6i) refuses startup if any
      `cpu_mode = shared` VM is configured while the ratio is `< 1.0`.
    - **Time-slice length** is a global `[hype]` setting, `shared_timeslice_us`
      (default `4000`, i.e. 4ms — conventional desktop-scheduler scale, short
      enough to bound worst-case latency for other shared-tier VMs, long enough
      to keep context-switch overhead low relative to it), fixed by default with
      an operator override. No per-VM override: a shorter slice for one
      latency-sensitive shared VM is a plausible future knob, but nothing today
      asks for it, and a per-VM slice multiplies SMP-20's timing proof surface
      (every distinct slice length becomes a separate case to verify).
    - **NUMA placement** prefers node-local and falls back cross-node with a
      loud diagnostic, rather than refusing admission outright — matching §6i's
      existing pattern of failing fast only for actual overcommit, not for a
      suboptimal-but-workable placement. A hard single-node-only rule would make
      small or oddly-shaped hosts un-startable over a performance concern, not a
      safety one.
    - **SMP-20's acceptance criteria** (what "proven" means, operationally) are
      explicitly left to the ticket, not recorded here — plan.md tracks
      decisions and architecture, §9 already points at SMP-20 as the pointer,
      and pulling test-acceptance detail into the plan duplicates the board and
      rots faster than the ticket does.
      See `docs/hype-cfg-spec.md` §5.1 for both new keys' full definitions.

40. **SMT siblings — decided: allocate whole physical cores, execute on every
    thread (2026-08-16).** Recorded because decision 39 left the unit of
    execution undefined, #473 offered "SMT off for the pool" as an option,
    and today's code silently discards every sibling thread.

    **The rule, in three parts.**
    - The unit of **execution** is a hardware thread. Every thread hype has
      been given may run a vCPU.
    - The unit of **allocation and exclusion** is a physical core. A core,
      with all of its threads, is owned by exactly one owner at a time: one
      dedicated VM, or one `isolation_group` in the shared pool.
    - Within one owner there is no restriction and no flush. Across owners,
      no two may occupy threads of one physical core at the same time; a
      core is drained of the outgoing owner and flushed (L1D + IBPB, SMP-18)
      before the incoming owner enters.

    **What this settles.** A dedicated VM given one 2-thread core runs on both
    of them — one vCPU, which its guest sees as two logical CPUs (decision 47
    fixes which of those numbers `vcpus` counts). A two-core shared pool
    dispatches on four threads, not two. A single VM is trivially one trust
    group, so its own threads may always occupy sibling threads of its own
    cores — that is the ordinary guest-SMP case and needs no special
    permission.

    **Rejected: running the shared pool with SMT disabled** (the alternative
    #473 offered). It idles half the machine unconditionally, including in
    the common deployment where every pool VM is in one trust group and no
    isolation rule is engaged. Core-granular allocation gives the identical
    security property — no distrusting co-residency on a core — and costs
    threads only in the configuration that actually needs it.

    **Rejected: thread-granular allocation across trust groups** (letting
    distrusting groups take sibling threads of one core). That is exactly
    the L1TF/MDS exposure §6g calls the genuinely weakened axis, and no
    flush can mitigate it, because the two owners execute *concurrently*
    rather than in turns. If an operator ever wants it, it must arrive as an
    explicit opt-in key with the risk stated, not as a default.

    **The cost, stated plainly.** The pool's allocation quantum is a whole
    core. G mutually-distrusting groups therefore need G cores concurrently
    resident, whatever each group's vCPU count. In the worst case — many
    single-vCPU distrusting groups — the pool behaves exactly as if SMT were
    off, and `C × (T − 1)` threads sit idle. That is the honest price of the
    isolation the operator asked for, and the operator's lever is
    `isolation_group`: naming one group for mutually-trusting VMs restores
    full thread density. The idle-thread count must be reported (SMP-21),
    never hidden, so a config paying this price is visible.

    **Fallback when siblings cannot be identified.** Sibling detection needs
    a trustworthy (package, core, thread) map. #378 already found firmware
    reporting every `EFI_CPU_PHYSICAL_LOCATION` as zero, repaired from CPUID
    leaf 0x0B/0x1F (Intel) and 0x8000001E (AMD), and refuses unproven
    isolation when neither source works. The same gate applies here: if
    siblings cannot be proven, hype treats every logical processor as a
    single-threaded core. That is the conservative direction — it wastes
    threads, it never pairs distrusting owners by accident.

    Delivered by #479 plus scope amendments on #186, #190, #472, #473, #477.

    **Conformance history (2026-08-19).** This decision defines the allocation
    quantum and was for a day read as also defining what `vcpus` counts. It
    does not, and the attempt to make the code match that reading (#560) was
    reverted by decision 47 and #564: `vcpus` counts **cores**, and SMT changes
    what the guest sees rather than what it is charged.

    Two things worth keeping from that day. First, **"a core is the allocation
    unit" and "a vCPU costs a core" are different statements** — this decision
    only ever asserted the first, and four sites had quietly assumed one or the
    other (#559, #560, #561, #562). Second, **admission and placement must
    spend the same currency computed by the same code**: they did not, and
    #559 was two VMs started on one APIC ID with one of them silently never
    running. They now share `core/smp_pack.c`.

    A guest is also now told its real `threads_per_core` rather than a
    hardcoded 1, because a VM on two siblings that believes it has two
    single-threaded cores cannot reason about its own internal SMT exposure.

41. **Device hotplug — decided: attaching and detaching guest-visible
    hardware while a VM runs is in v1 scope, on the buses that can
    express it; every other bus queues the change to the VM's next boot
    (2026-08-17, TERM-9/#485).** Recorded because decision 33 rejected *VM*
    hotplug and nothing anywhere said what happens to a `attach`/`detach`
    (§6b terminal, TERM-12/TERM-13) issued against a running VM.

    **The rule.** An attach or detach against a running VM applies
    **immediately** on a bus whose guest-visible contract includes runtime
    arrival/removal, and is otherwise **queued** to the VM's next boot —
    reported to the operator as queued, never refused and never silently
    dropped. A queued change is written to `hype.cfg` like any other `set`
    edit, so it survives a host reboot and applies on the VM's next start
    wherever that happens.

    **Which buses hot-attach in v1.**
    - **USB (xHCI)**: yes — hotplug is the bus's native model. hype flips
      the emulated root-hub port's connect status and raises the port
      status-change event; every guest OS already handles this path.
    - **AHCI/SATA**: yes — SATA defines surprise hotplug (PxSSTS.DET
      transitions + PxIS port-change interrupt), and AHCI-aware OSes
      handle it. hype models the port going device-present/absent.
    - **virtio-blk and NVMe (PCI-attached)**: no — arrival/removal of the
      *function itself* is PCIe hotplug, which hype's guest chipset model
      does not implement (no slot capability, no ACPI hotplug methods),
      so these queue to next boot. This is the fallback rule doing its
      job, not a missing feature: implementing PCIe hotplug machinery for
      v1 was considered and rejected below.

    **Rejected: emulating PCIe/ACPI hotplug so every bus hot-attaches.**
    It adds a chipset-model surface (slot registers, ACPI methods, guest
    OS quirks per family) whose only v1 customer is attaching a disk
    without rebooting — which the queue-to-next-boot fallback already
    covers honestly, and which USB/SATA cover immediately for the media
    cases that actually motivated TERM-12. Revisit only if a real
    operator need for runtime virtio/NVMe arrival shows up.

    **Rejected: refusing hot-attach entirely** (everything queues). USB
    passthrough's whole point (#241) is plugging a device into a running
    guest; a version of `attach` that always waits for a reboot fails the
    primary use case while being only marginally simpler.

42. **Thin-provisioned virtual disks — decided: promoted from v2 to v1
    (2026-08-18, #524). A `file:` backing image may be created holding
    almost nothing and grown as the guest writes; preallocation stays the
    default.** Reverses §13's own "stays out of v1" position, which rested
    on a premise decision 29 has since removed: hype's
    post-`ExitBootServices` writer could not assign a missing range, so a
    thin image saved nothing. It can now, per filesystem, under a stated
    crash-ordering contract.

    **Why it is worth v1 scope.** hype runs from a USB stick or a laptop's
    single disk. A preallocated 100 GB image costs 100 GB the moment it is
    made, whatever the guest uses, which is routinely the difference between
    one virtual disk and none.

    **The two creation modes** (§6b `mkdisk`, §6d, TERM-11/#507/#508):
    - **Preallocated** — every block or cluster allocated and its metadata
      written at create time. The default, and the only mode offered on a
      host volume that cannot back a thin image.
    - **Thin** — the operator asks for it explicitly. `mkdisk` reports both
      virtual size and host-allocated size after creating either mode, so
      the difference is visible rather than asserted.

    **Which host filesystems can back which thin image**, following
    decision 29 rather than inventing new rules:
    - **Thin qcow2** needs only *growth at the end* of the backing file, as
      newly allocated clusters are appended. Every writer that can extend a
      file can back it: ext, and FAT32/exFAT (whose writers allocate and
      zero-fill through the new size), and NTFS per decision 30.
    - **Sparse raw** needs *interior holes*, so it is **ext-only**. FAT32
      and exFAT have no sparse representation at all (decision 29), so the
      request is **refused, naming the filesystem of that volume** — never
      silently satisfied by producing a fully allocated file, which is the
      failure mode being designed out.
    - Growth is not free of ordering rules: new blocks are zeroed or filled
      before metadata exposes them, exactly as decision 29 requires. The
      file-backed backend must therefore re-resolve a file's ranges after it
      grows instead of resolving them once at open (#506) — that
      resolve-once behaviour is the actual defect, not the file format.

    **Over-commit is allowed, and reported.** With thin images the sum of
    every VM's declared virtual disk size may exceed host free space; that
    is the point. Admission control (§6i) keeps validating each image's
    virtual size against `target_disk_size_gb` as it does today, and
    additionally reports total committed virtual size against the host
    volume's free space. It does not refuse on that basis. Host RAM
    admission is unchanged and stays a hard refusal.

    **Running out of host space is a guest I/O error, never a host fault.**
    A write that cannot allocate fails that guest's I/O with a device error
    and is reported to the operator. It must not panic the hypervisor, halt
    other VMs, or return false success — the same rule §6g already applies
    to a faulted guest, reached here through the storage path.

    **Still refused, still v2.** Pooling extents across several physical
    disks, per-VM encryption at rest, reclaiming space a guest has freed
    (discard/TRIM punching holes back out) and any defragmentation story.
    qcow2 refcount-table growth also stays refused: a thin qcow2 sizes its
    refcount table for the full virtual size at create time, the way
    `qemu-img` does, so that refusal is never reached in normal operation.

    **Rejected: offering thin raw on FAT32/exFAT by zero-filling the gaps.**
    That is preallocation with extra steps and a misleading name — it
    allocates the space it claims to save.

    **Rejected: making thin the default.** Preallocation fails at create
    time, in front of the operator, when the space is not there. Thin defers
    that failure into a running guest's write path, which is a worse place
    to discover it, so the operator opts in.

43. **VMCS ownership — decided: a vCPU's VMCS is current on exactly one
    logical processor, its owner, and every cross-core read or write goes
    through published state (2026-08-19, #523).**

    **Why this is architectural, not stylistic.** The VMCS current pointer is
    per logical processor, and the SDM permits a processor to hold VMCS data in
    on-chip caches -- a VMWRITE may live only in that core's cache until VMCLEAR
    writes it back. So a VMCS must not be active on two logical processors at
    once, and moving one between cores requires **VMCLEAR on the core where it
    is active**, before VMPTRLD on the new core.

    **What hype did instead.** `vmx_ensure_current()` (#483) VMCLEARs and
    VMPTRLDs from the core that *wants* the VMCS -- the wrong side of the
    handshake -- and all 63 VMCS field accessors call it, so any core reading
    any field silently steals the VMCS from the core running that vCPU.
    Measured on the bare-metal Intel run of `60fb26a`: `VMCSRELOAD: count=73`,
    then one entry failed with VM-instruction error 4 (VMLAUNCH with non-clear
    VMCS), hype stopped vm0's AP at t~31s after 13,026,023 successful exits,
    and the guest -- still believing it had two CPUs -- reported
    `rcu_preempt detected stalls` on CPU 1 at t=84s, 24 times.

    **The rule.**
    - Once a vCPU is dispatched, **only its owner** makes its VMCS current.
    - **Observers read a published snapshot** the owner refreshes at each exit
      (interrupt state, debug state, exit counts). Reading a VMCS field
      cross-core is itself the violation, so the prohibition and the mechanism
      are one rule, not two.
    - **Cross-core state changes are posted, not written** -- INIT/SIPI reset
      and topology updates become pending requests the target applies before
      its own next entry, the shape the pending-vector machinery already uses.
    - **A vCPU changes owner only by explicit hand-off**: the current owner
      VMCLEARs and releases before the new owner VMPTRLDs. This is what makes
      SMP-13's shared tier (#469) legal rather than a licensed version of
      today's bug.
    - `vmx_ensure_current()` stays, as a **counted and reported violation
      detector**. Silence is what let this run for as long as it did.

    **Why it is a hard invariant.** There is no safe degraded mode: a stolen
    VMCS does not fail fast, the two cores' cached copies diverge, and the
    loser's writes vanish. Error 4 is the *lucky* case, where the inconsistency
    lands on the launch state and the CPU refuses; the unlucky case is an
    exit-control or EPT-pointer write that quietly does not take. The damage is
    deferred and non-local -- it lands on another core, in another VM, at an
    unrelated instruction. And a VMCS carries **guest state** (RIP, CR3,
    segments), drawn from one shared `g_vmcs_pool`, so a cross-core write is a
    section 6g/6j boundary matter, not only a stability one.

    **Applies to VMX; the mechanism is shared.** SVM's VMCB has no
    current-pointer or cache-writeback protocol (VMRUN takes the address in
    RAX), so the specific hazard is VMX-only. The published-state rule is
    written for both backends anyway, so diagnostics do not grow a per-backend
    shape -- the same argument #482 made for device dispatch.

    **Rejected: VMCLEAR-and-retry on error 4.** It masks the state bug it is
    recovering from, and once ownership holds it can never fire. `53f4087`
    already makes the failure loud, which is what the retry was really for.

    **Rejected: an IPI handshake so the owner VMCLEARs on demand.**
    Architecturally correct, but it pays an inter-processor round trip on every
    diagnostic read of a running vCPU. What the readers want is a snapshot, not
    live registers.

    **Rejected: leaving `vmx_ensure_current()` as the standing protocol with a
    warning comment.** That is the drift #482 removed for device dispatch: 63
    call sites auto-steal today, and the next accessor added re-breaks it. The
    rule has to be structural.

44. **Display mode -- decided: no config key; hype sets the mode nearest 1920x1080
    at boot (2026-08-19, #529).**

    **What forced the decision.** `resolution = WxH` in `hype.cfg` was the one
    config key that had to be read before `ExitBootServices`: applying it calls
    `hype_gop_mode_set(gop, ...)`, which goes through the GOP protocol, and
    `core/gop_mode_hw.c` has exactly one setter taking that protocol pointer.
    Decision 37 moves config reading into Phase 1, so "Phase 0 does no file
    I/O" and "a configured resolution applies on this boot" could not both
    hold.

    **The rule.** The key is gone. At boot hype picks, from whatever modes the
    firmware's GOP offers, the one closest to 1920x1080 -- minimum difference in
    total pixels, tie-broken toward the wider mode, so an exact match wins by
    construction. No GOP, or no mode list, keeps the firmware's current mode.
    Which mode was chosen, and why, is logged.

    **Rejected: applying the configured value on the NEXT boot.** It keeps the
    key but makes `set resolution` a two-boot operation, and the value still has
    to be persisted and read early by something.

    **Rejected: a narrow pre-EBS read of display keys only.** It preserves
    today's behaviour at the cost of the property decision 37 exists to
    establish -- Phase 0 stops doing file I/O -- for one cosmetic setting.

    **Rejected: hype writing its own display driver so it never needs GOP.**
    Priced seriously and it is not close. Real VESA is INT 10h and unreachable
    on a UEFI boot, so this means a KMS-class driver per display engine: PLL and
    clock setup, DP/HDMI link training, AUX/DPCD, EDID parsing, panel power
    sequencing, bandwidth and watermark calculation, all generation-specific.
    i915's display code alone is >100k lines and AMD's DC is larger, against one
    config key, and it contradicts section 3's "thin". The decisive cost is the
    failure mode: mode-setting that is wrong on real hardware gives a black
    panel with no console, on the serial-less laptops where that screen is the
    only diagnostic channel there is. Firmware currently guarantees a working
    framebuffer before hype touches anything, and that guarantee is worth more
    than a configurable resolution.

    A Bochs/QEMU-register-only setter, which needs no per-generation work, is a
    reasonable follow-up if live resolution changes on a rig are ever wanted; it
    would not remove the GOP dependency on real hardware.

45. **Direct kernel boot -- decided: `boot = kernel` is a first-class boot mode,
    not a test-only hook (2026-08-19, #535/#534).**

    **What forced the decision.** #534 moves the 18 self-test guests out of
    `boot/main.c` and into build artifacts "booted like any other kernel in a
    configured VM". Nothing in `hype.cfg` could point a VM at a kernel image:
    `boot` took `installer` or `disk`, and both go through OVMF. The M3 shim that
    makes a firmware-free boot possible (`core/linux_boot.c`) had existed and been
    unit-tested since M3-3, and was reachable only from one in-binary microtest --
    exactly the coupling #534 exists to remove.

    **The rule.** `boot = kernel` plus `kernel = <path>` loads a bzImage-shaped
    image into the VM's guest RAM and enters it in long mode at its 64-bit entry
    with `RSI` on the zero page. No guest firmware is in the path. Everything else
    is the ordinary VM path: the same admission, the same RAM carve, the same
    NPT/EPT build and DMA map, the same device model, the same dispatch loop, the
    same per-VM log. A kernel VM needs no storage and no `firmware` key, so those
    are waived for that mode alone; naming both a `kernel` and an `install_media`
    is refused rather than resolved by precedence. A kernel that will not load
    refuses that one VM and says why -- never fatal to the host, so one bad
    artifact in a suite config cannot take the other VMs down.

    **Guest page tables live in guest RAM.** Guest CR3 is a guest-physical
    address, and a configured VM's RAM starts at GPA 0 mapped to a host carve, so
    the tables cannot be the host-resident ones the existing microtests use. Those
    only work because they run identity-mapped, which is itself a way the
    in-binary battery differs from every real guest.

    **Rejected: a UEFI-application microtest on a FAT image, booted via
    `boot = disk`.** It needs no new load path at all, which is genuinely
    attractive. It was rejected because it changes what the tests test: OVMF has
    already enumerated PCI, programmed BARs and claimed the framebuffer before the
    first instruction of a test whose subject is often exactly that emulation, and
    it spends seconds of firmware startup per guest that executes tens of bytes.

    **Rejected: keeping the battery in-binary and only extracting the payloads.**
    It shrinks `boot/main.c` without removing the coupling: the asserts, the
    bespoke launch path and the `HYPE_SELFTEST_LIMIT` rebuild-to-bisect knob all
    survive, and a microtest still cannot use the media path real guests use --
    which is what retired ISO-2 in #452.

46. **A guest fault stops that VM, not the host -- decided: uniformly, for firmware and
    kernel guests alike (2026-08-19, #538).**

    **What forced the decision.** Ten `hype_fatal()` calls in the FW-1 dispatch loop were
    reachable from things a GUEST does: an unmodelled MSR, unmodelled AHCI/NVMe/virtio/disk
    register access, an MMIO fault hype cannot decode, an exception the guest does not handle,
    exhausting the exit budget, and leaving the loop without ever reaching idle. `hype_fatal()`
    halts the machine. That was written when hype ran one firmware guest, where a faulting OVMF
    means nothing useful is left running.

    Two things made it wrong. Several VMs are the normal case now, so one guest's fault took every
    other guest down with it -- an availability failure caused by a VM that is supposed to be
    isolated, which is section 6g's concern pointed the other way. And #534's microtest suites make
    a faulting guest an EXPECTED outcome: those guests exist to fail when hype is wrong, so the
    more effective the test, the wider the blast radius. Measured: one deliberately faulting
    micro-kernel panicked the host and took a healthy second VM with it before that VM could report
    its own verdict.

    **The rule.** A fault hype attributes to the guest stops THAT VM: lifecycle OFF, one ERROR line
    naming the VM, what its guest did, and that nothing else stopped. Its dedicated core parks. The
    host, the dashboard and every other VM continue, and the VM can be restarted from the terminal.
    A fault hype attributes to ITSELF -- a VM-entry failure, an invalid VMCS/VMCB -- stays fatal:
    that means hype is broken, and continuing would be reporting results from a hypervisor that
    cannot be trusted to produce them.

    **Uniform, deliberately.** A firmware guest gets the same treatment as a kernel guest. A
    wedged OVMF is not more fatal to the host than a wedged kernel, and a split rule ("fatal for
    firmware, survivable for kernels") would mean the blast radius of a bug depended on which boot
    mode happened to trip it. The previous split -- fatal for everything -- was never chosen; it is
    what existed because only firmware guests existed.

    **What is given up.** A halted machine to inspect. The log outlives the fault instead, which on
    a serial-less host is the more useful artifact anyway: `hype_fatal()`'s flush competes with the
    on-stick drain at exactly the moment the machine is stopping, and #522 is the standing evidence
    that the end of a run is the part most likely to be missing.

    **Rejected: absorbing the fault and continuing the guest.** It converts a located failure into
    a guest that limps with wrong data -- the "benign default MMIO catch-all" bug class this project
    has already been bitten by (#311). Stopping is a verdict; absorbing is a guess.

47. **What a vCPU *is*, on each tier — decided: a dedicated vCPU is a PHYSICAL
    CORE and SMT is a bonus; a shared vCPU is a share of scheduler time
    (2026-08-19).** Recorded because decisions 39 and 40 together left it
    unstated, and the gap produced four bugs in one day (#559, #560, #561,
    #562), each a piece of code pricing or describing a vCPU differently from
    the plan and from the other pieces.

    - **Dedicated tier — a vCPU IS a physical core.** `vcpus = N` asks for N
      whole cores and costs exactly N cores **on every host**. What the guest
      sees is whatever those cores provide: `N × threads_per_core` logical
      CPUs, so 2 per core with SMT on and 1 without. **The guest was never
      promised the sibling**, so a non-SMT host costs it CPUs and never stops
      the config fitting. On a 5950X — 16 cores, 32 threads — one core for the
      BSP leaves **15 vCPUs**, each carrying 2 threads.
    - **Shared tier — an sCPU is a share of scheduler time.** `N` sCPUs
      ("scheduled CPUs") is a weight, not hardware: more sCPUs means a larger
      share of the pool. 50 VMs at 1 sCPU each with two or three at 2 or 4 is a
      normal configuration. **A guest with N sCPUs sees exactly N CPUs**, for
      compatibility — its own OS needs a stable CPU count to configure against.
    - **The guest's CPU count is therefore DERIVED on one tier and LITERAL on
      the other**, and that asymmetry is deliberate. A dedicated VM is buying
      hardware and gets what that hardware is; a shared VM is buying time, and
      the number of CPUs it presents is a compatibility choice rather than a
      physical fact.
    - **The two tiers must never be priced by one rule.** A dedicated vCPU
      consumes a core whether or not the guest runs; an sCPU consumes hardware
      only while dispatched. Admission spends the two currencies separately
      (§6i).

    **Why cores and not threads.** The rejected alternative — `vcpus` counting
    hardware threads, so one 2-thread core yields two independently-countable
    vCPUs — was implemented for one day as #560 and reverted. Three reasons:

    - **The µarch CVEs.** Treating threads as independently allocatable units
      is one step from handing sibling threads of one core to *different* VMs.
      That is the exposure decision 40 rejects, and it is the one no flush can
      mitigate: PortSmash (CVE-2018-5407), TLBleed and SQUIP (CVE-2021-46778)
      are *contention* channels rather than errata, and both owners execute
      concurrently so there is no switch boundary to clean at. Pricing in cores
      keeps the allocation unit and the exclusion unit the same object, which
      is what §6g's argument actually rests on.
    - **It matches the isolation model of Hyper-V, KVM and Xen** — a core
      belongs to one trust domain at a time, SMT exposed only *within* it.
      (Those three differ from hype in the counting convention: their vCPU
      counts are logical processors the guest sees. The isolation property is
      the part worth matching, and it is identical.)
    - **One config fits every host.** The core cost never moves, so a config
      validated on an SMT machine cannot fail to fit a non-SMT one. Under the
      thread-counting model it could, silently, by needing twice the cores.

    **Rejected, and stays rejected: a vCPU as a logical CPU across VMs**
    (thread-granular allocation, so a 16-core/32-thread host offers 32
    dedicated vCPUs by letting two VMs share a core's threads). It doubles
    apparent capacity and breaks the one property the dedicated tier sells.
    Unlike the buffer-leak classes (L1TF, MDS/ZombieLoad/RIDL/Fallout,
    Downfall), the contention channels above cannot be mitigated on a switch
    boundary, because there is no switch. STIBP addresses only the
    branch-predictor axis. If an operator ever wants this it arrives as an
    explicit opt-in key naming the risk, never as a default or a capacity
    optimisation.

    **Naming debt, recorded.** The config key is `vcpus` and now names cores,
    so `vcpus = 1` can produce a guest reporting 2 CPUs. Renaming it to `cores`
    — keeping `vcpus` as an accepted alias for §4.1's lossless round-trip — is
    the honest fix and needs its own ticket.

    Delivered by #564, superseding #560.

48. **Not-present nested-paging entries must be L1TF-safe — decided:
    unconditional, both tiers (2026-08-19).** A not-present NPT/EPT entry whose
    address bits are zero names host physical address 0, which is real memory.
    On L1TF-affected parts a speculative load can resolve through a
    not-present entry and leave the referenced line in L1D, so those bits must
    point at nothing: hype sets the physical-address bits of every
    not-present/reserved entry above the host's supported physical width, the
    same PTE-inversion mitigation Xen and Linux/KVM use.

    Recorded as its own decision because it is the **only** mitigation in this
    family that needs neither a scheduling change nor a switch boundary — it
    works while both threads are live, which is exactly what the contention
    channels in decision 47 do not allow. It is cheap, it is unconditional, and
    it helps most on the older or unpatched silicon a bare-metal hypervisor is
    most likely to meet.

    Scope: `hype_npt_mark_range_not_present` / `hype_ept_mark_range_not_present`
    and every other producer of a not-present entry, including the initial
    zeroed state of a freshly allocated table — a zero-filled page IS the unsafe
    pattern, so allocation must not be trusted to leave a safe default.
    Related but separate, and not covered here: hype reads no
    `IA32_ARCH_CAPABILITIES` (MSR 0x10A) `*_NO` bits today, so it cannot tell
    an affected part from a fixed one and does not try to — the mitigation is
    applied unconditionally rather than gated on a feature bit hype does not
    read.


49. **A guest-visible Bochs VBE adapter is config-selected, default OFF — decided
    (2026-08-20).** Recorded because it adds a device every guest could see, and
    #549 showed the alternative was not safe.

    hype already gives every VM a **ramfb** framebuffer through fw_cfg (§6e's
    display path, `etc/ramfb`), which needs no PCI device at all — the guest
    allocates the framebuffer in its own RAM and tells hype where it is. The
    Bochs VBE adapter (`devices/bochs_vbe.c`, PCI `0x1234:0x1111`) is a second,
    independent answer to the same question, and it exists today only inside the
    in-binary VIDEO-3 self-test, which builds a private PCI bus for itself.

    **Decided: `display = none | bochs` per VM in `hype.cfg`, defaulting to
    `none`.** A VM gets a VBE adapter only when asked.

    **Why not present it to every VM.** A Linux guest with `bochs-drm` inbox
    would bind it and move its console there, away from the ramfb surface hype
    renders and away from the serial console the scripted-input runner (#280)
    drives. That changes the console of every existing guest as a side effect of
    adding a device — and the guests it would change are the ones carrying the
    highest-value hardware evidence (#527's two Alpines). A display device is not
    chipset furniture like the Q35 MCH or the ICH9 LPC; it is the thing the
    operator watches, so it must not appear uninvited.

    **Why not a mode key.** Decision 44 already settled that hype picks the
    display mode itself, nearest 1920x1080, with no config key. This does not
    reopen that: `display` selects WHICH adapter a guest is offered, not what
    resolution it runs at. Mode stays hype's business.

    **Why config-selected rather than derived from `os_hint`.** The storage split
    (§6a: AHCI for Windows, virtio for Linux/BSD) is derived from `os_hint`
    because each OS has exactly one sensible answer. Display does not: a Linux
    guest is perfectly served by ramfb, and the reason to want VBE is usually a
    specific driver or a test, which is an operator intent rather than a property
    of the OS. Deriving it would guess at intent that the operator has.

    **Consequence for the microtest suite.** VIDEO-3's port (#565) sets
    `display = bochs` in its own config, so it exercises a real PCI device on the
    VM's own bus that the guest discovers, sizes and programs itself — rather
    than the private fixture the in-binary test built. Every other VM is
    unchanged, which is the property that makes this safe to land while the
    hardware-validation guests are the critical path.

    Rejected: presenting both surfaces to one guest with no way to choose. Two
    display devices with no stated precedence is a configuration whose behaviour
    depends on which driver binds first, and that is not a thing to leave to
    chance in the component the operator uses to see what is happening.

50. **The self-test battery is guest-side and config-selected, not
    hypervisor-resident — decided (2026-08-20), closing #534.** Recorded because
    it removes a facility hype had carried since M3, and because the reason it
    had to go is not visible from the code that replaced it.

    hype used to carry 18 self-test guests inside `hype.efi` itself: each was a
    C function in `boot/main.c` that hand-built a guest — page tables, a fake
    PCI bus, a private device model — ran it, and checked the result. They were
    reached through a compile-time knob (`HYPE_RUN_SELFTEST_GUESTS`) and a
    per-index macro. They are now **guest binaries under `tests/micro/`**, run by
    an ordinary VM entry from an ordinary `hype.cfg` suite file, selected by
    configuration like any other guest.

    **Why they had to move.** An in-binary test builds its own fixture, so it
    tests hype against hype's own idea of the hardware. Four real defects were
    found the week the ports landed, every one of them invisible to the test it
    replaced: #552 (hype leaked a VMX CPUID bit to guests — the old test compared
    hype's emulation against hype's emulation), #550 (no virtio PCI capability
    chain existed at all — nothing had ever walked the guest's bus), and #565's
    two (the VBE MMIO decode only worked identity-mapped, and `VIRT_WIDTH` was
    never latched, so hype rendered correctly while a real guest driver would
    have rendered nothing). A fixture cannot find a defect in the thing it is a
    fixture for.

    **The second reason is the verdict.** A guest-side test prints
    `MICRO PASS:`/`MICRO FAIL:` on its own console, through the same log path a
    real guest uses, so the operator reads the same evidence for a test as for a
    workload — and a *missing* verdict is a failure rather than a silence
    (#558 was exactly this: a test printed PASS and the log lost it, and counters
    that were consistent with passing got read as proof of failing).

    **What this actually cost and saved.** `boot/main.c` went from 25,803 to
    23,357 lines: **2,446 removed**, against roughly 7,560 estimated when the
    epic opened. The estimate was wrong, and worth recording as wrong: it was
    built by summing function spans, and those spans included machinery shared
    with code that stays (VIDEO-2's span is the documented case). The ports also
    *added* to `boot/main.c` — the `display` key, the VBE presenter, per-VM state
    that used to be function statics — and the tests themselves are 4,334 lines
    across 16 guests, so the code did not vanish, it moved to where a guest can
    run it. The honest summary is that the epic bought correctness and a real
    verdict channel, and bought about a third of the size reduction it promised.

    Rejected: keeping a small in-binary battery for the cases with no guest-side
    equivalent. Every case turned out to have one, and a two-mechanism test
    estate means the weaker mechanism is where a defect hides.

51. **The guest's interrupt pins are exhausted, so shared lines are now the norm
    and must be computed as such — decided (2026-08-20), from #81.** Recorded
    because it is a hard limit that will shape every guest device added from here,
    and because the correct handling of a shared line is not what the existing
    code did.

    hype presents each guest a 24-entry IO-APIC, matching the ICH9 it claims to
    be. All 24 are allocated: 0-15 are the ISA lines, 16-19 the PCI device-2
    block, 20 virtio-blk and the slot-0 NVMe front-end, 21 the ICH9 SATA
    function, and 22 and 23 the two extra disk slots (#329, which is itself why
    a VM caps at three disks). **There is no 25th pin.** virtio-net (#81)
    therefore shares GSI 20 rather than taking one.

    **A shared level-triggered PCI interrupt is the OR of its devices, and has
    to be computed that way.** The pre-existing code asked each device
    separately and deasserted the line in its own `else`. That is correct only
    while at most one device on the pin can be pending — true for virtio-blk and
    NVMe, because §5.6's front-end selection means they are never both present.
    virtio-net breaks it: a VM with a disk and a NIC is the ordinary case, and
    with per-device deasserts the quiet device withdraws the busy one's
    still-pending level and the guest waits for an interrupt that was already
    taken away. #440's comment had warned about exactly this hazard on GSI 21;
    adding a third device to GSI 20 is what made it reachable. The dispatch now
    computes one pending state per line and raises or deasserts once.

    **Why not widen the IO-APIC.** The pin count is a guest-visible property of
    the chipset hype claims to be, every VM would inherit the change, and the
    blast radius is every guest's interrupt routing — against the benefit of
    avoiding arithmetic that a correct shared-line implementation needs anyway.
    Sharing is also what real hardware does: PCI interrupt pins have always been
    shared, and any guest OS handles it.

    **The consequence for future devices.** A new guest device does not get to
    assume a free GSI. It picks a line to share, and whoever adds it extends
    that line's pending computation — which is now one place per line rather
    than one place per device. If a future device genuinely cannot share (a
    latency-sensitive one where another device's ISR would add jitter), that is
    the argument for widening the IO-APIC, and it should be made explicitly
    rather than by quietly taking pin 24.

    Rejected: MSI/MSI-X for the new devices as a way round the pin limit. It
    would work and it is where this eventually goes, but hype's guest-facing
    virtio and AHCI models answer NO_VECTOR for MSI-X today, so adopting it for
    one device means building the delivery path for one device — and the pin
    shortage is not urgent enough to justify that before something needs MSI for
    its own sake.

52. **How hype learns a guest's addressing, and how it hands a frame to
    another VM — decided (2026-08-20), from #83/#84/#85.** Two choices in the
    forwarding plane that are not obvious from the code and that a later change
    could quietly undo.

    **hype answers EVERY ARP request a guest sends, with its own MAC (proxy
    ARP).** Not just requests for a configured gateway address.

    The alternative was for hype to hold each guest's segment address, mask and
    gateway in `hype.cfg`. Rejected because that configuration would have to
    stay in step with whatever the guest's own OS was configured with, and would
    be wrong the first time someone changed it inside the guest — a mismatch
    whose only symptom is a network that does not work. Proxy ARP removes the
    question: whatever the guest believes its first hop is, the ARP for it
    resolves to hype, and every packet arrives here to be routed.

    It also solves the reverse problem. hype has to build an Ethernet header
    addressed to the guest for every inbound frame, and nothing tells it the
    guest's MAC. An ARP request carries the sender's MAC and IP together, so the
    guest's own first ARP teaches hype exactly the pair it needs, before any
    IPv4 flows.

    **Claiming to be every address is safe here because hype IS the router, not
    a peer on a shared segment.** Each guest's NIC is alone on its own
    point-to-point link (§6e's default isolation), so there is no other host
    whose address hype could be stealing. On a shared L2 segment the same
    behaviour would be a hijack; on a link with exactly one guest and one router
    it is the router doing its job. This is a reason the isolation default has to
    stay: proxy ARP is only correct while it holds.

    **What it cost, and how that was closed.** hype no longer knows the guest's
    netmask, so it cannot tell an on-link destination from a remote one — and a
    packet for a guest that has not yet ARPed would fall through to NAPT and go
    out the physical port with a private destination. So hype also records the
    addresses each guest ARPS FOR: an ARP request is the guest stating "I believe
    this is on my link", which is the information a netmask would have given.
    A destination in that set is forwarded to a peer or dropped, never
    translated. Without it, whichever of two peers booted first leaked its
    opening packets to the wire.

    **A frame for another VM goes through a per-VM MAILBOX, never straight into
    the peer's receive ring.** This is a locking decision, not buffering.

    The transmit path runs with the SENDING VM's device lock held — a guest's
    MMIO fault took it on the way in. Writing into the peer's receive ring there
    would mean holding one VM's lock while acquiring another's, and two guests
    transmitting to each other simultaneously would deadlock both cores. So the
    sender only ever touches the peer's inbox lock, and the pump — which holds no
    device lock — copies a frame out under the inbox lock, RELEASES it, and only
    then takes the target's device lock. **Nothing ever holds an inbox lock and a
    device lock at the same time**, which removes the cycle rather than making it
    rarer. A future change that "simplifies" this by delivering directly
    reintroduces a deadlock that only appears under simultaneous bidirectional
    traffic.

    The mailbox holds four frames and drops when full, counted. It is a hand-off
    point, not a queue with a queueing discipline — a real network drops when a
    buffer fills.

    **hype rewrites the Ethernet source to its own router MAC when forwarding
    between guests**, rather than passing the sending guest's MAC through. hype
    is a router between two isolated segments, not a bridge across one: the
    receiving guest's ARP cache maps its gateway to hype, so a frame from the far
    guest's MAC would arrive from an address it has no route to — and it would
    leak the other guest's hardware address across a boundary that exists to keep
    them apart.

## 11. Pre-M0 readiness checklist

Concrete, actionable items to close out before M0 work starts, beyond what
§9's milestone list already covers:

- [x] `git init` the repository; add a `LICENSE` file with the full GPLv3
  text (not just the mention at the top of this plan) and a `.gitignore`
  for build artifacts (`*.efi`, `*.o`, EDK2 build output directories, etc.).
- [x] Install and pin versions for: the C cross-toolchain targeting
  `x86_64-unknown-uefi` (clang/lld or GNU-EFI, per §8), QEMU, and an OVMF
  firmware image build — record exact versions used so "works on my
  machine" doesn't creep in later.
- [x] Confirm Secure Boot can actually be disabled on both the Intel and
  AMD test machines (§10 decision #18) — needed for §10 decision #5 to hold
  in practice, not just in the plan.
- [x] Confirm both test machines expose a way to get boot-time output before
  trusting GOP/framebuffer text rendering — a physical serial port (or
  equivalent) as the fallback debug channel if `hype.efi` fails before GOP
  init succeeds.
- [x] Settle the debugging workflow: QEMU `-s -S` + GDB attached to a debug
  build of `hype.efi` with symbols loaded, as the primary loop; serial
  logging as the channel that also works on real hardware where GDB-over-JTAG
  isn't assumed to be available.
- [x] Write the minimal freestanding primitives M0 will immediately depend
  on: a tiny `printf`-equivalent over UEFI `ConOut` (text-mode, pre-GOP) for
  M0's own output, and a panic/assert stub that halts cleanly with a message
  — small enough to write once, needed by literally every milestone after.

## 12. Suggested first PR

Scaffold `/boot` with a minimal UEFI app that: prints "hype" to the UEFI
console, dumps the memory map, and cleanly returns `EFI_SUCCESS` — the
skeleton to build on for M0, plus the repo layout from §7 and a build script
(the lightweight clang/lld-or-GNU-EFI pipeline per §8, not EDK2 — that's
reserved for the guest firmware blob) that produces a bootable `hype.efi`
runnable in QEMU+OVMF.

## 13. Future work (v2+, explicitly out of scope for v1)

Ideas captured here are deliberately **not** on any project-board milestone —
recording the intent now so it isn't lost, without pulling it into v1's
scope or weakening any v1 hard invariant to make room for it. Nothing here
should be implemented without first promoting it to a real board milestone
and, if it changes a v1 decision, updating §10 explicitly (per AGENTS.md's
own "keeping plan.md and the board in sync" rule).

- ~~**Real vCPU scheduler, alongside (not replacing) 1:1 exclusive pCPU
  pinning**~~ (noted 2026-07-14; expanded 2026-08-12) — **PROMOTED TO v1 on
  2026-08-16.** No longer future work. See §10 decision 39 for the model and
  the re-derived §6g argument, §3 for the two tiers, and #466–#478 on the SMP
  milestone for the delivery plan. Kept as a stub rather than deleted so the
  promotion is visible to anyone who remembers this section containing it.
- ~~**Thin-provisioned virtual disks**~~ (made explicit 2026-08-17,
  TERM-9/#485) — **PROMOTED TO v1 on 2026-08-18 (#524).** No longer future
  work. See §10 decision 42 for the model and what it still refuses, §6d for
  the two creation modes, and #506/#507/#508 on the STORAGE milestone for the
  delivery plan. Kept as a stub rather than deleted so the promotion is
  visible to anyone who remembers this section containing it.
- **Memory ballooning, for dynamic per-VM RAM allocation with a
  configurable floor and ceiling** (noted 2026-07-14). v1's admission
  control (§6i) sizes each VM's RAM as a fixed amount decided at start
  and never revisited; v2 would let a VM's actual resident RAM float
  between an operator-configured floor and ceiling, reclaiming unused
  memory back to the host (or other VMs) under pressure. This almost
  certainly needs a **guest-side driver** cooperating with the host (the
  same shape as virtio-balloon: the guest OS driver "inflates"/"deflates"
  a balloon of pages it stops using, which the host can then actually
  reclaim) — it is not something the hypervisor can safely do unilaterally
  from outside the guest, since only the guest OS knows which of its own
  pages are genuinely free. Implies a new guest-facing device (likely
  virtio-balloon itself, for the same guest-driver-availability reasons
  NET-2/M5 already lean on virtio for net/blk) plus new `hype.cfg` surface
  for the floor/ceiling and probably a host-side reclamation policy
  (§6i's admission-control math would also need to account for "ceiling,"
  not just a fixed size, when validating total host RAM commitment).

- **Web API/UI for remote management, with multi-hypervisor
  master/mesh linking** (noted 2026-07-14). v1 has no remote management
  surface at all -- everything is local (hype.cfg on the host's own ESP,
  local console/serial output). v2 would add a web API/UI allowing remote
  management of a single hypervisor instance, plus a way to link multiple
  "secondary" hypervisor instances under one "master" for centralized
  management, or (an alternative topology to design between, not both by
  default) have instances manage each other directly as a mesh with no
  single master. Security and encryption are explicitly paramount for
  this surface, not an afterthought -- this is a new, network-exposed
  attack surface on a type-1 hypervisor, so authentication, transport
  encryption (TLS at minimum), and authorization between instances all
  need to be designed in from the start, not bolted on. The UI itself
  should read as corporate-yet-modern, not a bare admin-panel aesthetic.
  This is a large, separate subsystem (its own network stack usage
  building on NET-*, a new API surface, a new UI, and a new inter-
  instance trust/protocol model) -- deserves its own dedicated design
  pass (auth model, wire protocol, master-vs-mesh topology choice) before
  promotion to a real board milestone, not just an API bolted onto existing
  per-VM management code.

- **Dynamically-sizable (thin-provisioned) virtual disks, pooled across
  one or more physical disks, encrypted and isolated per VM** (noted
  2026-07-14; corrected 2026-08-18). v1's storage model (M5's
  `blk_backend`, M10's physical-disk target) is a virtual disk mapped to ONE
  `file:` backing file or one whole `physical:` device. **That single file
  may be thin and grow on demand** -- promoted to v1 as §10 decision 42 --
  so what remains v2 here is pooling and encryption, not growth. v2
  would let a virtual block device draw its space from a pool (allocated
  space backed by extents drawn from a shared pool spanning part or all of
  one or more physical disks, not a single file), so multiple VMs' virtual
  disks share the same underlying physical capacity instead of each
  needing its own dedicated, fully-reserved region. Two invariants that
  must hold regardless of pooling: (1) **encryption** -- every VM's data
  at rest, encrypted, with per-VM (not shared/global) key material, so
  compromising one VM's stored data doesn't expose another's; (2)
  **isolation** -- a VM must never be able to address, read, or infer the
  existence of another VM's extents within the shared pool, even though
  they physically coexist on the same disk(s) -- this is the same
  guest-isolation invariant (AGENTS.md) M5/M10's per-VM `blk_backend`
  already needs, just harder to keep true once backing storage is
  literally shared/interleaved rather than physically separate files/
  devices. This is a substantial new storage-management layer (an extent
  allocator, a pool-spanning-multiple-disks abstraction, per-VM key
  management/rotation, and probably a background reclamation/defrag
  story for thin-provisioned space) -- deserves its own design pass
  before promotion to a board milestone, likely building on M5's
  `blk_backend` vtable rather than replacing it.

- **Remote management of hype itself** (noted 2026-08-15, confirmed as a v2
  goal alongside §10 decision #36). §6b keeps the v1 dashboard local-only and
  decision #36 rejects any listening socket in the hypervisor core (#406), so
  v1 has no remote administration of any kind. The intent to have it in v2 is
  recorded here so the v2 design starts from a position rather than by
  reopening the rejected ticket. Two constraints that decision #36's reasoning
  already fixes, and which any v2 design must answer rather than sidestep:
  (1) **the consumer comes first** — what is being managed remotely, by whom,
  and authenticated how, before any transport is chosen; and (2) **the
  hypervisor core is the worst available place to host the service**, because
  it is ring-0 with the IOMMU, every guest's NPT/EPT and the physical disks,
  and it has no process to lose (no privilege left to drop, nothing to
  restart). The direction that follows is decision #9's own shape applied one
  level up: run the management service **in a guest** — which already has its
  own OS, hardened network stack and contained blast radius — and give it a
  narrow, explicitly-modelled host-side hook (a small, versioned, strictly
  validated control channel, in the spirit of §6j's device-emulation trust
  boundary) rather than putting a parser and a listener in the trusted core.
  A management VM privileged enough to control other VMs is itself a new
  trust tier and needs its own §10 decision when promoted; it must not
  silently become a hole in the §6g/§10 guest-isolation invariant.
