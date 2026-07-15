# hype real-hardware test package

This is a build of `hype.efi`, a UEFI type-1 hypervisor, for testing on
real hardware rather than QEMU. **Read this whole file before booting
it** — this project's own real-hardware validation gate has not been
run against your specific machine yet.

## What this actually does to your machine

- It does **not** touch any hard drive, partition, or file on your
  system. At this stage of development there is no disk driver at all
  (that's a later milestone) — nothing here can read, write, or format
  anything on your PC's own storage.
- It **does** take over the whole machine once it starts: it disables
  interrupts, replaces the firmware's own GDT/IDT/page tables, calls
  `ExitBootServices()` (the point of no return back to firmware), and
  — if your CPU has AMD-V (SVM) or Intel VT-x (VMX) — attempts to set
  up virtualization extensions and launch a handful of tiny built-in
  test guests.
- **Worst case if something goes wrong: the machine hangs or reboots.**
  Recovery is a hard power cycle. No data is at risk.

## Current validation status (be aware before you boot this)

- The core UEFI bring-up (M0/M1: console, memory map, `ExitBootServices`,
  own GDT/IDT/paging, timer) has only ever been validated in QEMU. This
  is the first time it's running on real firmware/real silicon.
- The **AMD/SVM path** has real QEMU+KVM nested-virtualization
  validation throughout (a long list of built-in test guests: CPUID/MSR
  interception, guest-interrupt injection, PS/2 keyboard/mouse, a
  virtual display adapter, PCI enumeration, virtio-blk and AHCI disk
  I/O, and more), but has **never been run on real AMD hardware**.
- The **Intel/VMX path** is not functional past CPU detection yet — if
  your CPU is Intel, the virtualization test guests will print "not
  implemented yet" and skip themselves; everything else (console,
  memory map, timer) should still run normally.
- The very last test this build runs (**FW-1**) is a deliberately
  **known-failing** attempt to boot this project's own real, vendored
  OVMF firmware as a nested guest — it currently hits a NULL-pointer
  fault partway through and is expected to panic. This is intentional
  and already understood at the QEMU level; what real hardware adds is
  whether the *same* fault reproduces identically, differently, or not
  at all (see "What you should see" below) — that's the whole reason
  to run this build.
- Unsigned — Secure Boot must be **disabled** in firmware setup, or
  this won't load at all.

If you hit a hang or an on-screen panic, that itself is useful
information for this project — see "If it hangs" below.

## Writing this to a USB drive

You have two options (pick one):

**Option A — copy files onto a USB drive you format yourself:**
1. Format a spare USB drive as FAT32 (any OS's own disk utility can do
   this — this will erase that drive, so make sure it's the right one
   and has nothing on it you want to keep).
2. Copy this folder's contents (the `EFI` folder) to the root of that
   drive, so you end up with `EFI\BOOT\BOOTX64.EFI` on the drive.

**Option B — flash the pre-built raw image (`hype-usb.img`, next to
this README's own folder) directly:**
- Linux/macOS: `sudo dd if=hype-usb.img of=/dev/sdX bs=4M status=progress`
  — **triple-check `/dev/sdX` is your USB drive and not your main
  disk** before running this; `dd` will silently overwrite whatever
  device you point it at.
- Windows: use [Rufus](https://rufus.ie) or
  [balenaEtcher](https://etcher.balena.io) and point it at
  `hype-usb.img`.

## Booting it

1. Plug the USB drive into the target machine.
2. Enter your firmware's boot menu (commonly F12/F11/Esc/F2 at power-on
   — varies by manufacturer) and pick the USB drive, or set it first in
   boot order.
3. Make sure **Secure Boot is disabled** in firmware setup (this build
   is unsigned).
4. Boot it.

## What you should see

If a display is connected, you'll briefly see UEFI's own boot text,
then (once it gets far enough) a black screen that starts filling with
white status text as each built-in test guest runs in turn — the
**same rich diagnostic detail this project's QEMU testing already
relies on is mirrored to the screen**, not just the serial port (the
screen console is set up before any test guest runs, specifically so a
machine with no serial port/cable still gives back full detail). The
text scrolls as it goes, so whatever's on screen when it stops is the
most recent output, not something buried above it.

Expect it to run through a long sequence of `<name>: ...` lines (one
self-contained test per built-in device/feature) and finally land on a
line starting `PANIC: fw-1: exc vec=... err=0x... cr0=0x... cr2=0x...
cr3=0x... rip=0x...` — **this is expected**, not a sign anything you
did was wrong (see "FW-1" above). The machine will sit there
permanently after that panic; a hard power cycle is the only way out.

## Capturing debug output (still recommended if you can)

Everything printed to the screen is *also* printed over the serial
port (COM1, `0x3F8`, 115200 baud, 8N1) — serial capture is still worth
doing if you have the means (a physical RS-232/DE-9 port or a
USB-to-serial adapter, captured on another machine with `screen
/dev/ttyUSBx 115200`, minicom, PuTTY, etc., started **before** you
boot the USB drive), mainly because a terminal scrollback is easier to
copy-paste from than a photo of a screen. But if you can't do that,
**a phone photo of the final on-screen panic message is enough** —
that's the actual, complete diagnostic payload this project needs back
from you.

## If it hangs (no panic message, just stops)

The single most useful thing to report back is: **the last line you
saw** (serial or screen) before it stopped responding. The log is
structured so consecutive lines bracket every risky operation — e.g.
`cpu: vendor=... vmx=... svm=...` then `vmm: ... detected` then
`vmm: about to enable ...` then `vmm: ... enabled`, and similarly around
`ExitBootServices`, the GDT/paging/IDT loads, and enabling interrupts,
and again around each individual test guest. Whichever "about to ..."
line (or `<name>: ...` test-start line) printed last without its
matching "done"/next line following is exactly where it stopped.

## If you see the FW-1 panic (the expected outcome)

Please report back the **entire** `PANIC: fw-1: ...` line verbatim
(vector/err/cr0/cr2/cr3/rip), plus whatever raw-instruction-byte and
stack-dump lines printed just above it. If this exact fault (same
vector, same `cr2`) shows up on real hardware too, that confirms it's
a genuine firmware bug reachable independent of nested-virtualization
quirks; if it's *different* (or doesn't happen at all), that's equally
valuable — it means the earlier QEMU-only fault was an artifact of
running under nested SVM, not a real bug in this project's own guest
setup.
