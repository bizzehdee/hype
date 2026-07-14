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
  validation throughout, but has **never been run on real AMD
  hardware**.
- The **Intel/VMX path** is not functional past CPU detection yet — if
  your CPU is Intel, the virtualization test guests will print "not
  implemented yet" and skip themselves; everything else (console,
  memory map, timer) should still run normally.
- Unsigned — Secure Boot must be **disabled** in firmware setup, or
  this won't load at all.

If you hit a hang, that itself is useful information for this
project — see "If it hangs" below.

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
then (once it gets far enough) a black screen with one line of white
text: `hype: Boot Services exited, hypervisor now running`. It then
runs for about a second and sits there permanently (this is expected —
there's nothing after that yet, it's meant to just prove the whole
stack came up cleanly).

## Capturing debug output (recommended if at all possible)

Most of the useful diagnostic detail is only printed over the serial
port (COM1, `0x3F8`, 115200 baud, 8N1), not to the screen — this
project logs a line before and after every real-hardware-sensitive
step specifically so a hang can be pinned to an exact point.

- If your machine has a physical serial (RS-232/DE-9) port, or you have
  a USB-to-serial adapter, connect it to another machine and capture
  with a terminal program (`screen /dev/ttyUSBx 115200`, `minicom`,
  PuTTY, etc.) **before** you boot the USB drive, so you don't miss the
  early lines.
- If you can't capture serial, screen output alone still tells us
  whether it got as far as "Boot Services exited" or not.

## If it hangs

The single most useful thing to report back is: **the last line you
saw** (serial or screen) before it stopped responding. The log is
structured so consecutive lines bracket every risky operation — e.g.
`cpu: vendor=... vmx=... svm=...` then `vmm: ... detected` then
`vmm: about to enable ...` then `vmm: ... enabled`, and similarly around
`ExitBootServices`, the GDT/paging/IDT loads, and enabling interrupts.
Whichever "about to ..." line printed last without its matching
"done"/next line following is exactly where it stopped.
