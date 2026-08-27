Queue of hardware validations owed by the 2026-08-27 bugfix sweep. Everything
below is fixed and QEMU-clean where QEMU can reach it; none of it can close
without a boot.

## AMD -- the 5950X desktop, one boot covers all of these

Build: default, no `EXTRA_CFLAGS`. Config: `tools/hw-val-2026-08-25/hype1a.cfg`,
with `\hype\disks\run1a-scratch.img` staged first (#738) -- check the
`cfg: loaded` byte count to be sure the intended file is the one that loaded.
Leave the keyboard and mouse on the 2.0 hub, and leave the two SuperSpeed hubs
on controller 2 ports 7 and 8 populated.

| Ticket | What to read | Passes when |
|---|---|---|
| #734 | `DIAG: HID reports=` and `DIAG: MOUSE reports=` | non-zero, with the keyboard still behind the hub |
| #734 | `host-xhci: interrupt-IN transfer FAILED ... cc=` | absent; if present, the completion code names the remaining fault |
| #737 | `Address Device slot N ... completion code 17` | absent for the device behind the hub on controller 1 |
| #739 | ports 7 and 8, and `host-usb: INVENTORY` | both hubs walked; no "bad hub-descriptor type 0x2a", no "GET hub-descriptor FAILED"; whatever is behind them is listed |
| #738 | `m5-8: target_disk` | resolves; `fw-1: disk front-end` is not "no SATA disk attached" |
| #735 | `ioio` in EXHIST, `UARTTX COM1 written` | both keep moving after `reboot`; a 0xCF9 appears in IOHIST |
| #732 | `cfg: autostart` + `#732: vm...` | already proven in QEMU; this is a no-regression read |

### What the 2026-08-27 boot 1 said about #734, and what changed since

The boot read `HID polls=20745 reports=0 errors=1` and `MOUSE polls=238463
reports=0`. Two separate things:

1. Both claimed HIDs shared ONE interrupt-IN ring and ONE "transfer
   outstanding" flag per controller. An idle input device holds that flag
   forever, so the other endpoint's doorbell was never rung again. Fixed: each
   endpoint now owns its own block (`HYPE_XHCI_INT_IN_MAX`), and
   `tools/734/run-734-qemu.sh` reproduces the lock-out in QEMU -- the keyboard
   froze at 67 reports across 100 keystrokes once the mouse went still, and
   rises 120 -> 320 with the fix.
2. `errors=1` says one keyboard transfer completed with a failure code, and the
   boot could not name it. It stayed at 1 because of (1): the endpoint never
   re-armed. With (1) fixed the endpoint retries every poll, so this next boot
   says whether that error is transient or permanent -- read the new
   `interrupt-IN transfer FAILED ... cc=` line for the code.

A one-boot discriminator worth running for #734 if it still fails: move the
keyboard to a root port. Reports there and not behind the hub confirms the
TT/periodic-schedule path as the fault.

Separately, on the laptop, not the desktop: #733's 1-vCPU run with
`hype1a-1cpu.cfg` actually copied over `\hype.cfg` this time.

## AMD -- a second boot, AVIC build

Build: `make clean` then `EXTRA_CFLAGS=-DHYPE_ENABLE_AVIC=1`; gate on the banner
sha AND the echoed flag, since `make` does not rebuild on a flag change alone.
2 vCPUs.

| Ticket | What to read | Passes when |
|---|---|---|
| #640 | the `[#193]` AVIC banner | prints on a NORMAL boot, not only after a restart |
| #640 | `AVICSTAT vm0: ... undecodable=` | 0 |
| #640 | per-vector IPI counts vs a flag-off run of the same config | they match |

## Intel -- the i5-13420H / the nested-VMX box

| Ticket | What to read | Passes when |
|---|---|---|
| #729 | `micro/vmexit` MSR round-trip | reads back 0x123456000, and `0 probe(s) failed out of the non-fatal set` |
| #708 | the same 2-physical-vCPU Alpine boot on the DEFAULT build | tells us whether the hang is APICv's or general VMX; either answer unblocks it |
| #698 | the AP's arming state before and after the M8-4 restart | see the comment on the ticket |

## Not queued, and why

#640 criterion 5 (one owner of guest APIC state, written as a plan.md decision)
is design work, not a run. #733 needs the laptop. #740 has no fix yet.
