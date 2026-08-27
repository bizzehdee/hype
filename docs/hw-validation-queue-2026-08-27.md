Queue of hardware validations owed by the 2026-08-27 bugfix sweep. Everything
below is fixed and QEMU-clean where QEMU can reach it; none of it can close
without a boot.

## AMD -- the 5950X desktop, one boot covers all of these

Build: default, no `EXTRA_CFLAGS`, commit `02da239`. Config:
`tools/hw-val-2026-08-25/hype1a.cfg`,
with `\hype\disks\run1a-scratch.img` staged first (#738) -- check the
`cfg: loaded` byte count to be sure the intended file is the one that loaded.
Leave the keyboard and mouse on the 2.0 hub, and leave the two SuperSpeed hubs
on controller 2 ports 7 and 8 populated.

| Ticket | What to read | Passes when |
|---|---|---|
| #734 | `DIAG: MOUSE reports=` | non-zero with `errors=0` -- passed on boot 3, must stay passing |
| #734 | `DIAG: HID reports=` | non-zero, with the keyboard still behind the hub -- the one still open |
| #734 | `CTXDUMP` | the keyboard's and mouse's `int-in` lines differ ONLY in `route`, `mps`, `esit` |
| #734 | `SET_PROTOCOL(boot) ... REFUSED` | absent |
| #737 | `Address Device slot N ... completion code 17` | absent for the device behind the hub on controller 1 |
| #739 | ports 7 and 8, and `host-usb: INVENTORY` | both hubs walked; no "bad hub-descriptor type 0x2a", no "GET hub-descriptor FAILED"; whatever is behind them is listed |
| #738 | `m5-8: target_disk` | resolves; `fw-1: disk front-end` is not "no SATA disk attached" |
| #735 | `ioio` in EXHIST, `UARTTX COM1 written` | both keep moving after `reboot`; a 0xCF9 appears in IOHIST. NOT expected to pass -- no fix is staged; reproduced twice |
| #732 | `cfg: autostart` + `#732: vm...` | already proven in QEMU; this is a no-regression read |

### Where #734 stands after three boots -- `RUN-CARD-2026-08-27.md` has the detail

The **mouse is fixed**. Boot 3 (10:42, `1919410`) read `MOUSE polls=200617 reports=548
errors=0 packets=548`. `SET_PROTOCOL(Boot)` plus a TRB sized at the endpoint's mps were
its cc=3 Babble.

The **keyboard is not**, and its cc=4 is now known to be permanent rather than
transient -- which is what the halt recovery was added to establish. Eight recoveries
ran, each completing cleanly (`Reset Endpoint cc=1`, `Set TR Dequeue cc=1`), and the
next transfer took cc=4 every time. The device answers EP0 control transfers through
the same hub and the same TT, and refuses its interrupt-IN endpoint.

The comparison that matters is inside one boot: the mouse works one hub port along from
the keyboard, same 2.0 hub, same full speed, same interval, same endpoint address.
Boot 4 adds `CTXDUMP` -- the OUTPUT slot and endpoint context the controller holds after
Configure Endpoint, for both devices and their TT hub. The two `int-in` lines should
differ only in `route`, `mps` and `esit`; any other differing field is the bug.

If they are identical, the next step is physical: move the Keychron `3434:0da4` off the
hub onto a root port. Note that the other keyboard already on a root port
(`ctrl1 port12 1462:7c91`) is class `03/00/00` -- **not** the boot subclass, so hype will
never claim it and unplugging the Keychron does not run that experiment by itself.

QEMU cannot reach this: `tools/734/run-734-qemu.sh`'s own CTXDUMP prints `ttslot=0`,
because its hub is full-speed and needs no Transaction Translator.

#735 reproduced on boots 2 and 3 and has no fix staged. Expect it again.

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
