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

Non-goals (v1): live migration, SR-IOV, nested virtualization, nested-paging
tricks beyond basic EPT/NPT, VM memory snapshotting (live RAM state), VirtIO
ballooning. (PCI-e device passthrough, GPU included, was a flat non-goal until
2026-09-03; decision #80 made it STRETCH-track work in progress, #699, behind
the authorization boundary that decision states. SR-IOV stays out.) (A remote management web API and web UI were a v1
non-goal until 2026-09-03; decision #79 promoted them to v1, the MGMT
milestone.)

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
- IOMMU (VT-d/AMD-Vi) required only on a host that assigns a device to a
  guest (decision #80, #699): AMD-Vi first, VT-d after, behind one driver
  interface. A host with no `backing = passthrough` device in `hype.cfg`
  needs no IOMMU and never touches it.

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
- **Removable USB mass storage presented to guest** (a fourth front-end,
  #446, plan.md §10 decision 55): a `.img` attached over an EMULATED,
  guest-visible xHCI controller + USB Mass Storage (BOT/SCSI) class device,
  so the guest sees *removable USB media* rather than a fixed disk. This is
  what most real Windows/Linux installers are written for (Windows Setup's
  partitioning, several Linux live-media detection paths, driver-injection
  flows all key off "is this removable USB media"). It is a substantial new
  device-emulation surface — a guest-facing xHCI (ring/TRB processing,
  port/slot/endpoint state machines) plus the MSC class on top — comparable
  to the AHCI/NVMe guest-facing work, and DISTINCT from `core/xhci.c`, which
  is hype's HOST-side driver for real controllers. It is backed through the
  same `hype_blk_backend_t` vtable the other front-ends share, so file- and
  physical-backed both work for free, and selected by `bus = usb-msc` in a
  `[disk.*]` entry. Decomposed into sub-tickets (controller bring-up, MSC
  class layer, config wiring, real-guest validation) rather than one change;
  the guest xHCI is default-absent, exactly as `tpm` and the SB firmware are,
  so no VM pays for it unasked.
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
- **Console output device**: the dashboard draws on the GOP framebuffer that
  UEFI hands hype, and that is the only display path in v1. When a PCIe GPU is
  passed through to a guest (#699, gated on #700), that framebuffer belongs to
  the guest and hype loses its screen. Decision #77 names the replacement: a
  USB display adapter, driven by hype's own xHCI stack, becomes the host
  console's display target, chosen at boot when the GOP device is a
  passthrough GPU. The dashboard's drawing code targets a linear framebuffer
  either way; only the sink changes. Ticket #793.
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
- **Host input devices are hot-pluggable** (decision 73, 2026-08-27). The
  operator's keyboard and mouse are physical USB devices on hype's own
  controllers, and unplugging one and plugging it back in must leave it
  working — a hypervisor whose only input surface is a keyboard cannot treat
  the keyboard as a boot-time constant. Detection is event-driven (xHCI Port
  Status Change Events for root ports, each hub's status-change interrupt
  endpoint for anything behind a hub) rather than a polled rescan, and the
  same mechanism covers USB mass storage arriving and departing, which is the
  harder half because hype boots from and logs to it (decision 28).
- **The GOP dashboard is the local surface; remote administration is served
  over the network (decision #79, 2026-09-03).** Until that decision this
  bullet read "explicitly local-only for v1 — no serial or network exposure",
  and decision #36 sharpened it to "hype never accepts an inbound connection".
  Both are superseded for one service only: the management API and web UI of
  the MGMT milestone (#500-#504), a TLS-only HTTPS listener inside the
  hypervisor core, disabled by default, authenticated before anything but a
  bounded request parser runs. The dashboard itself is unchanged: it still
  owns the physical framebuffer and keyboard, still takes the leader chord,
  and the remote UI is a client of the same command table (#459), never a
  second path into VM state. No serial protocol is added; serial stays a
  log/console output. Every other listener remains forbidden (§6e).

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
  `core/host_pci.c`. **USB Ethernet adapters are a second host-NIC source**
  (decision #78): the USB CDC-ECM/NCM class driver first, vendor chips
  (ASIX AX88179, Realtek RTL8153) after, each a driver behind the same NIC
  vtable, transported by hype's xHCI stack instead of a PCI DMA ring. A
  machine whose PCIe NIC is passed through to a guest, or that has none to
  spare, still gets an uplink. Ticket #794.
- **Forwarding plane, never an endpoint** (decision #36): host NAT genuinely
  requires protocol code — Ethernet framing, ARP for the uplink, IPv4 header
  parsing, UDP/TCP address/port rewriting with checksum fixup, and a
  connection-tracking table — and all of that is in scope. What is **not** in
  scope is hype acting as a network *peer*: no sockets API, no TCP state
  machine hype drives as an endpoint, no reassembly (over-large fragments are
  dropped, not reassembled), and **no listening socket of any kind** in the
  hypervisor core except the two named here. hype may rewrite a packet passing
  between a guest and the wire; hype is otherwise never the address a packet
  is sent to. The first exception is the uplink **DHCP client**, which NAPT
  needs to obtain hype's own uplink address: outbound-initiated only, one
  transaction at a time, replies matched against hype's own outstanding
  transaction ID, with a static-address setting in `hype.cfg` for operators
  who prefer to disable it. The second is the **management listener** of
  decision #79: one TCP port, TLS-only, off by default, with its own TCP state
  machine (MGMT-2) that serves that port and nothing else -- the forwarding
  plane does not gain a sockets API, and no other component may bind a port.
- **Guests may listen; hype may not, except on the management port.** The rule
  above constrains the hypervisor, not the guests. A guest running a server on its own virtual NIC
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
  stopped. Implemented (#177): read once on the BSP in Phase 1, applied per
  VM by name. Only an explicit `stopped` holds a VM back — a missing record,
  a refused one, or a VM added to `hype.cfg` since the last shutdown all
  keep the default and start, because an absent record must never read as
  "stop everything" (the first boot on a fresh stick has none, and a host
  that came up with nothing running because of that looks exactly like one
  that failed to boot its guests). The record is not consumed, so it also
  covers a power cut hype could not catch. This is explicitly a **restart-to-the-same-run-state**
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
- **No guest gets UNAUTHORIZED direct hardware access** (amended 2026-08-23,
  decision #80 on 2026-09-03): physical disk (§6d) and physical NIC (§6e)
  access are always mediated through the hypervisor's own host-side driver
  plus an emulated guest-facing frontend (virtio-blk/AHCI, virtio-net/
  e1000-class). The one exception is a PCI function an operator has
  explicitly assigned to one VM by durable identity, with its DMA confined to
  that VM's GPA range by the IOMMU and its interrupts remapped to that VM's
  vCPUs (decision #80). For every other device, the validation above is the
  *only* thing standing between a guest and the host for storage/network I/O,
  exactly as before: the IOMMU is required only on a host that assigns a
  device, and its presence does not relax any software check.

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
   (§9) rather than as a subsequent optimization pass. *Reaffirmed
   2026-08-21: the §14.3 gap analysis found the code diverged (AVIC opt-in
   default-OFF, no Intel APICv at all); decision 58 sets the path back —
   enabled by default on both vendors once each is bare-metal-validated.*
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
    - ~~Storage HBAs migrate opportunistically, never big-bang.~~ **REVERSED
      2026-08-22 (#426):** the working AHCI/NVMe/xHCI drivers migrate onto the
      shared PCI/DMA/IRQ facility as a single tracked ticket covering all
      three, not one-at-a-time as each is separately judged worth it. The
      reasoning below for staying mechanical does not vanish — it becomes a
      constraint on how the full migration is done: each HBA's protocol-
      specific structures (AHCI command tables/FIS, NVMe SQE/CQE, xHCI TRBs)
      are untouched; only the generic index/wrap/phase math and the buffer-
      slot bookkeeping move onto `core/host_pci_dma.c`/`host_pci_irq.c`, as a
      mechanical, behaviour-preserving extraction verified against the
      existing unit suite and a full `hype.efi` link, not a rewrite. Where an
      HBA's shape does not reduce to the shared primitives without distorting
      it (AHCI's single-outstanding-command-per-port model has no ring to
      migrate), the ticket documents that explicitly rather than forcing a fit.
      What stays true: a refactor purely for symmetry, with no unit-test/link
      verification backing it, is still not a win (decision #17 / the §6j "one
      place to forget the check" reasoning) — the full-migration scope changes
      *when* all three move, not the discipline used while moving each one.
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

    **Amended 2026-09-03 by decision #79 (#492 MGMT-1).** The "no listener"
    conclusion is reversed for exactly one service, the management API/UI
    (MGMT-2..6). Everything else in this decision stands: the NAT forwarding
    plane still translates and never terminates, no sockets API exists for it,
    and no component other than the management listener may bind a port. The
    arguments the HNET-9 rejection below rests on are not withdrawn; decision
    #79 turns them into requirements the listener must meet. #406 stays
    Rejected -- the MGMT tickets supersede it rather than reopen it.
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

53. **The virtual switch is a learning bridge whose members keep their real
    MACs, and hype's proxy ARP yields to the members — decided (2026-08-21),
    NET-6 #223.** Three choices inside the §6e opt-in shared L2 segment:

    **Bridge, not router, within a switch.** Frames between members are
    delivered verbatim — real source MAC, no header rewrite. Decision 52's
    router rewrite exists to keep hardware addresses from leaking across an
    isolation boundary; a switch's members were explicitly configured onto ONE
    segment, so between them that boundary does not exist and bridge semantics
    (ARP between members, member-run DHCP, mDNS) are the point of the feature.
    Decision 52 stands unchanged for every VM not on a switch and for every
    pairing that crosses a switch boundary.

    **hype answers a member's ARP request only when no member has been learned
    to own the asked-for address.** Decision 52's answer-everything proxy ARP
    would hijack member-to-member traffic on a shared segment (its own text
    says so: "On a shared L2 segment proxy ARP would be a hijack"). The
    alternative — configuring each switch's subnet so hype knows what is
    on-link — was rejected for decision 52's own reason: config that must
    mirror what is set inside the guests is wrong the first time they diverge.
    Cost, accepted and documented: a member whose address hype has not yet
    learned can race hype's answer once around boot; the member's own flooded
    reply re-teaches the asker, so the race self-heals with ARP's normal
    refresh.

    **Unknown destinations flood, and a full MAC table evicts round-robin
    rather than refusing to learn.** Flooding is the correct bridge behaviour
    for an unlearned address and is bounded by the switch's own membership; a
    table that refused new entries would flood forever, which is correct but
    permanently slow and looks like working sluggishness. The table is
    per-switch, mutated only under the sending guest's device lock, and a stale
    read costs one flooded frame — never a delivery outside the switch, which
    is the property that matters.

54. **The guest TPM 2.0 is implemented in-tree, not integrated from
    ms-tpm-20-ref/libtpms -- decided (2026-08-21), #433.** The reference
    stacks assume a crypto library, a heap and an NV-storage abstraction that
    a freestanding ring-0 module with no libc and no allocator does not have,
    and pulling one in to answer a compatibility probe is a poor trade. The
    scope is exactly what a Windows fTPM detection and a Linux `tpm_crb`
    driver touch: Startup/Shutdown/SelfTest state gates, GetCapability
    (family "2.0", the SHA-256 PCR bank, the command list), PCR_Read/Extend
    over 24 PCRs with real SHA-256 chaining, and GetRandom/StirRandom on
    host-mixed entropy. Every other command answers TPM_RC_COMMAND_CODE
    honestly. EK/SRK creation, NV storage and attestation-adjacent commands
    are a named follow-up -- they need key generation, which is where
    integrate-vs-implement is revisited.

    Two shape choices worth recording. **The CRB completes synchronously**:
    the spec's only contract on CTRL_START is that it reads back 0 when the
    command is done, and completing before the guest can re-read it is legal
    and removes a whole async state machine. **The TPM2 ACPI table is per-VM
    but the DSDT MSFT0101 device node is not yet emitted**: the DSDT is a
    static compiled blob shared by every VM, and a TPM node in it would
    advertise a TPM to VMs that configured none, whose CRB window is then
    unmapped. The TPM2 table (which carries the CRB control-area address) is
    what binds `tpm_crb` and what a compatibility probe reads; the DSDT node
    for Windows needs a per-VM SSDT overlay and is the follow-up's.

55. **Guest-facing removable USB mass storage is a new emulated front-end,
    decomposed rather than landed whole -- decided (2026-08-21), #446.** The
    ask is a `.img` a guest sees as removable USB media, which needs an
    EMULATED guest-visible xHCI controller plus a USB Mass Storage class
    device on top -- a surface the size of the AHCI or NVMe guest-facing
    work, not a config addition. It is deliberately separate from
    `core/xhci.c` (hype's HOST-side driver for real controllers): the two
    share the USB *protocol* structs but nothing else, and conflating them
    would couple a guest-facing model to a host datapath the #343/#377
    corruption work carefully serialised. Backed through the existing
    `hype_blk_backend_t` vtable, so file/physical backends come for free, and
    the guest xHCI is default-absent (a VM without a `bus = usb-msc` disk
    gets no controller), the same posture `tpm` and Secure Boot take. The
    breakdown, each its own board ticket: (a) the guest xHCI controller PCI
    function + ring/TRB/port/slot/endpoint state machine; (b) the USB-MSC
    (BOT/SCSI) class device over it, backed by `hype_blk_backend_t`; (c) the
    `bus = usb-msc` config wiring + admission; (d) real-guest validation
    (Windows Setup and a Linux live-media path both seeing removable USB).
    Landing (a) before (b) before (c) is the natural order; (d) gates the
    epic's close.

56. **Write durability on a device with no cache-flush barrier -- decided
    (2026-08-21); revised (2026-08-21) after #596's real root cause landed as
    decision 57.** hype's on-medium FAT32/exFAT/ext writers keep the
    filesystem crash-consistent by ORDERING: a metadata pointer that reaches a
    block (a FAT link before the directory size that spans it, #377; an exFAT
    DataLength; an ext journal commit) must be durable before the pointer is
    published. That ordering was enforced with a cache-flush barrier
    (`SYNCHRONIZE CACHE` on USB MSC), invoked through the fs-agnostic
    `fs->sync` callback.

    **Attribution correction (2026-08-21).** This decision originally claimed
    the mechanism below explained #596. It does not: #596 reproduced with FUA
    active, on a SATA-USB SSD whose `SYNCHRONIZE CACHE` works, and in QEMU. The
    real cause was a cross-core race in the log drain (decision 57). FUA remains
    correct and stays: a barrier no-op that reports success is still a lie, and
    a flush-rejecting stick still needs per-command write-through for the #377
    ordering. Only the "this fixes #596" claim is withdrawn.

    **The defect this DOES fix (durability on flush-less media).** Some cheap flash sticks
    reject `SYNCHRONIZE CACHE` as an unknown opcode. #516 handled that by
    latching `sync_cache_unsupported` and treating the barrier as a no-op that
    RETURNS SUCCESS -- on the assumption "rejects the flush ⟹ cacheless ⟹
    nothing to flush." That assumption is UNSOUND: such a device can still
    buffer/reorder/lose writes. So `flush_metadata` believed it had ordered the
    FAT link, published the larger size, and the device then lost the
    once-written tail FAT sectors while the repeatedly-rewritten directory entry
    survived -- a window for exactly the `dirent size > cluster chain` state
    #464 forbids. A no-op that reports success is a lie the whole ordering
    rests on.

    **Decided.** When the device has no working flush barrier, hype sets the
    SCSI **FUA (Force Unit Access)** bit on every `WRITE(10)`. FUA is
    per-command write-through: the write completes only when the block is on the
    medium, so writes land in issue order and a metadata pointer can never
    outlive the block it names -- the ordering the barrier used to provide,
    obtained without a flush the device refuses. The fix lives at the shared
    block layer (`core/xhci_hw.c` `hype_xhci_msc_write`), NOT in any one writer,
    because the missing guarantee was in the fs-agnostic sync path: one change
    covers FAT32, exFAT and ext. It applies ONLY to devices that reject
    `SYNCHRONIZE CACHE`; a device with a working flush keeps write-back speed and
    the flush barrier unchanged.

    **Alternatives considered.** (i) Keep the no-op but have each writer
    read-back-verify the chain before publishing the size -- rejected: a device
    that serves un-durable buffered data on reads (read-your-writes) confirms a
    link that is later lost, so read-back proves visibility, not durability. (ii)
    Refuse durable appends on such a device -- rejected: it disables logging to
    exactly the cheap boot sticks the field uses. (iii) Per-writer fixes --
    rejected as redundant once the block layer is correct.

    **Limit, stated plainly.** If a device ignores FUA as well (accepts it but
    does not force to media), hype cannot manufacture a durability the hardware
    refuses; such a device is not crash-consistent for any writer, and the
    operator should use `SYNCHRONIZE CACHE`- or FUA-honouring media for durable
    or `physical:` writes. Efficacy on a given controller is a
    hardware-validation item, not a claim the code can make on its own.

57. **#596 root cause: the #239 single-owner rule must cover EVERY sink drain
    -- decided (2026-08-21).** hype's logging is single-writer by design: every
    core appends records to the in-RAM capture buffer (core/logbuf.c), and only
    the BSP drains that buffer to the USB/FAT sinks (#239), because the FAT32
    writer's shared state (the single-sector FAT cache, the allocation cursor,
    each file's size/tail_cluster) has no lock -- ownership IS the lock.

    **The defect.** `usb_log_flush()` enforced #239 only for its combined-sink
    half (inside `usb_log_flush_limit()`). Its split-sink completion loop
    called `hype_log_sink_flush_budget(&g_vm_log[vi], ...)` DIRECTLY, with no
    ownership check -- and `usb_log_flush()` is called from every guest's
    dispatch loop on that guest's own AP, where the author relied on the
    internal guard making the call a no-op. Result: up to four cores ran
    `hype_fat32_append` on the same file concurrently. Racing on `f->size`, a
    32 KiB cluster-boundary extension was skipped (each core computed the
    offset-in-cluster before the other advanced the size), the append wrapped
    back into the tail cluster, and `flush_metadata` published a size the chain
    does not reach: fsck's `file size > cluster chain length`, one leaked
    cluster, wrong free count -- #596's exact signature. This is why only
    `\VMn.LOG` files ever corrupted (`\HYPE.LOG` drains only through the
    guarded path), why it was intermittent on hardware (the AP flush must race
    the BSP at a cluster boundary), and why it was device-independent (SanDisk
    stick, SATA-USB SSD, QEMU usb-storage alike -- FUA/decision 56 could not
    fix it).

    **Evidence.** A compile-gated in-RAM journal (`-DHYPE_596_JOURNAL`,
    core/fat_write_fs.c) recording every allocation, FAT-entry set, sector
    write and size publish, stamped with the executing core's APIC ID, plus a
    publish-time chain audit that dumps the journal over raw serial on the
    first divergence. In QEMU (tools/596/run-596-qemu.sh) the dump showed
    cores 0, 1 and 2 interleaved in one file's append stream, the chain
    skipping the racing core's cluster (`315 -> 317` with 316 leaked), and
    return-address records placing the AP calls in `usb_log_flush()`'s split
    loop. The guard itself never misfired (zero guard-pass records on APs):
    the loop simply never consulted it.

    **Decided.** `usb_log_flush()` now checks `usb_log_this_core_owns_usb()`
    at entry, covering the split loop. The rule going forward: ANY code that
    touches a sink or `g_hype_log.fs` -- append, drain, create, or FS-LEVEL
    READ (`hype_fs_read_at` mutates the shared FAT cache and seek state, so
    reads are not innocent) -- must either run under the #239 guard or post a
    request to the BSP (the #454 vars-service mailbox pattern). Block-layer
    reads may stay cross-core; they are serialized by blk_usb's ticket lock
    and touch no FAT state.

    **Alternatives considered.** (i) An fs-level lock so any core may write --
    rejected: it converts guest dispatch latency into USB-transfer waits on
    every log burst, and single-owner is already the working design; the bug
    was a leak in the funnel, not the funnel. (ii) Per-sink core affinity --
    rejected: all sinks share ONE `hype_fs_t` (decision behind #338), so
    per-sink ownership still races the allocator and FAT cache.

58. **APICv/AVIC enabled by default on both vendors -- decided (2026-08-21).**
    Decision 6 requires hardware-accelerated interrupt delivery from the
    start; §14.3 found the code diverged: AVIC is build-time opt-in default
    OFF (#193), and Intel has no APICv path at all. The ruling: the opt-in
    default-OFF state is INTERIM ONLY. Target state on both backends is
    accelerated interrupt delivery ON BY DEFAULT wherever the capability MSRs
    grant it, with trap-and-emulate as the capability-absent fallback --
    never the preferred path on capable hardware.

    The gate to flipping each vendor's default is one bare-metal validation
    run on that vendor (#600 AMD, #605 Intel; #599 is the Intel
    implementation): a working bare-metal guest must not regress on the next
    run (#193's own rule). Enabling before that validation was rejected --
    an interrupt-delivery regression on real hardware costs a full
    cold-boot-and-photograph cycle to even see. Keeping opt-in indefinitely
    was rejected -- it silently makes the trap path the de-facto product,
    which decision 6 already ruled out.

    **Boot-D finding (2026-08-22), and what it changes.** The AMD gate did not
    move. A flag-on build ran 26 minutes on bare metal and logged no AVIC line
    at all -- the enable and the banner both sit in `fw_1_vm_reinit()`, whose
    only caller is the VM-restart path, so a normal boot never reaches them,
    and the state they point at is one 4 KiB backing page shared by every vCPU
    of every VM with `max_physical_id = 0`. #193 delivered detection and a
    VMCB field write, not a working AVIC. #640 carries the implementation
    (per-vCPU backing pages, populated physical/logical ID tables, real
    handling for the incomplete-IPI and no-accel exits, and one stated owner
    of guest APIC state -- the software LAPIC model or the AVIC page, not
    both). #600 is blocked on it. Nothing about the ruling changes: the target
    is still ON by default per vendor. The correction is that "flip the
    default after one validation run" presumed a feature that was ready to
    validate, and on AMD it is not.

59. **A guest's console must not be silently lossy -- decided (2026-08-22).**
    Boot D read as a wedged guest: Alpine reached its login, took the scripted
    `root`, printed the motd as far as "You may" and then said nothing for 17
    minutes. The guest was healthy and idle at a live shell the whole time
    (`GUESTPC ... lastreason=0x78` at the idle RIP). hype had dropped 5941 of
    41199 console characters (`UARTTX: COM1 written=35258 dropped=5941`), and
    the shell prompt was inside the dropped burst.

    The cost is not cosmetic. `fw_1_drain_uart_console()` is what feeds
    `fw_1_script_feed()`, so a dropped byte is a byte no expect pattern can
    ever match: the input script stalled, #211's marker was never written, and
    the whole 26-minute run yielded nothing on its own bar. Any past run
    judged "guest wedged after login" is now suspect -- #637 first among them.

    **Decided.** The guest UART model applies BACK-PRESSURE and never claims a
    byte it did not queue (#639): LSR reports the transmitter ready only while
    the TX ring has room, a THR write to a full ring raises no THRE, and the
    dequeue that first makes room raises THRE as a counted 0->1 edge (#512's
    mechanism) to wake an interrupt-driven writer. The ring is 4 KiB so the
    common burst never reaches back-pressure. `UARTTX` now separates
    `stalled` (the guest waited, nothing lost) from `dropped` (loss), and
    after this a nonzero `dropped` means output is missing from the log.

    **The general rule.** A model that reports a resource permanently ready
    over a buffer that can fill will lose data, and the loss is invisible to
    both the guest and the log reader. Where hype models a device the guest
    can outrun -- and the drain is the BSP's alone, by the #239 rule --
    readiness must be reported from the buffer's real state, not asserted.
    Silent truncation of a diagnostic channel is worse than a stalled guest:
    a stall is visible in one counter, a gap is indistinguishable from the
    guest having nothing to say.

60. **The log volume is the volume hype booted from, selected by identity, not by USB
    enumeration order -- decided (2026-08-22).** A bare-metal i5 run had a blank USB stick
    (no partition table) enumerate before the FAT32 stick hype actually booted from.
    `usb_log_setup()` claimed the first MSC unconditionally (#387's "first one wins" rule,
    written for a different concern -- never re-pointing an ALREADY-OPEN sink mid-run, not
    choosing which stick opens it) and could not mount the blank one, so the whole run
    produced zero on-disk logs. Operator ruling: enumeration order is irrelevant: the log
    volume is, by definition, the device carrying the ESP hype loaded from, and a bystander
    stick -- blank or not -- must never receive or deny that role.

    **Decided.** `usb_base_is_boot_volume()` (boot/main.c) reuses `fw_1_boot_vol_verify()`'s
    existing identity check -- hype's own loader plus both firmware images present, confirmed
    against the parsed `\hype.cfg` when one was read -- the same check #447 already uses to
    locate the boot volume for config write-back. `usb_log_setup()` now rejects any candidate
    partition base that mounts but fails this check, touching nothing on it: no file created,
    no write attempted. It is called on every MSC the sweep finds -- the primary path and every
    #387 "extra" device -- gated by `g_hype_log_ready`, so at most one candidate ever wins and
    the "never re-point once open" invariant is unchanged; only which stick gets the first
    chance to open it changed. QEMU rig `tools/638/run-638-qemu.sh`: a blank stick enumerated
    first is checked and left alone, the boot ESP enumerated second gets `\HYPE.LOG`, opened
    exactly once.

    **Alternatives considered.** (i) Match by USB serial number recorded at a prior boot --
    rejected: needs a place to persist that serial before any log sink exists, a chicken-and-egg
    #447 already solves structurally by checking content, not identity metadata. (ii) Always
    prefer device 0 / the boot-order-designated device from firmware -- rejected: hype has
    already left UEFI's boot services by the time USB enumeration runs (#447's own comment),
    so no firmware boot-device handle survives to consult.

61. **A boot's log survives the next boot -- bounded generation rotation, not truncation --
    decided (2026-08-22).** Several validation protocols need two consecutive boots compared
    against each other (a marker written on boot 1, read back on boot 2: #119/#120/#178, #211,
    #600). `HYPE.LOG` and every per-VM log were truncated at each boot, so boot 1's own evidence
    -- whether it wrote the marker, what its own diagnostics said -- was gone by the time boot 2's
    log could be read, with nothing in the surviving log even saying a prior one existed.

    **Decided.** `sink_start()` (`core/log_sink.c`) rotates existing generations of a filename up
    by one (oldest evicted) immediately before creating a fresh file, for every sink this
    function serves -- `\HYPE.LOG` and every per-VM log alike, since both funnel through the same
    function. Four generations are kept (`HYPE_LOG_SINK_MAX_GENERATIONS`): not unbounded growth,
    a small fixed window is what the two-boot protocol needs. `HYPE.LOG` -> `HYPE.1.LOG` ->
    `HYPE.2.LOG` -> ... -> evicted past 4, matching FAT32/exFAT naming so a directory listing
    still groups a log's generations visually. The rotation runs inside `sink_start()`, under the
    same #239/decision-57 guard its own `hype_fs_create()` call already requires -- a rename is a
    filesystem mutation exactly like the create it precedes.

    A small persisted counter (`HYPE.BOOTCOUNT`, plain decimal text, read-increment-written back
    on the SAME volume the log lives on) gives each boot a monotonic number, printed as `fw-1:
    boot #N on this volume [#643]` immediately after the sink opens -- as close to "this
    generation's first line" as hype's actual boot order allows (the true first buffered line,
    the build banner from TSC=0, is flushed a few lines earlier in the same initial batch). Two
    logs pulled off one stick can now be ordered correctly without trusting FAT timestamps, which
    a battery-less RTC or a host's own copy tooling may not have preserved (#556).

    QEMU rig `tools/643/run-643-qemu.sh`: boots the SAME media twice without wiping it between
    boots. Verdict: both `HYPE.LOG` (boot 2) and `HYPE.1.LOG` (boot 1, rotated not truncated)
    exist afterward, their boot-counter lines read 1 and 2 respectively, and boot 1's actual
    content (not just its banner) is intact in `HYPE.1.LOG`.

    **Alternatives considered.** (i) Unbounded per-boot filenames (timestamp- or counter-named
    logs, never overwritten) -- rejected: the volume is finite and #596's writer is the
    constrained, single-owner path; an operator who leaves a stick running for weeks would fill
    it. (ii) A separate "previous boot" marker file instead of true rotation -- rejected: only
    preserves one prior boot, and the two-boot validation protocol sometimes needs to look back
    further (a soak run followed by two short re-checks, for instance).

62. **exFAT with two FAT copies: keep both in sync, matching FAT32's discipline -- decided
    (2026-08-22).** #652 found that a `NumberOfFats == 2` exFAT volume (a TexFAT volume) has
    hype write only the ACTIVE FAT copy and never touch the inactive one, while
    `core/fs_ops.c` declares the exFAT driver's full write capability with no asterisk for this
    case. After any hype write, the inactive copy describes an allocation map that no longer
    exists: a repair tool or another implementation that trusts `ActiveFat` and later flips it
    (the second copy's entire purpose) follows chains hype never wrote, and if anything flips
    that bit between two hype sessions, hype's own next mount reads the stale map it left
    behind. FAT32 has never had this gap -- `fat_set` (`core/fat_write_fs.c`) has always written
    every copy in `num_fats` from one authoritative sector image, specifically because "reading
    each copy independently before modifying it allowed a stale medium read to resurrect an
    older allocation map."

    **Decided.** Sync both copies, not refuse the write mount. `hype_exfat_fs_t` gains
    `num_fats`, and `fat_set` (`core/fat_exfat_fs.c`) writes the same sector to every copy from
    one authoritative image, mirroring FAT32's `fat_set` exactly. This keeps exFAT's declared
    capability bits honest (#493) instead of narrowing them, and a TexFAT volume is not an
    exotic case to refuse -- `mkfs.exfat` will produce one whenever asked, and an operator has
    no way to know hype would silently half-write it.

    **Alternatives considered.** Refuse to mount a `NumberOfFats == 2` volume for writing (mount
    read-only, log the reason) -- rejected: exFAT already declares full write capability for
    every other volume shape, and narrowing it here is a worse surprise than the small fixed
    cost of one more sector write per FAT update. The sync-both-copies fix is also a direct,
    already-proven port of FAT32's existing mechanism, not new design.

63. **ext namespace mutation: htree insertion is refused, not guessed at; directory growth is
    bounded to what this slice can address -- decided (2026-08-22).** #498 gave ext2/3/4
    `create`/`unlink`/`mkdir`/`rmdir`/`rename`, journaled (jbd2, checksummed per decision 29/#495)
    on ext3/4 and direct-ordered on ext2 (`core/extj_namespace.c` / `core/ext2_namespace.c`,
    sharing directory-block content logic in `core/ext_dirent.c`). Two scope questions came up
    that decision 29 did not already answer:

    **Htree (`dir_index`) directories.** An htree-indexed directory's interior index blocks are
    disguised as ordinary (inode-0) entries at fixed offsets a linear insert has no way to avoid
    disturbing. **Decided:** refuse (-1) any INSERT (`create`, `mkdir`, and `rename`'s
    destination side) into a directory carrying `EXT4_INDEX_FL`, cleanly, before touching
    anything -- this slice has no htree-aware insertion, and a corrupted index is far worse than
    a refusal. REMOVAL (`unlink`, `rmdir`, and `rename`'s source side) is NOT refused: it only
    ever tombstones a real, named leaf entry found by linear scan, which never touches the
    disguised interior blocks -- the same property that already lets `core/ext.c`'s read-only
    resolver walk an htree directory without understanding htree at all.

    **Directory content growth.** A directory's own data is enumerated and grown through direct
    block pointers (classic) or the in-inode extent root only (ext4) -- never single/double/
    triple indirect or a multi-level extent tree. **Decided:** refuse rather than guess when a
    directory needs more reach than that to accept a new entry. Every directory this slice
    itself creates, and every real mkfs.ext2/3/4 root directory validated against (#498's
    `tools/498/run-498.sh`), stays well inside this. `unlink`/`rmdir` must still accept ANY
    existing regular file or directory by that name, though -- including one the #384/#385/#497
    write path grew past direct+single-indirect (classic) or a depth-0 extent root on a real,
    heavily used volume, which this slice's own writes never produce but cannot assume don't
    exist. **Decided:** the SAME refuse-rather-than-guess rule applies there too -- `free_all_blocks`
    only ever enumerates a classic direct+single-indirect map or a depth-0 extent root (an interior
    extent index entry is the identical 12 bytes as a leaf entry with different field meanings, so
    misreading one as the other would free garbage block numbers, not merely leak space); a
    deletion whose target's blocks run deeper than that is refused UP FRONT, before any mutation,
    leaving the volume exactly as it was.

    **Alternatives considered.** Implementing real htree insertion -- rejected as disproportionate
    to this ticket's scope; htree only matters once a directory is large enough that a linear
    scan gets slow, which is not a shape this slice's own writes, or the validated bar, produce.
    Silently falling back to appending past the htree-disguised region -- rejected outright: the
    ticket calls this exact shortcut out as the one most likely to be attempted and the most
    damaging, since it corrupts the index while looking like it worked.

64. **#416 descoped: NTFS `$LogFile` replay is refused, not attempted; hype's own writes get an
    internal dirty-flag bracket instead of a from-scratch LFS journal -- decided (2026-08-22).**
    #416 as filed asked for parsing `$LogFile`'s restart areas and log-record pages, and replaying
    committed / rolling back uncommitted transactions on a dirty volume. That format (Microsoft's
    LFS -- Log File Service) is undocumented outside Microsoft's own driver source. The project's
    own stated reference implementation, **ntfs-3g, does not implement `$LogFile` replay either** --
    it refuses a dirty volume and tells the operator to boot Windows to clean it, which is exactly
    what hype's existing `#337` read-side mount already does (the `$VOLUME_INFORMATION` dirty-flag
    check in `hype_ntfs_mount`). A from-scratch redo/undo engine reverse-engineered from partial
    community notes, with no independent tool to validate it against, risks the worst outcome this
    project can produce: a replay that LOOKS successful but silently corrupts a real Windows volume.

    **Decided.** Keep detect-and-refuse as the mount gate for a genuinely dirty volume, matching
    ntfs-3g's own behaviour -- no replay is attempted, ever. What #416 delivers instead, for hype's
    OWN future writes (#417 onward):
    - A dirty-flag BRACKET around a writable session: set `$VOLUME_INFORMATION`'s dirty bit before
      the first mutation, clear it only after every pending write has reached the medium and the
      session closes cleanly. This is the same contract Windows's own driver gives an external
      tool -- an interrupted hype session leaves the bit set, and any conforming NTFS driver
      (Windows or ntfs-3g) that mounts it afterward correctly demands a `chkdsk` rather than
      trusting metadata a crash may have left inconsistent. hype does not need to write genuine
      LFS records for this guarantee to hold; the dirty bit is the interop signal, not `$LogFile`'s
      own content.
    - hype's own write-ordering discipline (data before metadata, leaf before parent) inside that
      bracket, the same crash-safety shape decision 29 already established for FAT32/ext -- "the
      journal transaction primitive" from #416's original wording is this ordering discipline plus
      the dirty-flag bracket, not a generic undo/redo log of arbitrary NTFS operations.
    - Fixup STAMPING (the write-side counterpart of the fixup verify `core/ntfs.c` already has) for
      any MFT record or INDX block hype itself writes.
    - USN journal (`$Extend\$UsnJrnl`) maintenance IS implemented for real: `USN_RECORD_V2` is a
      **public, Microsoft-documented** Win32 structure (unlike `$LogFile`'s internal format), so
      there is genuine ground truth to build and validate against.

    **The #596 lesson, applied from the start rather than retrofitted.** #596 (plan.md decision 57)
    found that hype's host filesystem writers hold shared, unlocked state (FAT32's FAT cache,
    allocation cursor, per-file size) where ownership IS the lock -- and that a missing guard at a
    new call site let up to four cores run the same writer concurrently, corrupting a cluster chain.
    NTFS's dirty-flag bracket and USN sequence counter are the same shape of shared, unlocked state.
    Rather than wait for hardware to find the same bug again, the dirty-flag/USN entry points
    (`core/ntfs_journal.c`) take a `hype_fs_owner_guard_t` (`core/fs_owner_guard.{c,h}`, new,
    generalizing #239/decision-57's ad hoc `usb_log_this_core_owns_usb()` pattern into a reusable
    primitive) and refuse -- never silently proceed -- when the executing core is not the bound
    owner. The existing FAT32/log-sink guard in `boot/main.c` is left as-is: it is mature,
    hardware-validated code, and unifying it onto the new shared primitive is a separate, lower-risk
    cleanup, not a prerequisite for NTFS's own writer to get this right on day one.

    **Alternatives considered.** Full LFS redo/undo replay -- rejected above (no ground truth, real
    reference implementation doesn't do it either; user-confirmed 2026-08-22: "do what ntfs-3g
    does"). An fs-level lock so any core may write to the dirty flag/USN state -- rejected for the
    same reason decision 57 rejected it for FAT32: it converts guest-dispatch latency into I/O
    waits on every mutation, and single-owner is already the working shape everywhere else in this
    codebase.

65. **#709: the ext/NTFS write battery gets a boot-time self-test hook on a
    second, named real disk, reusing the disk-inventory + serial-selection
    machinery TERM-11/#487's `mkdisk` already established -- decided
    (2026-08-23).** Recorded because it adds new capability to `boot/main.c`
    (AGENTS.md's highest-blast-radius freestanding code), and because it
    looks, at first glance, like exactly the thing decision 50 rejected.

    **Why this is not decision 50's mistake.** Decision 50 moved *guest device
    emulation* self-tests out of `hype.efi` because an in-binary test built its
    own fixture and so tested hype's emulation against hype's own idea of the
    hardware -- real defects (#552, #550, #565) were invisible to it precisely
    because nothing external (a real guest OS driver) ever walked the surface
    under test. hype's own host filesystem writers are a different shape of
    code entirely: there is no guest layer to skip, because the thing being
    proven is "does hype's own FAT32/ext/NTFS writer produce a volume a real,
    independent tool accepts" -- the FAT32 self-test (`fw_1_run_fat_battery`,
    `HYPE_FAT32_SELFTEST_MARKER`, pre-existing, host-side, on the real boot
    media) already established and validated this shape: its own log line
    says outright "run `sudo fsck.vfat -nv` after this boot -- that is the
    judge", i.e. the self-test's own PASS is provisional, and an independent
    tool is still the final word. #692's generic battery run against ext/NTFS
    the same way is the identical category of test, not a step backward into
    hypervisor-resident guest emulation testing.

    **Design.** A new optional `[hype]` key, `fs_selftest_disk = <serial>`,
    naming an entry in `g_disk_inv` (`core/disk_inventory.c`, #258) by serial
    -- selection by serial, never index, matching every other named-disk
    convention in this codebase (`mkdisk`, `target_disk.path_or_id`). At boot,
    after disk inventory and before guest VMs start:
    - resolve the serial to a `hype_disk_entry_t`; absent or not found is a
      silent no-op (this is opt-in test infrastructure, never a boot
      requirement);
    - bind raw sector I/O to that disk with the same shape
      `mkdisk_dev_read`/`mkdisk_dev_write` already established (chunked
      AHCI/NVMe reads/writes through the disk's own bus), as its own
      independent binding -- not sharing `g_mkdisk`'s state, which belongs to
      the interactive `mkdisk` terminal command and has its own lifecycle;
    - `hype_fs_mount_auto()` the first partition (or superfloppy) that
      mounts, exactly as `mkdisk_mount_volume()` already searches;
    - marker-gate on a file in that volume's root (`FSTEST.RUN`, matching
      `HYPE_FAT32_SELFTEST_MARKER`'s convention) so the battery never runs
      against an operator's real data disk uninvited;
    - call `hype_fs_battery_run()` (#692) and log PASS/FAIL in the same
      greppable format `fw_1_run_fat_battery` already uses, so
      `tools/NNN/run-NNN-*.sh` harnesses can gate on it the same way.

    A QEMU harness (`tools/709/run-709-fsbattery.sh`) formats a small ext (and
    separately, NTFS) image, attaches it as a second disk with a matching
    serial, drops the marker, boots `hype.efi`, and asserts the PASS line --
    then the same disk gets one real-hardware run per this project's
    validation bar.

    **Alternatives considered.** A new `[disk.*]`-section-based resolution
    (reusing the guest-disk-attach config schema) -- rejected: `[disk.*]`
    sections describe a disk's role as a *guest's* backing store (attached
    through virtio-blk/AHCI/NVMe emulation to a VM), and this self-test never
    involves a guest at all; overloading that schema for a host-only purpose
    would make every `[disk.*]` reader need to know about a case that isn't a
    guest disk. Reusing `g_mkdisk`'s existing binding directly -- rejected:
    `g_mkdisk` is stateful, pumped incrementally from the BSP dispatch loop
    across many boot passes for a large qcow2 create, and reusing its fields
    for an unrelated one-shot mount at a different point in boot risks the
    two features corrupting each other's state.

66. **#609: host indirect-branch posture -- IBPB on the guest->host transition where the CPU
    grants it; retpoline compilation deferred as its own, separately-risked follow-up -- decided
    (2026-08-23).** Recorded because #609's own Bar requires this decision exist before any code
    for it lands, and because the two items in its cost-ordered list turned out to carry very
    different risk profiles once actually investigated.

    **The gap.** hype's post-exit code is dense with indirect calls through vtables
    (`vmm_ops`, `hype_fs_ops_t`, `hype_blk_backend_t`) reachable immediately after a VM exit, and
    nothing conditions the host's own branch predictor between guest execution and those calls --
    a guest can train the BTB and speculatively steer host code, the classic Spectre-v2 guest-host
    channel. Decision 48 (L1TF) and decision 47 (contention channels) do not cover this axis.

    **Decided: implement item 2 (IBPB on the transition) now; defer item 1 (retpoline
    compilation).** The two are not equally risky to attempt in this codebase, which only became
    clear from #604's own stack-canary attempt this same session (#711): `-fstack-protector`
    -- a FAR less invasive codegen change than retpoline, which rewrites every indirect
    call/jump in the binary into a thunk sequence -- broke `hype.efi`'s boot outright on this
    project's `x86_64-unknown-uefi` / `ld.lld -flavor link` combination, for a reason not yet
    root-caused (#711). Retpoline is compiler-flag-level, whole-binary, and touches the exact
    code shape (indirect branches) this project's minimal PE/COFF pipeline has already shown one
    surprising incompatibility with. Attempting it blind, in the same session #711 happened in,
    repeats exactly the mistake that ticket is a lesson in.

    IBPB-on-exit carries none of that risk: it reuses `#608`'s own just-landed infrastructure
    (`hype_cpu_has_ibpb()`, the real IBPB `wrmsr`, both already proven correct and boot-tested) and
    adds one MSR write at a point in the SVM/VMX exit path already proven safe to touch (the same
    bracket #608's SPEC_CTRL restore lives in). No new compiler flags, no whole-binary codegen
    change, no new failure class to root-cause blind.

    **What lands:** after every VM exit, on both backends, if the host CPU grants IBPB
    (`hype_cpu_has_ibpb()`, already vendor-hardcoded per file from #608), issue it before any other
    host C code that contains an indirect call runs -- the same "restore host state before
    anything else executes" placement #608's SPEC_CTRL restore already established. No skip-logic
    against AMD Automatic IBRS or Intel `IA32_ARCH_CAPABILITIES` in this pass: getting either
    exactly right needs bit-position certainty this session did not verify against a primary
    source (the `research-provenance` rule this project already follows), and a redundant-but-
    correct IBPB on hardware that already provides the guarantee some other way is a safe default
    to fall back to, unlike guessing a skip condition wrong and silently dropping the mitigation
    where it was still needed. Recorded as an explicit follow-up refinement, not a blocking gap.

    **Measured cost, both vendors, exit-path microbenchmark (`tests/micro/pausespin.c`-shaped
    spin-loop, N VM exits timed via TSC before/after this change):** recorded in #609's own ticket
    comment rather than duplicated here, since a benchmark number is a snapshot of one run on one
    machine on one day and belongs where it can be dated and re-run, not baked into a design
    decision that is supposed to stay true.

    **Retpoline itself remains open**, tracked by #609 continuing to reference this decision until
    it lands separately, with its own root-cause work on #711 as a prerequisite (the same toolchain
    combination is implicated) and its own before/after measurement.

    **Alternatives considered.** Attempting retpoline anyway, accepting the regression risk --
    rejected outright: #711 already spent real time on exactly this failure mode once this
    session, and there is no reason to expect a different toolchain interaction to fail more
    gracefully. A blanket unconditional IBPB with no CPUID gate -- rejected: on hardware that
    lacks it, the WRMSR itself would #GP on the host (this runs on hype's own CPU, not inside a
    guest), which is worse than skipping the mitigation.

67. **#691: AVIC's per-VM ID tables are grouped by an explicit `vm_idx` parameter threaded to the
    setup path, not by regrouping the flat vCPU pool; the AVIC backing page is authoritative for
    guest-visible LAPIC state once AVIC is active for a vCPU, with `g_fw_1_lapic`'s software model
    left as the flag-off fallback rather than kept in sync -- decided (2026-08-24).** Recorded
    because #691 exists specifically to make this decision before #640's implementation lands, per
    this project's own new-capability workflow.

    **The shape problem.** AVIC's IPI-delivery model needs a vCPU to look up any OTHER vCPU of the
    SAME guest by physical APIC ID, in a table the CPU itself walks in hardware -- so that table
    must be shared across every vCPU of one VM, and separate across VMs (hype's guest-visible APIC
    IDs are not globally unique: `boot/main.c`'s per-VM LAPIC init assigns IDs starting at 0 for
    every VM, so VM0/vCPU0 and VM1/vCPU0 collide if they ever shared a table). `svm_vcpu.c`'s own
    vCPU pool (`g_vmcb_pool`/`g_ctx_pool`) is a flat sequential slot array today, with no notion of
    "these N slots belong to VM K" -- the exact gap #640 found as its own cause 2.

    **Decided: thread `vm_idx` explicitly, don't regroup the pool.** `boot/main.c` already has
    `vm_idx` at every AVIC setup call site (it is iterating `g_vms[i]`); `svm_bits.c`'s AVIC
    configure function gains a `vm_idx` parameter and indexes a new
    `g_avic_physical_table[HYPE_CFG_MAX_VMS][4096]` /
    `g_avic_logical_table[HYPE_CFG_MAX_VMS][4096]` pair by it -- one physical/logical ID table PER
    VM, shared by every vCPU of that VM, sized by the existing `HYPE_CFG_MAX_VMS` bound rather than
    a new allocation scheme. The AVIC **backing page** stays genuinely per-vCPU (one page per pool
    slot, the same shape VMX's own `g_virtual_apic_page[slot]` already uses for its analogous
    per-vCPU virtual-APIC page) -- it is that one vCPU's own register file, never shared.

    **Alternative considered and rejected: regroup `g_ctx_pool`/`g_vmcb_pool` by VM.** Retrofitting
    VM-grouping into the pool's own allocation would touch every existing caller that computes a
    slot index today (`gpr_ptr`, the whole exit-dispatch slot-lookup family), for a benefit only
    AVIC's ID tables need. Threading one extra `int vm_idx` parameter down a call chain
    `boot/main.c` already has the value for is smaller, safer, and localized to exactly the new
    feature that needs it -- the same "don't disturb a large, working, well-tested structure for
    one new caller's need" reasoning this project has applied elsewhere.

    **Which owns guest-visible LAPIC state: the AVIC backing page, once AVIC is active for that
    vCPU.** AVIC's entire point is that most guest APIC register accesses never trap to hype at
    all -- the CPU services them directly against the backing page in hardware. Keeping
    `g_fw_1_lapic`'s existing software model bidirectionally synchronized with that page would mean
    either polling it on a schedule (races against the guest's own concurrent hardware-accelerated
    writes, and costs cycles AVIC exists to avoid spending) or intercepting every write (which
    defeats AVIC's own acceleration by turning every access back into a trap). Instead: once AVIC
    is active for a vCPU, any HOST-side code that needs that vCPU's guest-visible LAPIC state reads
    the backing page directly (a new accessor, `#640`'s own implementation scope) rather than
    `g_fw_1_lapic`; the software model remains exactly as it is today for the flag-off build and
    for any vCPU AVIC is not active on. This is the same ownership split KVM's own AVIC support
    uses: hardware owns the fast path, software intervenes only on the two exit reasons
    (AVIC-incomplete-IPI, AVIC-noaccel) hardware cannot service itself.

    **Not part of this decision** (left to #640's own implementation): the three-copy setup-call
    consolidation, the incomplete-IPI/noaccel exit handlers themselves, and the actual backing-page
    accessor's exact signature.

68. **#716: a date-based build version, `YYYY.MM.DD[-tag] (#commit-id)`, shown on the dashboard
    (§6b) and the startup banner (`boot/main.c`'s existing `"hype: build " HYPE_BUILD_ID` line) --
    decided (2026-08-24).** The date component is the build date, not the commit date -- when the
    running binary was actually produced, not when its source last changed, since the same commit
    can be rebuilt on different days and the two can legitimately differ.

    **`-tag` comes from a build flag, not a fixed value.** Following `HYPE_BUILD_ID`'s own pattern
    (Makefile, `EXTRA_CFLAGS` baking a `-D` define into the build): a new `HYPE_BUILD_TAG` define,
    settable the same way, defaults to `alpha` for an unconfigured/local build, is set to `ci` by
    the CI pipeline's own invocation, and is passed empty for a release build so the version string
    carries no tag suffix at all -- `2026.8.24 (#aaaaaaaa)` rather than `2026.8.24- (#aaaaaaaa)`.
    This keeps the mechanism identical to the existing `HYPE_BUILD_ID` wiring rather than inventing
    a second convention for build-time metadata.

    **`#commit-id` is `HYPE_BUILD_ID` itself** (`git describe --always --dirty --abbrev=7`) --
    not a second git invocation. The version string composes the existing define with the new
    date/tag pieces rather than duplicating what `HYPE_BUILD_ID` already computes.

    **Open question, not resolved by this decision: zero-padding of month/day.** The worked
    example that prompted this ticket used `2026.8.24` (no leading zero); whether the format should
    instead zero-pad to `2026.08.24` for fixed-width sorting/display is left to whoever implements
    #716 to confirm before writing the date-formatting code, and is called out explicitly in that
    ticket rather than assumed either way here.

    **Alternative considered and rejected: derive the date from the commit (`git show -s
    --format=%cd`) instead of the build machine's clock.** Rejected because it collapses the one
    piece of information a build stamp is for on this project -- distinguishing a fresh capture
    from a stale one (see `HYPE_BUILD_ID`'s own Makefile comment) -- a rebuild of an old commit
    would misreport itself as an old build.

69. **#506: a file-backed virtual disk grows its backing file on demand, so a sparse image is
    usable at runtime, not merely creatable -- decided (2026-08-24), revised same day after
    finding the right layer already exists.** First pass of this decision proposed teaching
    `core/blk_image.c`'s physical-only `hype_file_map_t` path to tolerate holes. Wrong layer:
    `core/file_range.c` (#381) already has a sparse-aware contract, `hype_file_rmap_t`, with
    `hype_file_rmap_read_at()` synthesizing zeros for HOLE/UNWRITTEN ranges without touching the
    medium, and `hype_file_rmap_write_at()` already refusing a write into a HOLE ("needs
    allocation... this layer must not fake") rather than inventing sectors. Both are unit-tested
    and already load-bearing for NTFS/ext internals (journal/allocator reads). A sparse guest
    disk should be built on this, not a second implementation of the same hole semantics inside
    `blk_image.c`.

    **A new guest block backend, `hype_blk_image_sparse_t`, sits beside (not inside)
    `hype_blk_image_t`.** It holds a `hype_file_rmap_t` instead of a `hype_file_map_t`, plus a
    growth handle (`hype_fs_t *`, `hype_fs_file_t *`, and the file's path -- needed to re-resolve
    the rmap after growth via `hype_fs_map_ranges`). A disk not marked sparse in `hype.cfg` keeps
    using `hype_blk_image_t` exactly as today, unconditionally -- the fast, no-holes,
    no-filesystem-metadata-ever path is untouched for every existing raw/qcow2-backed disk.
    Read: `hype_file_rmap_read_at()` directly -- DATA through the injected host read, HOLE as
    zeros, no filesystem writer ever touched by a guest read. Write: `hype_file_rmap_locate()`
    first to classify the target range. DATA -> `hype_file_rmap_write_at()`, the existing fast
    in-place path. HOLE -> the growth path below (UNWRITTEN should not occur here in practice --
    #507 restricts sparse creation to ext, which represents an unallocated region as HOLE, never
    ext4 fallocate's UNWRITTEN, precisely because hype's own extent-walking reader already
    refuses an unwritten extent, #696 -- but a write hitting UNWRITTEN is refused the same as any
    other non-DATA range this layer cannot fake, not silently promoted to a growth attempt).

    **The write path, not a separate allocator, does the growing.** `HYPE_FS_CAP_WRITE_GROW`
    already exists (`core/fs_ops.h`) and FAT32 (#382) and ext (#384/#385, jbd2-journaled) already
    implement it: `write_at` past current EOF allocates and zero-fills the gap, crash-safely, and
    is exactly what `tools/487`'s mkdisk pump already calls to build a file cluster by cluster.
    On a HOLE hit, the sparse backend hands the guest's OWN write buffer straight to
    `hype_fs_write_at()` on the growth handle -- one filesystem-level call, not an
    allocate-then-remap-then-retry dance -- then re-resolves the rmap (`hype_fs_map_ranges`) so
    the newly-allocated region joins the fast DATA path for every subsequent access. `map.size_bytes`
    is the file's LOGICAL/virtual size from the first `hype_fs_map_ranges` call at VM setup, so
    `be->total_sectors` reports the full guest-visible disk size before the guest has written
    anything -- exactly what `hype_file_rmap_t` already carries.

    **Serialization reuses the existing per-resource ticket lock (`core/ticket_lock.c`, SMP-7
    #191), one new instance scoped to filesystem growth, not per-volume.** Two vCPUs -- possibly
    on two different VMs' sparse disks that happen to share a host volume's allocation bitmap --
    growing at once is the hazard the ticket names explicitly; a single global growth lock is
    simpler than per-volume and growth is rare relative to steady-state I/O, so the extra
    serialization cost is not worth the complexity of scoping it narrower. The lock is held for
    exactly one `hype_fs_write_at` call plus the following re-resolve -- never across a guest
    VM-exit boundary, matching every other lock in this codebase (SMP-7's own "nothing may block,
    spin on another condition, or enter a guest while holding it").

    **Refusal surface, per the ticket's bar:** the fs write_at call fails cleanly (volume full,
    file would exceed `HYPE_FILE_MAX_RANGES`, or the mounted filesystem lacks
    `HYPE_FS_CAP_WRITE_GROW`) and the sparse backend's write returns -1 for that guest write --
    the same "guest write fails" contract every other blk_backend error already has, never a
    host-side abort. A filesystem without `HYPE_FS_CAP_WRITE_GROW` cannot back a sparse image at
    all; refused at VM setup time (disk marked sparse but the volume can't grow files) rather
    than discovered at the first hole.

    **Alternative considered and rejected: allocate-then-remap-then-retry-via-raw-sectors**, i.e.
    ask the filesystem only to extend the allocation (no data), remap, then issue the guest's
    write through the normal raw-sector path. Rejected because it reintroduces exactly the
    ordering hazard #385's journaled ext writer exists to avoid: a crash between "extend" and
    "write the guest's actual bytes" leaves a newly-allocated, zero-filled region that reads as
    valid but is not what the guest asked to be there -- functionally silent data loss for that
    write, distinguishable from a clean failure only by an alert guest checking its own data.
    Routing the real bytes through `write_at` in the SAME call as the growth means the filesystem
    driver's own crash-safety story (data before the metadata that exposes it, #385's rule)
    covers this path for free, because it is the same call ext's own hole-filling writer already
    makes crash-safe for its own sake.

    **Alternative considered and rejected: extend `hype_blk_image_t`/`hype_file_map_t` itself to
    tolerate holes** (this decision's own first draft). Rejected on discovering `hype_file_rmap_t`
    already solves the identical problem, tested, for NTFS/ext's own internal readers --
    duplicating it inside `blk_image.c` would mean two independent implementations of "HOLE reads
    as zero, UNWRITTEN reads as zero but is allocated, a write into either needs a capability this
    layer does not have" to keep in sync, for no benefit over reusing the one that already exists.

70. **#232: the `hype-additions` companion ISO is a SEPARATE disc from the OS installer, per
    platform family, attached as a second `cdroms` entry -- decided (2026-08-25).**

    **Correction (2026-08-26, #727):** this entry originally said `docs/hype-cfg-spec.md`
    "already allows up to ~4 per VM -- no hype-side capability needed, only content".
    That was reading the SPEC, not the code. `cdroms =` is parsed, admission-checked and
    displayed, but nothing ever attaches it to a guest device: a VM with `install_media`
    plus `cdroms = addons` boots with one optical device, not two. The capability is
    genuinely missing and is decision 71 below; #232 is blocked on it. #228 proved the underlying mechanics (offline package repo, unattended answer
    file, post-install bootloader/initramfs fixups) but baked all three into a REMASTERED Alpine
    ISO. That does not generalize to #146's mixed-distro case or to BSD/Windows, which is exactly
    the open question #232's own comments left unresolved. Splitting into a stock, unmodified OS
    installer (`cdroms[0]`) plus a small per-platform `hype-additions.iso` (`cdroms[1]`) is what
    every other hypervisor's equivalent (Guest Additions, VMware Tools, `virtio-win`) already does,
    and lets the additions content be versioned/rebuilt independently of whatever OS ISO an
    operator supplies.

    **Content manifest, by platform family** (`os_hint` already exists per-VM,
    `docs/hype-cfg-spec.md` §5.6, so the additions ISO can carry all three trees and let each
    platform's own bootstrap pick its own):

    - **`linux/`** -- generalizes #228's proven Alpine recipe rather than replacing it: an offline
      apk repo (`apks-hype/x86_64/` ONLY -- §the #232 ticket comment already corrects the original
      "needs x86_64/ and noarch/" instruction; apk reads one arch dir's `APKINDEX.tar.gz` and every
      package, `noarch` included, must be indexed there, matching a real Alpine mirror), a
      `local.d`-runlevel unattended driver script (`tools/228/autoinstall.start`'s pattern:
      explicit `modloop` start + a watchdog, since it does not survive `setup-alpine` re-entering
      the default runlevel), and an `mkinitfs.conf` carrying Alpine's own stock `sys-install`
      feature list verbatim (`ata base cdrom ext4 keymap kms mmc nvme raid scsi usb virtio`) --
      curating a smaller list against hype's OWN virtual disk (`virtio` alone boots under hype)
      was #232's own real regression once the disk moved to bare metal for #226. Other distros
      (#146) get their own subtree with their own package-manager equivalent; the apk tree is not
      assumed to be the only one forever.
    - **`bsd/`** -- FreeBSD's native unattended mechanism is `bsdinstall`'s `installerconfig`
      (a shell script bsdinstall sources instead of running its menu UI when found at a known
      path on the install media), not a remaster. The additions ISO carries an `installerconfig`
      plus an offline `pkg` repo mirror ONLY if packages beyond base/kernel are needed -- `bsd`'s
      `os_hint` already defaults to `virtio-blk` + virtio-net (§5.6), both inbox in FreeBSD's
      GENERIC kernel since well before any FreeBSD release hype targets, so **no driver payload is
      needed for BSD at all**, unlike Alpine's initramfs-feature-list problem. The one bootloader
      lesson #120/#228 both already paid for still applies: FreeBSD's own EFI installer must land
      at the UEFI-spec fallback path (`\EFI\BOOT\BOOTX64.EFI`), since hype's guest OVMF carries no
      NVRAM boot entry across a restart.
    - **`windows/`** -- Windows Setup's native unattended mechanism is `autounattend.xml` at the
      root of ANY attached media (Setup scans every drive for it). **No driver payload either**:
      `windows`'s `os_hint` deliberately defaults to `ahci-sata` (§5.6's own reasoning -- "a virtio
      system disk is invisible at Windows install") + `e1000` (§ NIC-derivation table), and both
      are inbox on every Windows version hype targets -- the exact opposite of the usual
      `virtio-win` problem other hypervisors solve, because hype's own bus defaults for `windows`
      were already chosen to avoid it. What Windows DOES need that neither other platform does:
      an explicit boot-config step (`bcdedit /ems {default} on` + `bcdedit /emssettings COM1
      115200`, run from `autounattend.xml`'s specialize pass) to get ANY guest console output onto
      the serial port hype's whole diagnostic pipeline (`\HYPEFULL.LOG`, input scripts, #698-style
      debugging) depends on -- Windows Setup itself is silent on a serial line without this, unlike
      Linux/BSD where the installer's own console already defaults to what the kernel command line
      says.

    **Open and NOT resolved by this decision: how the primary OS installer boot medium learns to
    look at the second CD at all.** #228's remaster works because the seed lives ON the medium that
    boots. A genuinely separate-ISO Alpine flow needs either (a) Alpine's own live-boot kernel
    parameters (`alpine_repo=`, `apkovl=`) pointed at the second CD-ROM device, which is standard,
    documented Alpine functionality and would let `cdroms[0]` stay a bone-stock ISO, or (b) hype
    generating a tiny custom boot medium of its own. FreeBSD's `installerconfig` and Windows'
    `autounattend.xml` do not have this problem -- both mechanisms scan every attached medium for
    their answer file by design, so the stock ISO can stay `cdroms[0]` unmodified with no
    kernel-parameter bridging required. This is real remaining design work for the Linux leg,
    tracked as follow-up rather than settled here.

    **Resolved (2026-08-25), same day:** option (b) above -- `tools/232/linux/bridge-boot.start` is
    a two-line apkovl (find the separate additions medium, `exec` its `install-linux.sh`) injected
    into a stock `alpine-standard` ISO via the same xorriso remaster technique #228 already proved
    (`tools/232/linux/make-bridge-iso.sh`), carrying NONE of the repo/seed content #228's own
    remaster did. This keeps the "separate ISO" property where it matters -- the repo, the seed and
    the mkinitfs/bootloader fixups all live on `hype-additions.iso` alone, versioned and rebuilt
    independently of the boot medium -- while sidestepping Alpine's lack of a "scan every medium"
    behavior with the smallest possible per-boot-medium footprint, rather than the kernel-parameter
    route (a), which would need cmdline plumbing this decision did not want to add scope for. Chosen
    over (a) because it needed no `hype.cfg`/`grub.cfg` changes beyond what #228 already had a
    working recipe for.

    **Scope correction (2026-08-25):** the content manifest above undersold the real target matrix
    by naming only Alpine for Linux and only FreeBSD for BSD. The actual bar this ticket is scoped
    against is: **Linux** across package-manager families (apt/Debian, dnf/Fedora, pacman/Arch, apk/
    Alpine -- each needs its own offline-repo + unattended-answer mechanism, none of which share
    tooling with another), **Windows 7 through 11** (and Server equivalents -- `autounattend.xml`'s
    own schema/components shift across that range, particularly 7/8.x's non-UEFI-default install
    vs. 10/11's GPT/UEFI default), and **BSD** beyond FreeBSD (OpenBSD's `install.conf`, NetBSD's
    `sysinst` response file, DragonFlyBSD's own installer -- each a distinct mechanism from
    FreeBSD's `bsdinstall`/`installerconfig`). Only the Alpine leg is built and QEMU-validated as of
    this correction; every other family is unbuilt and tracked as follow-up (#725 and likely
    per-family sub-issues once the Alpine pattern is proven enough to generalize from), not
    silently assumed covered by "linux/" or "bsd/" as directory names might imply.

71. **Any number of guest optical drives: one AHCI HBA per drive, all sharing ONE level-triggered
    GSI, capped by free PCI device numbers rather than by IO-APIC pins -- decided (2026-08-26).**
    #727 found `cdroms =` documented but never attached. The reason it was never a small fix:
    `hype_ahci_t` (`devices/ahci.h`) holds ONE port's registers as scalars, not an array --
    `HYPE_AHCI_PORT_COUNT 6` is only the readable aperture, and the header says port zero is the
    only active medium. A second disc cannot be a second port on the existing HBA.

    **Chosen: one HBA (one PCI function) per optical drive**, following the precedent #262 set for
    the ATA disk (`g_fw_1_ata_ahci`) and #329 for extra disk slots (`vm->disk[slot].ata_ahci`).
    The multi-HBA NPF dispatch that needs already exists and is proven; what is new is a per-VM
    array of `{hype_ahci_t, hype_atapi_t, hype_iso_stream_t}` and the resolve/attach loop.

    **Interrupts: all optical HBAs share one GSI.** The 24-pin IO-APIC is fully allocated already
    (16-19 the dev-2 block, 20 virtio-blk/NVMe/virtio-net, 21 the ICH9 SATA function, 22-23 the
    extra disk slots), which is exactly what caps disks at 3 (`HYPE_FW_1_MAX_DISKS`). Rather than
    let that cap the disc count too, every optical HBA shares the dev-2 CD line (GSI 16) and the
    LINE is treated as the OR of its devices -- the same arrangement decision 58's neighbours
    already use for GSI 20 (`vblk_pending || nvme_pending || vnet_pending`, computed inline in the
    dispatch loop). #440's warning applies and is the whole reason this must be an OR: hype's
    per-device deassert would otherwise drop a still-pending interrupt from another HBA on the same
    pin, which is why #440 moved the SATA function OFF GSI 16 in the first place. The OR must cover
    the primary ATAPI HBA and every extra optical HBA before any deassert.

    **The cap becomes PCI device numbers, which is why "any number" is honest.** Bus 0 has 32 slots;
    2/3/4/5/6/7/8/9/31 are spoken for, leaving roughly 20 free. `devices/dsdt.asl` gets `_PRT`
    entries for that range routed to the shared GSI, added ONCE and unconditionally -- a `_PRT`
    entry for an absent device is inert, which is the same reasoning the extra-disk and virtio-net
    entries already record. So no per-config DSDT, and adding drives later needs no ACPI change.

    **Rejected: widening the IO-APIC past 24 pins.** Same reasoning decision 58's neighbour records
    -- the pin count is a guest-visible property of the chipset hype claims to be, every VM would
    inherit it, and the blast radius dwarfs one shared line.

    **Rejected: a media-swap on the single existing drive.** Cheaper, and enough for #232's
    post-install bridge alone, but it cannot serve the case the spec explicitly documents (an
    installer ISO plus a Windows storage-driver ISO visible *simultaneously* during setup), and
    building the cheap thing first would leave the documented behaviour still unimplemented.

    **Implemented and QEMU-validated 2026-08-26** (`tools/727`): the guest sees `/dev/sr0` and
    `/dev/sr1`, mounts the second and reads back a marker file present only on that disc. Two
    traps found doing it, recorded because neither is visible from the design:

    - **There are TWO BAR-publish paths and the obvious one is the wrong one.**
      `fw_1_program_kernel_bars()` returns early unless `vm->kernel_boot`, so a latch added there
      never fires for a VM whose own firmware programs the BARs -- which is every installer VM,
      i.e. exactly the configuration `cdroms =` exists for. The live one is the firmware-boot path
      beside #262's SATA latch. The symptom read nothing like the cause: hype wedged in HOST code
      with the guest's exit count FROZEN while its in-host time climbed, on an ECAM access to the
      new device.
    - **Claiming an NPF the model declined is an infinite fault loop.** Ignoring
      `vmm_handle_ahci_npf_map`'s return value and claiming the access regardless means a register
      the model does not implement never gets its RIP advanced. Claim only what was serviced, as
      the primary drive's branch already did.

    **Known coupling to fix in the same change:** the ISO bounce pool is one slot per VM
    (`bounce_slot = vi`, four sites in `boot/main.c`, pool sized to the VM count by #428). Two
    streams on one VM would share a buffer and corrupt each other's reads, so the pool resizes to
    `vm_count * max_optical` with distinct slots. #428's own lesson -- a silently-clamped per-VM
    index caused a real bug three times in this code -- says compute the slot explicitly and let an
    out-of-range value fail loudly rather than clamp.

72. **`autostart` and the run-state record BOTH have to permit a VM to start; the config is the
    ceiling, the record is a veto within it -- decided (2026-08-27).**
    #732 found `[hype] autostart` parsed, validated and serialized, with no consumer: `autostart =
    none` started everything. Honouring the key is a bugfix, but it lands on a site that already
    has an answer to "does this VM start" -- M9-4's run-state record (#177, `\hype-state.txt`) --
    and the two can disagree, so the precedence is a decision and not just code.

    **They are ANDed. A VM starts only if `autostart` permits it AND the record does not say
    STOPPED.** The two answer different questions and neither is a better source for the other's:
    the config is operator-authored and durable ("what should be up"), the record is hype's own
    memory of the last shutdown ("what was up"). Letting the record override the config would mean
    a VM the operator excluded comes up anyway because it happened to be running before -- which is
    #732's complaint restated. Letting the config override the record would mean a VM the operator
    stopped at the terminal restarts on the next boot, undoing #177.

    **The default keeps today's behaviour exactly.** `autostart` defaults to `all`, and an absent
    or unreadable config leaves it there, so a host with no `autostart` key behaves as it does now
    and only the record holds anything back. This matters for the same reason #177 gives for
    UNKNOWN: a fresh stick with no record and no config that came up with nothing running looks
    identical to one that failed.

    **A name in `autostart` that matches no VM is a warning, not a refusal.** It is a typo in an
    operator's list, and the cost of refusing the whole boot over it is far higher than the cost of
    saying so and starting the VMs that did match. Consistent with S4.3's warn-and-retain stance
    for unknown keys.

    **Logged either way, at the same site and in the same shape as M9-4's line**, because the
    failure this ticket describes is silence: an operator who asks for one VM and gets four must be
    able to read why from the log.


73. **USB hot-plug: event-driven, covering HID and mass storage -- decided (2026-08-27).**
    Raised by the operator during #734's hardware validation: unplugging the keyboard and
    plugging it back in leaves hype unable to use it. Stated as a requirement, not a
    preference.

    hype has no hot-plug machinery at all today -- not incomplete, absent. Enumeration
    runs exactly once, during the root-port scan and hub walk at boot. There is no Port
    Status Change Event handling anywhere: the only event TRB type `core/xhci.h` names is
    `HYPE_XHCI_TRB_TRANSFER_EVENT = 32`, and `cmd_submit_wait()` documents port-change
    events only as noise to be skipped while waiting for a command completion. A device
    that leaves is never torn down and a device that arrives is never seen.

    **Detection is EVENT-DRIVEN: xHCI Port Status Change Events for root ports, and each
    hub's status-change interrupt endpoint for everything behind a hub.** The alternative
    considered, and rejected, was a polled rescan -- periodically re-reading PORTSC and
    issuing `GET_PORT_STATUS` to every hub from the existing dispatch loop. Polling is
    markedly simpler, reuses enumeration code that already works, and needs no new event
    class; its cost is per-hub bus traffic on every tick forever, and a plug latency
    bounded by the tick. Event-driven was chosen for the opposite trade: no steady-state
    traffic, and a topology change is noticed when it happens rather than up to a tick
    later. It is the larger change and it lands in the layer that produced #734's four
    distinct faults, so it is to be built behind its own diagnostics from the start.

    **The hub half is not optional and is the harder half.** A 2.0 hub reports downstream
    port changes on its own status-change interrupt endpoint, which hype deliberately does
    not configure today -- `hype_xhci_configure_hub_slot()` sets Context Entries to 1
    (EP0) precisely because nothing needed it. The operator's keyboard is behind a 2.0
    hub, so the case that prompted this requires that endpoint. It also makes every hub a
    consumer of the per-endpoint interrupt-IN pool, which is `HYPE_XHCI_INT_IN_MAX = 4`
    and was sized for two HIDs.

    **Scope is HID and mass storage, and the two are not the same problem.** A keyboard
    or mouse arriving or leaving is an enumeration question: claim it, or release it and
    stop polling. Mass storage is a data-integrity question, because hype boots from USB
    and writes its own log there (decision 28). An unplug mid-write must fail the
    in-flight I/O cleanly and mark the backing device gone rather than let a FAT32 or
    exFAT writer continue against a device that is not there; a re-plug must not silently
    resume a half-written cluster chain. #596 is the precedent for how badly that half
    goes wrong when it is not thought about. The log sink and `blk_phys` therefore need an
    explicit "backing device departed" state, and that is the bulk of the work.

    **Device identity on re-plug is by port and route, not by VID:PID or serial.** Two
    identical keyboards must not be confused for one another, and a device moved to a
    different port is a different attachment even when it is the same physical object.
    This matches how `media_disk` already resolves (by INQUIRY serial) only because a
    storage medium's identity genuinely is its contents; an input device's is its place in
    the topology.

    **Slot teardown is required, not incidental.** `DEVPOOL` is 8 per controller and a
    departed device's slot is currently never freed, so without teardown a dozen
    unplug/replug cycles exhaust enumeration itself.

    **Blocked by #743.** Hot-plug recycles slot ids as its normal mode of operation, and a
    HID on a recycled slot id currently fails every interrupt-IN transfer with cc=4 --
    observed four times across the 2026-08-27 boots, mechanism not yet identified, worked
    around in `fw_1_hub_visit()` with a fixed keep-budget of 3. Building hot-plug on that
    foundation would produce a feature that works intermittently and appears to be its own
    fault. #743 must be understood first.

    **Decomposed into four tickets, not one.** #744 port-change events and departure
    teardown (root ports); #745 arrival and claim on a root port; #746 the hub
    status-change endpoint, which is what covers the machine that asked for this; #747
    mass storage's departed-device state, which is the data-integrity half and is
    independent of #745/#746.

    **Built 2026-08-27, #744-#746; #747 outstanding.** Two findings worth keeping with the
    decision rather than only in the tickets:

    - **A hub's first status report describes the state BEFORE the walk.** The endpoint is
      armed before the port loop, so every populated port reads as "changed". Acting on it
      re-enumerates devices that never moved, and re-enumeration RESETS the port -- which
      would knock out a working keyboard moments after claiming it. The walk discards that
      first report, and an arrival on a port the inventory already knows is slotted is
      ignored. Any future hot-plug work has to keep both.
    - **QEMU cannot validate the hub half.** Measured, not assumed: hype arms the endpoint
      and polls it 18400 times with zero errors across a run that did a runtime detach and
      attach, and QEMU's `usb-hub` reports nothing. Root ports validate fully in QEMU;
      #746's bar needs the 5950X. The `HUBPOLL` counters exist so that "hype is not
      polling" and "the hub is not reporting" can never be confused again.

    The #743 risk this decision named as a blocker was tested and downgraded: hype's
    handling of a recycled slot id is correct (`tools/743`), so hot-plug's constant slot
    recycling is a hardware risk to measure on the 5950X, not a design flaw to fix first.

74. **A command that never completes aborts and restarts the command ring, and a ring that
    cannot be restarted is declared dead rather than retried -- decided (2026-08-30).**

    Boot 31 measured the alternative. A Stop Endpoint command went out at t=9.9 min and no
    event ever came back. hype's response was a bare `return -1`: the TRB stayed enqueued,
    the ring was never examined, and the next command went in behind it. Every command for
    the remaining 74 minutes timed out -- 614 of them, each costing the 125 Hz input tick a
    full second -- and because reviving a silent interrupt-IN endpoint needs two commands,
    all three keyboards and all five hub devices on that controller were deaf for the rest
    of the run with `revives=0` and `errors=0`. Nothing in the log said the ring had
    stopped, because nothing looked at it.

    **USBSTS is read before anything is attempted.** HSE or HCE means the controller itself
    has failed, and no command-ring surgery recovers that -- only a controller reset would,
    and that tears down every addressed device including the stick hype is writing its log
    to. That case is named and latched, not papered over with a retry that cannot work.

    **Otherwise the recovery is the one xHCI 4.6.1.2 specifies**: set `CRCR.CA` to abort the
    command in flight, wait for `CRCR.CRR` to clear (the pointer field is only writable
    then), drain the events the abort posts -- routing transfer events to their endpoints,
    because dropping one is how an interrupt-IN endpoint goes permanently deaf (#761) --
    then re-point the ring at its base and carry on.

    **Bounded at four recoveries, then dead.** Retrying for ever is what the old code did by
    accident, and it is worse than stopping: it spends the input tick, it fills the log, and
    it hides the failure behind noise. Once the ring is declared dead every later command
    fails IMMEDIATELY instead of waiting a second first, so the dashboard and the guests keep
    running while the USB devices on that controller are honestly reported as unreachable.

    **A recovery that fails must be counted.** The same run showed why: an endpoint deaf for
    74 minutes reported `revives=0`, which reads as "the revive never fired". It had fired
    hundreds of times and failed, on a path that incremented nothing and printed nothing.
    `revive_fail` now sits beside `revives` on the same line, and the DEADMAN warm-reboot
    treats a failed revive as equal evidence to a successful one -- it previously required a
    revive to have succeeded, so it would have declined to fire in exactly the case where the
    input path was provably unrecoverable.

75. **A wedged xHCI controller is RESET and re-enumerated -- never the one carrying the log or
    the boot medium -- and the wedge is made reproducible by injection so it costs one
    hardware run rather than many -- decided (2026-08-30).**

    Boot 35 closed off the software options. Controller[2]'s command ring stopped answering,
    hype issued a Command Abort as a full 64-bit CRCR write and waited xHCI 4.6.1.2's own five
    seconds, and `CRR` was still set -- with `usbsts=0x00000010`, Port Change Detect alone: no
    halt, no host system error, no host controller error. The controller insists it is healthy
    and will not release its command ring. Every interrupt-IN endpoint on it is then
    unrecoverable, because reviving one takes two commands, and hot-plug goes blind with them
    because a hub reports through the same kind of endpoint. The operator re-plugged a keyboard
    and hype never saw it.

    **The reset is per CONTROLLER, not per machine.** The DEADMAN warm reboot already exists and
    works, but it ends the run -- and a run that ends is a run whose remaining ninety minutes of
    evidence never happened. On the 5950X the wedged controller carries cameras, hubs and the
    HIDs; the boot medium and the log sink are on the other one. Resetting just the wedged one
    costs the devices on it and keeps everything else, including the log that has to record
    whether the recovery worked.

    **hype REFUSES to reset the controller that owns the log sink or the boot medium**, says so,
    and stays dead instead. Staying deaf with evidence beats recovering input and losing the
    record of why it was needed; and hype writes its log through that controller, so resetting
    it would pull the floor out from under the diagnosis mid-write. This needs a fact hype does
    not currently record -- WHICH controller owns the sink -- so that comes first.

    **Teardown is explicit, and it is the risky half.** A controller owns interrupt-IN blocks,
    MSC bulk rings and backends, media device entries, USB inventory rows, hub-table rows and
    claimed HID slots. Some already have per-controller release paths (`hype_xhci_hub_forget_ctrl`,
    `hype_xhci_int_in_release_slot`, `hype_usb_inventory_note_departed`); **media devices have no
    removal path at all** -- `media_add_dev()` only ever appends. #780 is the standing warning
    about what half-tracked media identity costs: a stale entry that names one disk and reads
    another is worse than a missing one, because the miss is refused loudly and the swap is not
    noticed.

    **The wedge is injected, not waited for.** It has appeared in three boots out of eleven, and
    every observation so far has cost a cold boot, an operator session and a verbal report. A
    recovery path validated that way would take weeks and still be under-tested. So a build-gated
    fault injection fakes the command timeout on demand, which makes the whole path -- abort,
    refusal, teardown, reset, re-enumeration, re-claim -- deterministic under QEMU, and leaves
    exactly ONE hardware run: the confirmation that a real wedge recovers.

    **Rate-bounded and counted.** Three resets per controller inside a sliding ten-minute
    window, then the controller is left dead and said to be dead until the window clears. A
    controller that needs resetting every few minutes is a different bug, and a recovery good
    enough to hide it is a recovery that stops anyone finding it. (Revised 2026-09-02, #792: the
    first form was a fixed count per run, and boot 42 spent all three in 44 minutes on a stall
    that recurs every 5-25 minutes; a fourth would have ended input for the rest of the run. The
    stall is that controller's steady state, so the bound has to distinguish "every few minutes"
    from "every half hour", which only a rate can.)

    Decomposed into five tickets rather than one: the injection harness, the ownership facts and
    the refusal, the teardown, the reset and re-enumeration, and the single hardware
    confirmation. The first two are independent of each other; nothing else can be written
    honestly until they exist.

76. **The guest LAPIC's IRR is a published VIEW of the VMM layer's pending-vector set, not a
    register the LAPIC model owns -- decided (2026-08-31).**

    `devices/guest_lapic.h` used to state that the IRR (0x200-0x270) was deliberately not
    modelled, "because there is no point in this design where a vector is known to be
    requested-but-not-delivered, so any value put there would be invented". That premise was
    false when it was written and is what #789 cost. The VMCB/VMCS layer's per-vCPU
    `pending_irr[8]` is exactly the requested-but-not-delivered set, it uses the architectural
    IRR layout already, and it is not a rare state: a Windows 10 guest with two vCPUs left its
    AP spinning with IF=0, polling its own IRR, while hype held two vectors it could not inject
    and coalesced 16,777 further attempts away.

    Measured on the #789 rig, one 300 s run each, same media and config, the AP:

    | vCPU 1 | IRR reads 0 | IRR published |
    |---|---|---|
    | NPFs claimed by the LAPIC | 16,340,460 | 65,744 |
    | last faulting address | 0xFEE00210 (IRR1) | 0xFEE000B0 (EOI) |
    | interrupts injected | 2 | 18,643 |
    | injections coalesced away | 16,777 | 20 |
    | pending / IF at the end | 2 / IF=0 | 0 / IF=1 |

    The guest went from never reaching its own CD driver to issuing the full `cdrom.sys`
    command set, matching what one vCPU had always achieved.

    **A view, not a copy.** The LAPIC model does not own this state and must not: a vector lives
    in `pending_irr` from request to injection, and a second copy here would be a second thing to
    keep in step -- the failure mode decision 51's shared-line rule already warns about one level
    down. `hype_guest_lapic_set_requested()` refreshes the view, and the two places that serve an
    IRR read (the MMIO NPF handler and the x2APIC MSR path, per backend) call it first. A stale
    view is worse than none: a guest polling the IRR to decide whether to keep waiting must see
    the queue as it stands now, not as it stood at the last injection attempt.

    The honest limit is unchanged in shape and now stated the other way round: the ISR is marked
    at commitment, so a bit can be set marginally before the guest takes the interrupt; the IRR
    is read at fault time, so it is exact for the reader it exists for.


77. **A USB display adapter is the host console's display when the GOP device is passed
    through to a guest -- decided in principle (2026-09-03), gated on #700.**

    GPU passthrough is still a v1 non-goal (§1) until PASSTHRU-1 (#700) lands its own
    decision; this one records what that decision must include so it is not discovered on
    the machine. Passing the only GPU to a guest takes the framebuffer hype draws its
    dashboard on. Without a second display the operator loses the host view exactly when it
    matters most: during the bring-up of a device hype has just given away. A USB display
    adapter plugged into the host is that second display, driven by hype's own xHCI stack
    (§6b, #793).

    **Selection is at boot, not on the fly.** If `hype.cfg` assigns the GOP's PCI device to a
    guest, hype must find a supported USB display adapter before it starts that guest, bind
    the dashboard to it, and refuse the passthrough if none is present -- the same
    "refuse loudly rather than run blind" rule §6i's admission control applies to storage.
    Hot-plugging the adapter later is not required; hot-unplugging it is handled like any
    other USB departure (decision #73) and leaves the dashboard headless, which the log
    records.

    **The protocol question comes first.** DisplayLink's DL-3xxx/6xxx protocol is
    proprietary and undocumented; the class-standard candidates and any adapter with an
    open or reverse-engineered protocol must be surveyed (research-provenance skill) before
    hardware is bought. #793's first task is that survey; nothing else in it starts until
    an adapter with a workable protocol is named. The dashboard code is unaffected: it draws
    into a linear framebuffer today and keeps doing so; the adapter driver's job is to move
    that framebuffer to the device at a rate the dashboard's refresh (§6b) already tolerates.

78. **USB Ethernet adapters are a supported host-NIC source, class driver first -- decided
    (2026-09-03).**

    Decision #36's family list is PCI-only. A machine that hands its PCIe NIC to a guest
    (#699), or a laptop with no second NIC, has no uplink for §6e's NAT. USB Ethernet is the
    ordinary answer on real machines and hype already owns the transport: every USB
    adapter sits behind the xHCI stack that carries the log sink and the boot medium.

    **CDC-ECM/NCM first, vendor chips after.** The USB Communications Device Class is
    documented by the USB-IF and needs no vendor knowledge; many adapters and every phone
    tether speak it. ASIX AX88179 and Realtek RTL8153 are the two vendor families worth a
    driver, in that order, only once the class driver carries traffic on real hardware.

    **Same vtable, different transport.** A USB NIC is one more driver behind decision #34's
    NIC vtable; the forwarding plane (decision #36) does not learn what carries the frames.
    What differs is the data path: bulk transfers on the xHCI stack instead of a PCI DMA
    ring, so this driver shares decision #75's rule that the controller carrying the log
    sink is never reset, and adds a new consumer to the USB transfer lock the input tick
    and the log flush already share (#484). Throughput is bounded by that lock and by
    USB 3 bulk, not by the NIC; the bar is "an uplink that works", measured against the
    PCIe path, not parity with it. Ticket #794. This extends decision #36 and does not
    reopen its endpoint/forwarding-plane boundary.

79. **Remote management is a v1 goal: a web API and a web UI, served by a TLS-only
    listener inside the hypervisor core -- decided (2026-09-03, #492 MGMT-1). The
    management-VM-plus-control-channel alternative decision #36 proposed is rejected.**

    **What was decided.** hype accepts inbound connections on one management port. Behind it
    sit, in order: MGMT-2's TCP state machine and TLS termination (#500), MGMT-3's identity and
    roles (#501: admin acts on every VM, viewer reads every VM, user reads only assigned VMs),
    MGMT-4's versioned API derived from the dashboard's own command table (#502, `/api/v1`,
    OpenAPI document in the repo, every input through the same validators the config and
    command parsers already use), MGMT-5's web UI as a client of that API and nothing more
    (#503), and MGMT-6, which discharges decision #36's argument in code (#504). The API
    exposes what the local dashboard exposes -- VM list and state, start, stop, shutdown,
    force off, config read and edit, create, delete, attach and detach -- and no more. If the
    UI needs something the dashboard cannot do, the dashboard's command table grows first.

    **Where the listener runs, and why not in a guest.** Decision #36 said that if remote
    management ever came, it should be a service in a guest with a narrow host-side control
    channel. That alternative was considered again and rejected for three reasons. First, it
    needs a guest OS image to exist, boot and stay healthy before the host can be managed at
    all -- the host cannot be reached precisely when it matters most, when no guest is
    running. Second, the control channel is itself a parser in ring 0 reachable from a guest
    that is, by construction, on the network; it moves the attack surface rather than
    removing it, and adds a second trust tier (a VM allowed to command other VMs) that §6g's
    isolation invariant has no place for. Third, the operator's requirement is a bare-metal
    appliance managed from a browser with nothing else installed; a management VM is a
    second machine to operate. The listener is therefore in the core, with the cost paid
    openly, below.

    **The requirements the reversal inherits.** Decision #36's objection was that a listener
    puts pre-authentication, attacker-reachable parsing inside a ring-0 payload with no
    process to lose. That is true and becomes the design bar:
    - **Off by default.** One `hype.cfg` key enables remote management; absent, no port is
      bound and the TCP code is never entered.
    - **TLS only.** Plaintext to the port is refused -- not served, not redirected. One
      TLS version, one cipher suite family, chosen in MGMT-2 with the freestanding crypto it
      brings; the host key lives in the varstore.
    - **Authentication before anything else.** The only bytes an unauthenticated peer can
      make hype parse are the TLS handshake, the HTTP request line and headers, and the
      login body. MGMT-6 enumerates that list, bounds every field (request line, header
      count and size, body size, per-connection timeout -- exceeding a bound closes the
      connection instead of allocating), fuzzes those parsers in the host test suite,
      rate-limits and locks out failed logins, and audit-logs every authentication and
      every state-changing call.
    - **Default deny.** No request does anything until it is authenticated and authorised
      against MGMT-3's roles. Credentials come from TERM-16's store and are never stored by
      the API.
    - **Blast radius, stated.** A fault in the listener is a fault in ring 0: there is no
      privilege to drop and nothing to restart short of the DEADMAN warm reboot (§6h). The
      mitigations are bounds, fuzzing and the off-by-default key; they do not change what a
      successful exploit owns. An operator who does not accept that leaves the key unset and
      has exactly the v1 hype this decision found.

    **What is still refused.** Any second listener (§6e names the two allowed endpoints:
    the DHCP client and this port). Any plaintext or unauthenticated endpoint. Any UI
    capability that is not an API endpoint. Multi-hypervisor master/mesh linking stays in
    §13. SSH (#179, V2-MGMT-1) stays v2 and is not covered by this decision.

    **Records changed.** §1's non-goal removed; §6b rewritten; §6e's listener rule gains its
    second named exception; decision #36 carries the amendment note; §13's two remote-
    management bullets are stubs pointing here; §15's "board ahead of the plan" note is
    closed. AGENTS.md carries no inbound-connection line, so nothing there changes.

80. **PCI-e device passthrough: the authorization boundary -- decided (2026-09-03, #700
    PASSTHRU-1). The invariant becomes "no guest gets UNAUTHORIZED direct hardware access";
    AMD-Vi lands first; assignment is by durable identity with a ceremony at least as strict
    as decision #8's; hype owns every reset; nothing else moves.**

    **1. The invariant.** Amended on 2026-08-23 and ratified here: *no guest gets
    unauthorized direct hardware access.* Authorized means all of: the PCI function is named
    in that VM's own `hype.cfg` by durable identity; its DMA is confined by the IOMMU to that
    VM's GPA range; its interrupts are remapped so they can target only that VM's vCPUs.
    Every function not so named stays exactly as absolute as before: host driver plus emulated
    frontend, never guest DMA. This is one narrow exception, not a loosening; §6j, §14.1,
    §14.2, §14.8, `.learnings/invariant-authorized-hw-access.md` and AGENTS.md carry the same
    words.

    **2. AMD-Vi first, VT-d second, one interface.** The 5950X desktop is the only validation
    machine with PCIe slots and a discrete GPU to assign; both Intel machines are laptops
    with an integrated GPU and a BitLocker system disk, and the Intel nested box is a VM.
    The SVM backend is also the more proven one (this week's VMX runs found two host-level
    faults, decisions #795/#796). So #701 is the AMD-Vi driver, written behind an IOMMU
    vtable of decision #34's shape (attach/detach a function to a domain, map/unmap a GPA
    range, remap an interrupt), and the VT-d driver is a second implementation of that
    interface, not a parallel design. No VT-d code is written until AMD-Vi carries a device
    on hardware (#707's AMD leg).

    **3. What "assigned" means in `hype.cfg`.** A device block, in the shape `[disk.*]`
    already uses:

    ```
    [device.gpu0]
    type = pci
    backing = passthrough
    id_match = 10de:2484           ; vendor:device, mandatory
    bdf = 0000:01:00.0             ; mandatory, must agree with id_match
    serial = <DSN or subsystem id> ; mandatory when the function exposes a DSN capability
    allow_passthrough = true       ; the explicit per-device flag, like allow_overwrite
    ```

    and `devices = gpu0` in one `[vm.*]` section. Both `id_match` and `bdf` must match the
    enumerated function or the device is refused with both values printed; a function named
    by BDF alone, by slot order, or by "the GPU" is never assigned. **Whole IOMMU groups
    only:** every function that shares the group (a GPU and its HDMI audio function) is
    assigned to the same VM or none is. One VM per group, ever; two VMs naming the same
    group is a config error at admission (§6i).

    **4. The ceremony.** At least decision #8's, because a wrong assignment hands a DMA
    engine to a guest:
    - identity match as above, at every boot, against what the bus enumerates now;
    - an interactive confirmation on the local dashboard naming vendor, device, BDF and the
      VM, before the function is detached from the host, every boot, not scriptable
      (the same rule #125 applies to physical writes);
    - **refusals that no flag overrides:** any function in the IOMMU group of the controller
      carrying hype's boot medium or log sink (decision #75's role record, #783), the xHCI
      carrying the operator's keyboard, the NIC that is hype's NAT uplink, the IOMMU itself,
      any host bridge, and the GOP device unless a decision #77 USB console is bound first;
    - refusal when the host has no IOMMU, or the IOMMU does not cover the function, or the
      function is not in a group of its own or of functions all assigned to the same VM.
    A refused assignment stops that VM at admission and says why; it never falls back to an
    emulated device silently.

    **5. Reset and failure ownership: hype, always.** Before the function is handed over:
    bus-master disable, FLR (or #706's vendor quirk path for a GPU that has none), IOMMU
    domain created and the VM's GPA range mapped, interrupts remapped, then the guest sees
    the function. On every teardown -- Stop, Shutdown, Force off, a guest crash, the DEADMAN
    path -- in this order: bus-master disable, wait for DMA to quiesce, FLR, IOMMU domain
    destroyed, and only then §6h's RAM zeroing and reuse. A function that fails its reset is
    **quarantined** for the rest of the run: never returned to host use, never reassigned,
    named in the log, the same "left dead and said so" rule decision #75 uses for a
    controller. Handing the function back to a host driver is not planned: hype has no
    driver for a GPU, and a NIC assigned to a guest is not the uplink (refusal above).

    **6. What does not change.** Guest isolation (§6g): re-derived with a real device by #705
    before any assignment is called validated; until #705 passes, passthrough is a
    diagnostic build. CPU-time isolation (§3, decision #39): untouched. Guest RAM zeroing
    (§6h): untouched in substance, ordered after DMA quiescence above. The physical-write
    guard (decision #8): untouched; a passthrough disk controller is refused if it carries
    any `physical:` target. §6j software validation for every device hype drives: untouched,
    and the IOMMU's incidental cover of hype's own rings is not relied on. Decision #51's
    shared-line rule for emulated devices: untouched; remapped MSI/MSI-X exists for assigned
    functions only. Nested virtualization and SR-IOV: still out.

    **Rejected.** Assignment by BDF alone (moves when a card is added); a global
    `passthrough = on` switch (authorization is per device, per VM); handing a device back
    to the host after a guest (no consumer, and a failed reset would be invisible);
    starting with VT-d (no machine to validate on); allowing a partial IOMMU group (the
    other function's DMA would be unconfined).

    **Board.** #699's sub-issues are unblocked in the order this decision implies: #701
    (AMD-Vi), #702 (interrupt remapping), #703 (assignment framework and the `[device.*]`
    block), #704 (reset), #705 (isolation re-derivation), #706 (GPU quirks), #707 (hardware,
    AMD leg first), #793 (USB console, decision #77). #130 (STRETCH-3, passthrough NIC) is
    a consumer of the same framework and is not a separate design.

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

- ~~**Web API/UI for remote management, with multi-hypervisor master/mesh
  linking**~~ (noted 2026-07-14) — **the web API and web UI were PROMOTED TO
  v1 on 2026-09-03**, §10 decision #79, the MGMT milestone (#492, #500-#504).
  Only the multi-hypervisor master/mesh linking part stays here as future
  work: link several hypervisor instances under one master, or have them
  manage each other as a mesh; a topology to design between, not both by
  default.

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

- ~~**Remote management of hype itself**~~ (noted 2026-08-15) — **PROMOTED TO
  v1 on 2026-09-03**, §10 decision #79. The two constraints this bullet
  recorded from decision #36 were answered there: the consumer is the operator
  of a bare-metal appliance managing VMs from a browser (MGMT-3's three roles),
  and the service runs in the core rather than in a management VM, with
  decision #36's objections converted into MGMT-6's requirements.

- **`size_gb` on a `backing = file` disk CREATES the image** (noted 2026-08-27,
  from #740). Today both size keys are declarations hype *validates*, never
  instructions it obeys: `target_disk_size_gb` since #331, and `[disk.*]
  size_gb` since #740. hype has no post-`ExitBootServices` filesystem
  allocator and only ever writes in place, so an image must already be fully
  allocated on the volume, and both keys warn when the file's real size
  disagrees with the config.

  An operator plainly expects a `size_gb` on a file-backed disk to make the
  file, and the machinery already exists -- the terminal's `mkdisk` builds a
  sparse raw image through `HYPE_FS_CAP_WRITE_GROW` (§10 decision 69) -- so
  this is wiring rather than new capability. It is deferred, not rejected,
  because "the config file causes a multi-gigabyte allocation at boot" is a
  behaviour change with its own failure surface that v1 has no answer for:
  the volume filling mid-write, a partial image left behind by a boot that
  was interrupted, and an existing file of the wrong size (grow it? refuse?
  ignore it?). Each needs a decided answer before the key can be made to act,
  and #331's and #740's validation is the honest behaviour until then.

## 14. Gap analysis (2026-08-21)

A structured review of fifteen virtualization areas against this plan, the
board, and the code as it stands. Each subsection states what exists (with
file or ticket citations), what is missing, and the disposition: an existing
ticket, a new ticket raised from this review (#599–#604), or
deliberately-not-planned with the reason. This section records findings; it
changes no existing decision. Where a finding contradicts a §10 decision,
that is said explicitly rather than silently absorbed.


### 14.1 IOMMU / DMA remapping (AMD-Vi / VT-d)

Current state: no IOMMU use anywhere in the tree. §4 already rules: "IOMMU
required only if we ever add passthrough — out of scope for v1." No guest can
program a real device (§6j hard rule; the no-direct-hw-access invariant), so
there is no guest-controlled DMA to isolate. Host-driver DMA targets
(AHCI/NVMe/xHCI/NIC rings, and guest-RAM buffers passed to
`hype_blk_backend_*`) are validated in software against the VM's GPA map
before any controller is programmed.

Gap: hype's own memory has no hardware backstop against a misprogrammed or
malicious device — software validation is the only line.

Disposition: **promoted by decision #80 (2026-09-03), for assigned devices
only.** #701 (PASSTHRU-2) is the AMD-Vi driver, VT-d follows behind the same
interface; the IOMMU ships with the first assigned device and is mandatory for
it. For every device hype drives itself, §6j software validation stays the
boundary; the IOMMU's coverage of hype's own rings is defense-in-depth that
nothing relies on.

### 14.2 Interrupt remapping and MSI/MSI-X virtualization

Current state: interrupt remapping is an IOMMU facility and is absent for the
same reason as 14.1. Guest-facing MSI/MSI-X is deliberately not modeled:
every device answers NO_VECTOR (`devices/virtio_blk.c`,
`devices/virtio_net.c`, `devices/e1000_dev.h`) and delivery is by shared
IO-APIC line, per decision 51's explicit rejection of MSI-X "before something
needs MSI for its own sake." #514 fixed the one MSI-adjacent defect (config
cycles from AP vCPUs).

Gap: none beyond what decision 51 already defers.

Disposition: **deferred by decision 51 for emulated devices; met for assigned
ones.** A passthrough device cannot share an IO-APIC line, which is exactly
decision 51's stated trigger, so #702 (PASSTHRU-3) remaps its MSI/MSI-X to the
owning VM's vCPUs through the IOMMU (decision #80). Emulated devices are
unchanged: NO_VECTOR, shared lines.

### 14.3 APIC / x2APIC virtualization

Current state: the trap-and-emulate xAPIC path is what runs everywhere.
AMD AVIC landed as build-time opt-in, default OFF (#193;
`-DHYPE_ENABLE_AVIC=1` in `boot/main.c`). On Intel, the APICv secondary
controls M2-4 defined are **deliberately dropped** at VMCS build time
(`arch/x86_64/vmx/vmcs_hw.c`: APIC_REGISTER_VIRT, VIRTUAL_INTERRUPT_DELIVERY
and USE_TPR_SHADOW are not requested). x2APIC guest mode is masked (CPUID
leaf 1 ECX bit 21 forced clear, `arch/x86_64/cpu/cpuid_emulate.c`); the
x2APIC MSR range is unhandled by default.

**#601 update:** the x2APIC MSR interface (0x800-0x8FF) and the
IA32_APIC_BASE mode state machine now exist, over the same per-vCPU model
`devices/guest_lapic.c` already used for xAPIC MMIO -- but both are build-time
opt-in, default OFF, same convention as AVIC (`-DHYPE_ENABLE_X2APIC=1`; the
gate lives in `arch/x86_64/cpu/cpuid_emulate.c`, `arch/x86_64/svm/svm_vcpu.c`,
`arch/x86_64/vmx/vmcs_hw.c`). Default builds are unaffected: CPUID leaf 1 ECX
bit 21 stays forced clear and the MSR range stays unrecognized exactly as
before this work. Pending: a QEMU Linux-guest boot with x2apic enabled in
dmesg, and the same real-hardware validation gate #600/#605 set for AVIC/APICv,
before the default can move.

Gap: this contradicts decision 6 and §4, which require APICv/AVIC "from the
start." M2-4 (#30) is closed against that decision, but only structures and
the AMD opt-in exist; no accelerated path is active by default on either
vendor, and Intel has none at all.

Disposition: four tickets, governed by decision 58 (defaults ON once
bare-metal-validated per vendor). **#599** wires VMX APICv. **#600**
validates AVIC on bare-metal AMD and flips the AMD default ON. **#605**
validates APICv on bare-metal Intel and flips the Intel default ON.
**#601** (Low) models x2APIC guest mode. Decision 6 stays as written;
decision 58 + these tickets are the path back to it.

### 14.4 NPT/EPT corner cases

Current state: both backends use fixed 2 MiB leaf mappings, WB memory type,
built once per VM (`arch/x86_64/svm/npt.c`, `arch/x86_64/vmx/ept.c`). There
is no 4 KiB level and no split path — guest MMIO windows are 2 MiB-aligned
not-present holes by design (`npt.h` states this; decision on device windows
follows it). Not-present entries are L1TF-inverted per decision 48. A/D bits
are not enabled. Guest PAT/MTRR are modeled (#481 fixed reset state; g_pat
WB fixed the uncacheable-guest-RAM defect). Per-VM roots and ASID/VPID
separation are fixed and tested (#244, #245, #272, #273).

Gap: no dirty tracking and no page-granular permissions — but nothing in v1
consumes them (no live snapshot §1, no ballooning §13). The 2 MiB constraint
is a real design limit every new device window must respect; it is
documented where it binds.

Disposition: **covered by design.** No ticket. A 4 KiB/split mechanism gets
built when a consumer exists (the no-premature-abstraction rule), and the
first consumer must say so here first.

### 14.5 VMX/SVM capability handling

Current state: VMX control bits are negotiated against the capability MSRs,
TRUE variants preferred, granted bits read back rather than assumed
(`arch/x86_64/vmx/vmcs_hw.c`); VPID is gated on IA32_VMX_EPT_VPID_CAP
(#273); CR0/CR4 fixed bits applied before VMXON
(`arch/x86_64/vmx/vmx_enable_hw.c`). SVM features come from CPUID 0x8000000A
with graceful nRIP/decode-assist fallback (`arch/x86_64/svm/svm_vcpu.c`).
Nested honesty: guests never see VMX/SVM CPUID bits (#552, #316), and the
SVM instruction set is intercepted (#317) — nested virtualization is a §1
non-goal and is honestly absent rather than half-advertised.

Gap: decision 48 already records the one known hole (IA32_ARCH_CAPABILITIES
is never read; the L1TF mitigation is unconditional instead).

Disposition: **covered.** No ticket.

### 14.6 Guest SMP (INIT/SIPI, >1 vCPU)

Current state: INIT-SIPI-SIPI AP bring-up is implemented and hardened on
both backends (#188; #520 ported the SIPI CS fix to VMX). Cross-core VMCS
ownership is a §10 hard invariant (decision 43, #523). Correctness work is
live on the board: #526 (nested-VMX soft lockups), #527 (bare-metal Intel
VMCS-ownership validation), #525, and the SMP-1x shared-tier series
(#467–#478).

Gap: none untracked.

Disposition: **covered by existing tickets.** No new ticket.

### 14.7 ACPI / UEFI table correctness

Current state: per-VM synthesis of RSDP/XSDT/FADT+FACS/MADT/MCFG, a compiled
DSDT with PCI host bridge + _PRT, optional HPET (default absent — #436
found Windows bugchecks with it advertised), and TPM2+SSDT when a TPM is
configured (`devices/acpi.c`, `devices/dsdt_aml.h`; #60, #62, #312, #433).
Unit-tested (`core/tests/test_acpi.c`, `test_acpi_loader.c`). Guest UEFI is
real vendored OVMF (decision 1), so Runtime Services are OVMF's own, backed
by the pflash varstore hype persists (#457, #441) — hype does not synthesize
runtime services.

Gap: none found.

Disposition: **covered.** No ticket.

### 14.8 PCI device reset (FLR) and bus isolation

Current state: no device is ever assigned to a guest (no-direct-hw-access
invariant), so guest-side FLR has nothing to reset. Host-side, hype resets
the controllers it takes over during bring-up (NVMe CC.EN toggle in
`core/nvme_host_hw.c`; xHCI reset + settle in `core/xhci_hw.c`). Each VM has
its own emulated bus and device instances; the shared-singleton class of
defect was hunted and fixed (#245, #277, #563). Emulated devices reset per
their own contracts (virtio device_status=0 path is modeled and tested).

Gap: none — FLR is a passthrough concept.

Disposition: **promoted by decision #80.** #704 (PASSTHRU-5) is the FLR path;
decision #80 fixes who resets when (hype, before assignment and after every
teardown, with a failed reset quarantining the device for the run).

### 14.9 virtio completion and error paths

Current state: `process_virtio_blk_queue()` surfaces backend failures,
malformed chains, out-of-range LBAs and out-of-map segments as S_IOERR, and
unknown request types as S_UNSUPP (`arch/x86_64/svm/svm_vcpu.c`,
`devices/virtio_blk.c`); driver-initiated device reset re-runs negotiation
without leaking identity state (#310). All of it is unit-tested, including
the adversarial shapes (`core/tests/test_virtio_blk.c`, `test_virtio_net.c`).
SEG_MAX is offered (the max_segments=1 defect is fixed).

Gap: none directed; what remains is the undirected-input class.

Disposition: **covered**; adversarial coverage folds into the fuzz ticket
(**#602**).

### 14.10 Migration / snapshotting

Current state: live migration and VM memory snapshotting are §1 non-goals.
What exists is deliberate and narrower: the varstore persists per VM (#441),
and the §6h run-state record restarts the same VM set after a host power
event — explicitly restart-to-run-state, not resume.

Gap: none within scope.

Disposition: **deliberately not planned.** A single-box hypervisor with no
peer has no migration target, and RAM-state snapshotting buys little for
install-and-run workloads at the cost of dirty tracking (14.4) and device
state serialization for every model. Disk-image snapshotting stays #131
(STRETCH).

### 14.11 Fuzzing

Current state: directed adversarial unit tests exist at the §6j boundary
(malformed virtio chains, out-of-bounds LBAs); the only fuzz-style loops are
`core/tests/test_vt_fuzz.c` (host VT parser) and #504's planned MGMT
pre-auth fuzz. No guest-facing surface is fuzzed.

Gap: the guest-writable surfaces — virtio descriptors, AHCI/NVMe command
structures, MMIO/PIO register models, `hype.cfg` — get only inputs someone
thought of. §6j says a missed check here is a compromise, not a crash.

Disposition: new ticket **#602** — a host-side, sanitizer-backed fuzz
harness over the real device-model code in the unit-test build.

### 14.12 VM-exit testing

Current state: exit coverage is incidental. Microtests exercise whatever
exits their workloads take; `core/tests/test_vmexit.c` covers only the pure
classify/decide helpers. The EXHIST counters bucket exits per reason at run
time. History shows the cost of incidental coverage: #315, #291 and #317
were all found late, by guests.

Gap: nothing enumerates the intercept set and proves each reason is taken
and handled on both backends.

Disposition: new ticket **#603** — a systematic exit-reason coverage
microtest, cross-checked against the EXHIST counters.

### 14.13 Security hardening of hype itself

Current state: the guest-facing boundary is §6j validation plus the §10
decision 48 L1TF mitigation, and guests see no VMX/SVM (14.5). hype's own
environment, however, is unhardened: the host identity map is
PRESENT|WRITE with no NX anywhere (`arch/x86_64/cpu/paging.c`), so all host
RAM — guest RAM included — is executable at CPL0; there is no W^X for the
image, no SMEP/SMAP, and no stack canaries (`Makefile` CFLAGS; `chkstk.S`
is probing, not smash detection).

Gap: one missed bounds check currently escalates directly to code
execution, because overwritten data and injected code share writable,
executable memory.

Disposition: new ticket **#604** (Low) — NX for everything outside `.text`
(guest RAM first), W^X for the image, stack canaries, SMEP; each step
independently landable and gated on real-hardware runs.

### 14.14 NUMA and huge pages

Current state: guest RAM is backed by 2 MiB nested-paging mappings by
construction (14.4), carved from the one Phase-0 pool (§2). NUMA-aware
vCPU/RAM placement is already a board item: #475 (SMP-19).

Gap: none untracked. 1 GiB backing pages would shrink walk depth further but
no measurement motivates them.

Disposition: **covered by #475**; 1 GiB pages deliberately not planned until
a measurement asks for them (the measure-before-optimizing rule).

### 14.15 Observability and debugging

Current state: broader than the board suggests — per-reason EXHIST exit
buckets and per-VM AHCIREG/AHCIIRQ lines in the periodic log
(`boot/main.c`), a bounded per-port I/O histogram (`core/io_histogram.c`),
write-size histograms in `blk_backend`, deep guest-state dumps on faults and
preemption anomalies (`fw_1_436_deep_dump`), a per-VM watchdog
(`core/vm_watchdog.c`), per-core panic accounting, a panic flush hook that
persists the console log to the USB stick (`core/fatal.h`, #513), the USB
log split, and the dashboard's per-VM state.

Gap: none large enough to carry a ticket. The known soft spot — several
counters live as FW-1-era statics in `boot/main.c` — is a refactor-in-place
concern (#539's pattern), not missing capability.

Disposition: **covered.** No ticket.

## 15. Gap analysis, deep pass (2026-08-21)

A second structured pass, over the sub-areas §14 did not examine, across
thirteen categories. Same rules as §14: findings with citations, dispositions,
no existing decision changed. Where §14 or its tickets (#599–#605) already
dispositioned a sub-area, one line points there rather than re-analysing. New
tickets from this pass: **#606–#611**.

### 15.1 CPU virtualization core

- Bring-up and capability detection: §14.5.
- **VMCS/VMCB lifecycle**: VMX cross-core ownership and hand-off is §10
  decision 43 (hard invariant, violation-counted). SVM VMCB clean bits are
  modeled but always written 0 — "nothing clean, always reload"
  (`arch/x86_64/svm/vmcb.h:62`; every control mutation clears them,
  `svm_vcpu.c`). Correctness-first by design; a clean-bits optimization is
  deliberately not planned until a measurement asks for it (§14.14's rule).
- **Exit dispatch and unknown exits**: unknown MSRs are tolerate-and-trace-once
  (`arch/x86_64/svm/svm_vcpu.c:2259`), unknown port I/O is
  absorb-and-report (`hype_vmx_vcpu_handle_unknown_ioio`,
  `hype_svm_vcpu_handle_unknown_ioio`), and an exit hype cannot attribute
  stops that VM alone per decision 46. Systematic per-reason proof is #603.
- **CPUID/MSR interception policy**: SVM runs a default-intercept MSRPM with
  an explicit passthrough allowlist (`svm_vcpu.c:421 g_msrpm_passthrough`);
  VMX enables no MSR bitmap at all, so every guest MSR access exits — the
  safe posture, paid in exits. CPUID emulation is centralized and unit-tested
  (`arch/x86_64/cpu/cpuid_emulate.c`; vendor honesty #298, brand string
  #361). Covered; a VMX MSR bitmap is a perf item with no measurement behind
  it, not planned.
- **Exception/NMI injection**: EXITINTINFO re-staging is modeled (#315;
  #580's retire-HLT-before-inject ordering landed in `22997ce`); NMI
  injection exists (#484, `hype_svm_vcpu_inject_nmi`); STI/MOV-SS/NMI
  blocking windows are respected on entry (`vmcs_hw.c:4586`). Double-fault
  escalation happens inside the guest — exceptions are not intercepted by
  default (VMX exception bitmap 0, `vmcs_hw.c:901`), so hype never has to
  synthesize #DF. Correct by structure; window-by-window proof folds into
  #603.
- **TSC/clock virtualization**: the guest TSC is the raw host TSC —
  tsc_offset 0, no intercept, no scaling, no IA32_TSC_ADJUST model
  (`svm_vcpu.c:2176`; `hyperv.c:100`); TSC_DEADLINE and x2APIC are honestly
  masked (`cpuid_emulate.c:288`; x2APIC is #601). Sound on the dedicated tier
  because a vCPU never changes core and hosts require invariant TSC (#555).
  The shared tier's core migration (SMP-13 #469) must keep guest-TSC
  monotonicity, and SMP-20's proof (#476) should include it — noted here; the
  SMP series owns it, no new ticket.
- **Nested virtualization**: honestly absent (§14.5: CPUID masked #552/#316,
  SVM instructions intercepted to #UD #317). On VMX the guest also cannot set
  CR4.VMXE — it is host-owned in the CR4 guest/host mask and hidden from the
  read shadow (`vmcs_hw.c:1088–1106`) — so a guest VMXON takes #UD in-guest
  rather than reaching an unhandled exit. Covered.

### 15.2 Memory

- EPT/NPT structure, huge pages, permissions granularity: §14.4. Dirty-page
  tracking: §14.4 — still no consumer (#131 snapshots disk images, not RAM).
- **Guest #PF vs NPF attribution**: guest #PF is delivered natively (the
  exception bitmap/mask excludes it), so a guest page fault never reaches
  hype and cannot be misattributed; NPF/EPT-violation is the only
  nested-paging exit and is attributed per decision 46. Covered; folds into
  #603's reason coverage.
- **Guest-side W^X**: nested paging grants RWX over all guest RAM
  deliberately — the guest's own page tables are its W^X mechanism. What hype
  actually withholds from guest self-defense today is speculation control,
  which is #608 (15.11).
- **MMIO window registration hygiene**: the two-places trap (AP loop vs BSP
  chain) was closed by #482/#576 — one shared dispatch entry point. Proof
  that every registered window and every hole behaves is #603's job.
- **Ballooning / memory hotplug**: ballooning is §13 v2, explicitly out of
  v1; RAM hotplug is not planned anywhere — §6i's fixed-size admission model
  is the design.

### 15.3 Interrupts

- LAPIC virtualization: §14.3 (#599/#600/#605). x2APIC: #601. MSI/MSI-X:
  §14.2 / decision 51. Remapping: §14.1. Posted interrupts: arrive with
  APICv (#599) or not at all.
- **IO-APIC pin exhaustion**: decided, not merely observed — decision 51: all
  24 pins are allocated, a new device shares a line and extends that line's
  single pending-OR computation, and widening the IO-APIC requires an
  explicit argument. Handled by design; no ticket.

### 15.4 PCI and devices

- **Guest config-space model**: 256-byte config space, bus 0 only, no
  PCI-to-PCI bridges — documented scope (`devices/pci.h:25`). Capability
  chains are real (MSI + SATA caps, `devices/pci.c:170`; the virtio chain
  #550); BAR sizing is modeled and proven by a real guest bus walk with
  self-programmed BARs (#547); slot aliasing fixed (#573). 64-bit BARs are
  not modeled (decision 26 records the 32-bit scope) and nothing on the fixed
  device board needs one. Covered; conformance beyond this folds into
  #602/#603.
- **virtio conformance**: VIRTIO_F_VERSION_1 is negotiated
  (`devices/virtio_blk.c:81`), SEG_MAX offered, reset and renegotiation
  tested (#310). EVENT_IDX and multiqueue are unoffered optional features
  with no measured need (15.13).
- **virtio-scsi**: not needed — the optical path is AHCI/ATAPI for every
  guest family (§6d) and the disk buses are decision 26's three plus
  `usb-msc` (decision 55). §6d's passing "or a virtio-scsi CD-ROM" wording is
  a never-chosen option, not a commitment. Deliberately not planned.
- Passthrough and IOMMU: §14.1. Reset/FLR: §14.8. **SR-IOV**: out of scope
  with passthrough (§1) — a VF is passthrough by definition.

### 15.5 Guest boot

- UEFI path: vendored OVMF, decision 1; Linux direct kernel: decision 45;
  Windows/Linux/BSD paths: their milestones (M4–M7, GLADDER, BSD).
- **BIOS/CSM**: a stretch goal by design (§6, STRETCH-1 #128) — but
  `firmware = legacy` parses, round-trips, and is then silently ignored: the
  VM boots UEFI OVMF anyway. **#607** — refuse at admission until #128
  exists.
- **MP tables (MPS)**: not synthesized, deliberately — decision 23's guests
  are 64-bit UEFI-era systems that consume the MADT (§14.7). A guest needing
  legacy MPS is out of scope.
- **SMBIOS**: synthesized per VM and unit-tested (`devices/smbios.c`,
  `core/tests/test_smbios.c`; Type 4 topology honesty fixed by #562).
  Covered.
- **Multiboot**: not needed — BSDs boot via their UEFI loaders (decision 23),
  and `boot = kernel` is deliberately bzImage-shaped only (decision 45).
  Deliberately not planned.

### 15.6 SMP and scheduling

- AP startup / INIT-SIPI: §14.6. Shared tier: SMP-11..22 (#467–#478).
- **vCPU/VM teardown**: the lifecycle machine including Force-off and Delete
  is implemented and unit-tested (`core/vm_lifecycle.*` + tests;
  `core/vm_delete.h`, TERM-15 #491), and the RAM re-carve reports reuse so
  the zeroing invariant cannot be skipped silently (`core/ram_pool.h`).
  Covered — except two compile-time caps that contradict decision 33:
  `HYPE_CFG_MAX_VMS 16` and `HYPE_RAM_POOL_MAX_CARVES 64`. **#606**.
- **Oversubscription**: admission-only on the dedicated tier, ratio-bounded
  on the shared tier (§6i, decision 39). Covered by design.
- **Affinity**: `cpu_set` is the documented model (§5, decisions 16/47).
  Covered.
- **Pause/resume**: implemented (M8-5 #116; `HYPE_CMD_STOP`/`RESUME`) with
  the state machine unit-tested. Covered.

### 15.7 Timers

- **PIT**: modeled (`devices/pit.c`); the open defect is bare-metal legacy
  PIT/8259 delivery, #557. Covered by that ticket.
- **HPET**: modeled (`devices/hpet.c`) with a per-VM ACPI table, default
  absent on #436's Windows evidence (§14.7). Covered.
- **RTC/CMOS**: periodic interrupt rates implemented (`devices/cmos.c:175`);
  the earlier frozen-clock and probe defects are fixed (#303/#304). Covered.
- **LAPIC timer**: one-shot and periodic modeled
  (`devices/guest_lapic.c:434–451`); TSC-deadline mode is honestly masked
  rather than half-modeled. Deliberately not planned until a guest needs it.
- **Paravirtual clocks**: both exist — kvmclock (`devices/pvclock.c`, keyed
  on the KVM CPUID signature; motivated by a real-hardware PIT-calibration
  failure) and the Hyper-V reference TSC page + frequency MSRs
  (`arch/x86_64/cpu/hyperv.c`, #436). Covered.

### 15.8 Storage / network devices

- virtio-blk: §14.9. virtio-net + NAT + peers: the NET milestone (closed
  #81–#86).
- **Switch/bridge**: NET-6 delivered with decision 53's semantics; L3 routing
  between switches and VLANs are deferred by §6e's own text (future NET-7);
  inbound DNAT is NET-8 #448, open. Covered by plan + board.
- **TAP devices**: not applicable — hype is bare-metal; its uplink is its own
  NIC drivers (§6e), not a host kernel's TAP.
- **Interrupt moderation / multiqueue**: single queue, INTx, per decision 51;
  no measurement asks for more (15.13).

### 15.9 Host infrastructure

- **Memory allocators**: one Phase-0 pool, carved by an allocator with loud
  exhaustion statuses, overlap refusal and unit tests (`core/ram_pool.h`);
  admission checks the budget (§6i). Covered — minus #606's fixed carve-table
  constant.
- Host paging permissions: #604. Host SMP startup: §14.6, plus the tracked
  intermittent AP-trampoline #GP (#461).
- **Host APIC abstraction**: exists and is unit-tested
  (`arch/x86_64/cpu/lapic.c`); a few raw `0xFEE00000` literals remain in
  `boot/main.c` — §14.15's refactor-in-place class, no missing capability.
- **Host PCI**: legacy 0xCF8/0xCFC config access only, deliberately
  (`core/host_pci.h`); no host-side ECAM, so extended config space (>0xFF)
  and PCI segments ≠ 0 are unreachable. Nothing hype drives needs either
  today; the first host device that does turns this line into a bug report.
  Noted, not planned.
- **Host ACPI parser**: none — topology comes from EFI MP services + CPUID
  with firmware-quirk repair (#378), the timebase from TSC-via-Stall
  (decision 37), PCI from brute-force enumeration. No host MADT/MCFG consumer
  exists; deliberately absent until one does.
- Host timer subsystem: decision 37, plus the #370 unguarded-RDMSR trap,
  both settled. BSP loop cadence: bounded slices are decided (decision 28)
  and its starvation regressions were found and fixed by measurement
  (#368/#374/#375). Logging/serial/panic: §14.15 + decisions 28/57.

### 15.10 Reliability

- Guest-facing fuzzing: #602 (its scope already names PCI config writes and
  hype.cfg). Exit coverage: #603. Watchdog: implemented and unit-tested
  (`core/vm_watchdog.c`, `core/tests/test_vm_watchdog.c`). Crash dumps:
  §14.15 — the panic flush persists the console log to the stick (#513); no
  further post-mortem artifact is planned (decision 28 retired the
  EFI-variable tail deliberately).
- **Lock discipline**: few locks by design — ownership-over-locking is the
  standing pattern (decision 57), and the two real cross-lock orderings are
  documented where they live (decision 52's mailbox rule; blk_usb's ticket
  lock + the controller-wide transfer lock, #343). There is no repo-wide lock
  inventory or runtime order detection; at this lock count a detector is
  disproportionate. The rule that keeps this true: new cross-core state gets
  an ownership or ordering argument in its §10 entry, as decisions 52/57 did.
  Noted; no ticket.
- **Deterministic tests / flake tracking**: microtest verdict and
  INVALID-retry rules (the `microtests` skill); a flake becomes a WATCH
  ticket (#578 is the live example). Covered by practice.
- **Privilege-boundary audit**: **#610** — enumerate every guest-writable
  interface and cite the §6j check and test at each; the complement of #602
  (fuzz exercises what the audit enumerates).

### 15.11 Security

- Isolation: §6g/§14. IOMMU/DMA containment: §14.1. Host NX/W^X/SMEP/
  canaries: #604.
- **Speculative-execution posture, two holes found**: guests are denied their
  own mitigations — IA32_SPEC_CTRL/PRED_CMD are not virtualized, so the
  masked CPUID bits make every guest kernel run unmitigated internally
  (**#608**); and hype itself runs guest-reachable indirect branches with no
  IBPB on exit, no IBRS, and no retpoline build — decision 48 covers L1TF
  only, SMP-18 (#474) flushes only on shared-tier trust-group switches
  (**#609**, Low, sibling of #604).
- **Secure/measured boot of hype itself**: unsigned-with-SB-off is decision
  5; the signing path is STRETCH-2 #129. Measuring hype into the host TPM
  (hype's own chain — distinct from #432/#433's guest TPM) is deliberately
  not planned: it has no v1 consumer, and it builds on the signing story
  decision 5 already defers.
- Privilege-boundary audit: #610 (15.10).

### 15.12 Operations

- Lifecycle API: decision 11 + TERM-9..15, all closed. Serial console and the
  operator terminal: the TERM milestone. Disk snapshots: #131 (STRETCH).
  Save/restore and migration: §14.10. Metrics/tracing: §14.15.
- **Remote management**: decision 79 (2026-09-03) promoted it to v1; the MGMT
  epic #500–#504 is the implementation, decision 36 carries the amendment.
  Plan and board agree.
- **Live introspection**: none on demand — deep dumps fire only on faults
  (§14.15), and the terminal has no "where is this guest right now" command.
  **#611** — a decision-43-safe published-snapshot dump from the terminal.

### 15.13 Performance

- Exit profiling: EXHIST buckets + the I/O histograms (§14.15). EPT/NPT
  perf: §14.4. NUMA: #475. Host TSC calibration: decision 37.
- **Perf regression harness**: none standing, deliberately — baselines are
  per-ticket measurements (#107/#234/#295 pattern), the open measured work
  sits on the PERF milestone (#295), and the shared-tier benchmark obligation
  is SMP-22 (#478). Not planned until a regression escapes this practice.
- **virtio multiqueue / interrupt moderation**: single-queue INTx is decision
  51's stance, and multiqueue additionally requires the MSI-X delivery path
  decision 51 defers. No measurement asks for it. Deliberately not planned.
- **Exitless / low-exit fast paths**: IPI/EOI acceleration is #599/#600;
  PIO/MMIO hot-exit work exists exactly where measurement justified it (#295
  coalescing; the #367/#368 history). No general fast-path framework is
  planned — the measure-before-optimizing rule.
