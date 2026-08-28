Hardware validations owed as of 2026-08-28. Everything below is built and QEMU-clean
where QEMU can reach it; none of it can close without a boot.

`RUN-CARD-2026-08-28.md`, staged on the drive as `\RUN-CARD.md`, is the operative
document for the next boot. This file is the queue behind it.

## Boot 8 -- AMD 5950X desktop. One boot covers the whole hot-plug set plus #750

Build: default, no `EXTRA_CFLAGS`. Config `hype1a.cfg` (`vcpus = 2`, so 4 logical CPUs).
Keyboard and mouse start on the 2.0 hub. The run card carries the cable sequence and the
timings; do not improvise them, because a port that changes twice inside one hub poll
interval is one event and tests nothing.

| Ticket | What to read | Passes when |
|---|---|---|
| #744 | `host-hid: ... DEPARTED -- releasing` after step C | present, **and** the mouse still reports afterwards -- teardown must release only the departing device's slot |
| #744 | the same after step E | present for a **root port**, which is a separate path from the hub one |
| #745 | `host-usb: ... ARRIVED` after steps B, D, F | present, and the chord works at D -- claimed *and* wired into the input path |
| #746 | `DIAG: HUBPOLL hubs=1 polls=... reports=` | **reports non-zero.** QEMU has never produced this; it does not model the hub's status-change endpoint, which is the entire reason #746 needs hardware |
| #750 | `soft lockup` in `\RUN1A.LOG` | **absent**, across ~20 idle minutes on a 4-logical-CPU guest. First hardware exposure of the STI-shadow fix; 0 of 11 in QEMU against a 1-in-3 prior |
| #750 | `HB-` ticks | every 10 s with no gap |
| #735 #749 | `APVCPU vm0/N: ... unhandled=` | **0**, with `unclaimed=` carrying the count instead. This was 37,009,095 on boot 6 |
| #742 | `DIAG: HID[i/n] ... reports=` | every claimed keyboard reporting, merged into one stream |
| #741 | `host-usb: INVENTORY` | the composite dongle lists every interface, not only its first |
| #738 | `m5-8: target_disk` | resolves; `fw-1: disk front-end` is not "no SATA disk attached" |

## Not in boot 8

| Ticket | Why not, and what it needs |
|---|---|
| #754 | #747 itself is **closed** -- every clause of its definition of done is tested and passing in QEMU. What is left is the USB-SATA bridge's own behaviour under a physical yank, which `device_del` cannot model, and that is #754. Needs a **second, sanctioned scratch USB device**: `HYPEBOOT` cannot be it, since pulling the log medium destroys the run's own evidence |
| #743 | The recycled-slot fault does not reproduce in QEMU and hype's own bookkeeping is correct, so it is controller behaviour. Needs its own instrumented boot, not a shared one |
| #748 | A VM restart zeroing guest RAM under running APs. Fixed and QEMU-proven; needs a 3+ vCPU guest to have a victim at all, so it wants a config this boot does not use |
| #640 | The AVIC comparison boot. `hype-avic.efi` is staged alongside the default and unchanged; it is a separate boot because the point is the difference between the two |
| #753 | `int_defer_tsc[]` is keyed by vector. A diagnostic defect -- no hardware involved |

## Reading the logs

`\HYPE.LOG` contains invalid UTF-8. `LC_ALL=C grep -a`, or it matches nothing.

Archive both logs into `tools/hw-val-2026-08-25/logs/boot-8/` before re-staging. `stage.sh`
refuses to run while they are non-empty, deliberately: losing a boot's evidence to a
re-stage is worse than an extra manual step.
