Queue of hardware validations owed by the 2026-08-27 bugfix sweep. Everything
below is fixed and QEMU-clean where QEMU can reach it; none of it can close
without a boot.

## AMD -- the 5950X desktop, one boot covers all of these

Build: default, no `EXTRA_CFLAGS`, commit `1919410`. Config:
`tools/hw-val-2026-08-25/hype1a.cfg`,
with `\hype\disks\run1a-scratch.img` staged first (#738) -- check the
`cfg: loaded` byte count to be sure the intended file is the one that loaded.
Leave the keyboard and mouse on the 2.0 hub, and leave the two SuperSpeed hubs
on controller 2 ports 7 and 8 populated.

| Ticket | What to read | Passes when |
|---|---|---|
| #734 | `DIAG: HID reports=` and `DIAG: MOUSE reports=` | non-zero, with the keyboard still behind the hub |
| #734 | `host-xhci: interrupt-IN transfer FAILED ... cc=` | absent, or a single cc=4 followed by `reports=` climbing (a transient the halt recovery now absorbs) |
| #734 | `gave up after 8 halt recoveries`, `SET_PROTOCOL(boot) ... REFUSED` | both absent |
| #737 | `Address Device slot N ... completion code 17` | absent for the device behind the hub on controller 1 |
| #739 | ports 7 and 8, and `host-usb: INVENTORY` | both hubs walked; no "bad hub-descriptor type 0x2a", no "GET hub-descriptor FAILED"; whatever is behind them is listed |
| #738 | `m5-8: target_disk` | resolves; `fw-1: disk front-end` is not "no SATA disk attached" |
| #735 | `ioio` in EXHIST, `UARTTX COM1 written` | both keep moving after `reboot`; a 0xCF9 appears in IOHIST. NOT expected to pass -- no fix is staged; reproduced twice |
| #732 | `cfg: autostart` + `#732: vm...` | already proven in QEMU; this is a no-regression read |

### Where #734 stands after two boots -- read `RUN-CARD-2026-08-27.md` for the detail

Boot 1 (09:14, `f61f43e`) read `HID polls=20745 reports=0 errors=1` and `MOUSE
polls=238463 reports=0`. Two claimed HIDs shared ONE interrupt-IN ring and ONE
"transfer outstanding" flag per controller, so an idle input device held that
flag forever. Fixed in `3cb97e2`.

Boot 2 (09:46, `3cb97e2`) still read `reports=0` on both -- and the diagnostic
added alongside that fix named why:

```
host-xhci: interrupt-IN transfer FAILED slot=3 ep=3 ... cc=4 [#734]   keyboard
host-xhci: interrupt-IN transfer FAILED slot=4 ep=3 ... cc=3 [#734]   mouse
```

cc=4 is a USB Transaction Error, cc=3 a Babble Detected, and both HALT the
endpoint. hype cleared only its own "armed" flag and rang the doorbell again,
which a halted endpoint ignores -- so `errors` froze at 1 while `polls` climbed
past 300000. Fixed in `1919410`: halt recovery (bounded), `SET_PROTOCOL(Boot)`
at claim time, and a TRB armed at the endpoint's wMaxPacketSize rather than the
caller's 8-byte buffer.

Boot 3's #734 read is therefore about the KEYBOARD's cc=4, which none of those
three changes explains. One cc=4 followed by a rising `reports=` is a transient
now recovered. cc=4 repeating, or `gave up after 8 halt recoveries`, is a real
bus fault -- and the discriminator for that is to move the keyboard to a root
port. The 09:46 inventory already lists a second keyboard on one
(`[3] ctrl1 port12 1462:7c91`, owner=free), so unplugging the hub-side keyboard
runs that experiment with no re-cabling.

#735 reproduced cleanly on boot 2 and has no fix staged: the guest pinned its
reboot to vCPU 1, ran `reboot`, and then `ioio` froze at 59680, `UARTTX COM1`
at 5225, no `0xCF9` was ever written, and every vCPU stayed alive HLTing. Expect
it again on boot 3.

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
is design work, not a run. #733 needs the laptop. #740 has no fix yet. #735 has
no fix yet either -- it stays in the boot-1 table only as a re-observation.
