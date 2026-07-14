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
                       |-- Phase 0: UEFI application context
                       |     - Parse config (hype.cfg on ESP)
                       |     - Enumerate CPU features (VMX/EPT or SVM/NPT)
                       |     - Reserve memory (runtime services buffer) for
                       |       hypervisor + per-VM guest RAM
                       |     - Zero every page of each VM's reserved guest
                       |       RAM before that VM's first instruction runs
                       |       (§6f) — applies on every (re)start, not just
                       |       the initial hypervisor boot
                       |     - Load VM images / ISO installers from ESP or
                       |       attached storage into guest-reserved memory
                       |
                       |-- Phase 1: ExitBootServices() + take ownership
                       |     - Call ExitBootServices, become the only kernel
                       |     - Set up our own paging, IDT/GDT, APIC, timers
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
                             - Multiple guests run concurrently, each pinned
                               to 1..N vCPUs, own EPT/NPT address space
```

Everything before `ExitBootServices()` is a normal UEFI app using Boot
Services (file I/O via Simple File System protocol, memory via
AllocatePages). Everything after is our own tiny kernel: no dependency on
firmware runtime except UEFI Runtime Services we explicitly keep mapped
(time, variable services optionally, reset).

## 3. Why "thin"

- No general device driver model — a small, fixed board of virtual devices.
- No process/thread scheduler beyond exclusive 1:1 vCPU-to-pCPU pinning —
  avoids needing a real scheduler at all for v1. The operator can pin a VM
  to an explicit **subset** of host cores (§5 `cpu_set`) rather than the
  hypervisor always auto-assigning whichever cores are free; exclusivity
  (no two VMs' pinned sets overlap) remains a hard invariant, checked at
  startup (§6i), since it's what the fault-isolation guarantee (§6g)
  depends on.
- No filesystem in the hypervisor beyond what's needed to read guest images
  off the ESP/local disk (FAT32 via UEFI Simple File System pre-ExitBootServices,
  or a minimal read-only FAT/ext driver post-ExitBootServices if we need to
  load additional images after boot services are gone).
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
  tiny parser — no libc) enumerates guest definitions:
  ```
  [vm.win11]
  vcpus = 4
  cpu_set = 4-7             ; explicit host core subset to pin to (optional;
                            ; auto-assigned from whatever's free if omitted)
  mem_mb = 8192
  boot = installer        ; installer | disk
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
- **Storage presented to guest**: virtio-blk as primary (Linux/BSD have inbox
  drivers; Windows needs the virtio-win driver injected — see §6a). Also
  support emulated AHCI as a Windows-friendly fallback that needs no drivers
  at install time, at the cost of more emulation complexity. Recommend:
  *AHCI for Windows installer boot disk, virtio-blk for Linux/BSD*, both
  implemented in the device model, chosen via `os_hint`.
- **Network**: virtio-net, optional; not required for offline installs.
- **Video**: two phases, same underlying mechanism. Pre-OS-driver, the
  guest's own firmware renders through the GOP protocol we expose, into a
  linear framebuffer in that guest's RAM. Post-boot, we present a plain
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
- Explicitly **local-only for v1** — no serial or network exposure. This
  keeps the feature inside the existing console-ownership model instead of
  adding a network stack or serial protocol to the trusted hypervisor core.
  Revisit serial/remote access as a stretch goal only if a real headless-host
  use case shows up.

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
v1), and where the *install target* lives (a host-file-backed virtual disk,
or a real physical drive). Both targets are exposed to the guest through the
same block-device frontend (AHCI or virtio-blk, per `os_hint` as in §6/§6a) —
the installer never knows or cares which backend it's writing to.

- **Install media (ISO)**: read from the host ESP/local filesystem, exposed
  to the guest as a virtual optical drive (AHCI/ATAPI CD-ROM, or a
  virtio-scsi CD-ROM for Linux/BSD). Placed first in guest firmware boot
  order whenever `boot = installer`.
- **Virtual disk target (`target_disk = file:<path>`)**: a raw sparse file
  on host storage. If it doesn't exist yet, the hypervisor (or the `/tools`
  prep script, run ahead of time) creates it at `target_disk_size_gb`. Reads
  and writes from the guest are just file I/O against the host filesystem
  driver already needed to load ISOs and guest firmware/varstore.
- **Physical disk target (`target_disk = physical:<serial-or-guid>`)**: the
  guest's writes go straight to a real drive. This needs the hypervisor to
  own a minimal **host-side block driver** (AHCI + NVMe covers the vast
  majority of real hardware) so it can issue raw reads/writes after
  `ExitBootServices()`, since UEFI's Block I/O protocol is gone by then. The
  block backend abstraction (`struct blk_backend` with `file` and
  `physical` implementations behind one vtable) is what lets the same
  virtio-blk/AHCI frontend serve either case unmodified.
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

