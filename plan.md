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
  <!-- Full extensible schema: docs/hype-cfg-spec.md (CONFIG-1 #220) -->
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
  §10 decision 3) file on host storage, created ahead of time by
  `tools/make-disk-image.sh` — *not* by the hypervisor. `target_disk_size_gb`
  is a **declaration of intent that hype validates**, not a creation
  instruction: hype compares it against the resolved image's real size and
  reports a mismatch, which catches a VM pointed at a stale or truncated
  image. The host filesystem controls whether the backing file can remain
  sparse (§10 decision 29). ext supports true holes. FAT32 and exFAT do not;
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
    The only real bounds are physical: 1:1 pinning gives at most
    (usable cores - 1) VMs (the BSP keeps console/log duty), and total
    guest RAM must fit. Decided:
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
      this breeds); runtime VM hotplug (out of scope — the set of VMs is
      fixed at boot from `hype.cfg`, only their power state changes, §6f).

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
  promotion to a real board milestone, not just an API bolted onto existing
  per-VM management code.

- **Dynamically-sizable (thin-provisioned) virtual disks, pooled across
  one or more physical disks, encrypted and isolated per VM** (noted
  2026-07-14). v1's storage model (M5's `blk_backend`, M10's physical-disk
  target) is a virtual disk mapped to one fixed-size `file:` backing file
  or one whole `physical:` device -- no thin provisioning, no pooling. v2
  would let a virtual block device grow on demand (allocated space backed
  by extents drawn from a shared pool spanning part or all of one or more
  physical disks, not a single pre-sized file), so multiple VMs' virtual
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