- **Host NIC driver**: a minimal host-side driver for one common real NIC
  chipset family (Intel e1000/e1000e-class is the pragmatic first target —
  broad hardware support, simple register interface, well-documented) so
  the hypervisor can drive the physical network adapter directly after
  `ExitBootServices()`, the same way §6d's AHCI/NVMe driver does for
  storage. Lives in its own module behind a backend abstraction, same
  isolation principle as `blk_backend`.
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
  named in that pairing stays fully isolated from both of them. No VLANs,
  no general-purpose virtual switch — kept intentionally minimal, matching
  "thin," while still supporting deliberate guest-to-guest use cases (e.g.
  a database VM and an app-server VM that are meant to talk to each other).
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

## 6g. Fault isolation between guests

A misbehaving guest must not be able to affect other guests or the
hypervisor itself:

- **Memory/CPU isolation is mostly inherent to the architecture already
  chosen**: each VM has its own EPT/NPT address space (§2/§4), so a guest
  cannot read or corrupt another guest's memory regardless of what it does
  to itself. With 1:1 vCPU-to-pCPU pinning (§3), a guest's vCPU spinning
  forever occupies only its own pinned core, not one shared with other
  guests' vCPUs — so a hung guest doesn't starve others of CPU time by
  construction, not as an added feature.
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
  the shutdown sequence.
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
- Same principle for total configured `vcpus` against physical core count,
  given the 1:1 pinning model (§3) — no VM should be admitted if it can't
  actually get pinned cores.
- **Explicit `cpu_set` validation**: for any VM specifying `cpu_set`, confirm
  every listed core actually exists on this host, that the count matches
  `vcpus`, and — critically — that no two VMs' `cpu_set` ranges overlap.
  Overlap is refused outright (not just warned about), since exclusive
  pinning is what the fault-isolation guarantee (§6g) relies on; VMs
  without an explicit `cpu_set` are auto-assigned only from cores no
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
   VM boundaries, vCPU pinning holds) and that reported stats match reality.
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
    guest must never affect others. Memory/CPU isolation falls out of the
    already-chosen EPT/NPT-per-guest + 1:1 vCPU pinning architecture; on top
    of that, a per-vCPU watchdog (§6g) detects genuinely faulted guests
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
    `hype.cfg` are checked against actual physical RAM and core count at
    hypervisor startup; any VM that would overcommit either is refused with
    a clear diagnostic rather than allowed to start and fail later.
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
    ever share a pinned core — remains mandatory and is enforced at startup
    admission control (§6i), since §6g's fault-isolation guarantee depends
    on it.
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

Ideas captured here are deliberately **not** on any `task.md` milestone —
recording the intent now so it isn't lost, without pulling it into v1's
scope or weakening any v1 hard invariant to make room for it. Nothing here
should be implemented without first promoting it to a real `task.md` epic
and, if it changes a v1 decision, updating §10 explicitly (per AGENTS.md's
own "keeping plan.md and task.md in sync" rule).

- **Real vCPU scheduler, replacing 1:1 exclusive pCPU pinning** (noted
  2026-07-14). v1's hard invariant (§3, §10, AGENTS.md) is one vCPU
  permanently and exclusively owning one pCPU — simple, and it's *why*
  §6g's fault-isolation guarantee holds "by construction" (a hung guest
  occupies only its own core, never one shared with another guest's
  vCPU). A v2 direction is to replace this with hype's own scheduler:
  multiple vCPUs (from the same or different guests) time-sliced across a
  smaller pCPU pool, with optional config-driven affinity (e.g. pin
  specific vCPUs/VMs to specific pCPUs when an operator wants that, but
  don't require it). This is a materially bigger architectural change
  than it sounds, because §6g's fault-isolation story would need a new
  mechanism once "hung vCPU occupies only its own pCPU" is no longer true
  by construction — likely some form of scheduling quantum/priority
  guarantee enforced by the scheduler itself, replacing what pinning gave
  for free. Any v2 work here must explicitly re-derive how fault
  isolation holds under real scheduling before it can replace the
  pinning invariant, not just drop the invariant and assume isolation
  still holds. The scheduler must also be **NUMA-node aware**: on
  multi-socket/multi-node hosts, place a VM's vCPUs and its guest RAM on
  the same NUMA node wherever possible (and keep them together across
  any rebalancing), rather than scheduling purely on core availability —
  cross-node memory access is a real, measurable performance cliff this
  project shouldn't reintroduce once it's no longer avoided for free by
  static 1:1 pinning to a fixed core.

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
  promotion to a real task.md epic, not just an API bolted onto existing
  per-VM management code.
