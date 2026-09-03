# Boot AMD-1 (5950X) -- the input run with the SMP legs folded in (#426 #775 #790 #603 #641 #788)

Build: default, no `EXTRA_CFLAGS`, tree at or after da3e93d (rate-bounded xHCI reset #792, PS/2
gate #796, reset-path fixes #797/#798). Active config `\hype.cfg` = `hype1g.cfg`: the boot-42
guest (`run1a`, 2 vCPUs, reboot pinned to CPU 1 by `\input\vm0.txt` = `input-1a/vm0.txt`) plus
hype1b.cfg's three #603 microtests. Seven physical cores including the BSP; the 5950X has 16.

## Short form (20 minutes) -- what this staging is for

Twenty minutes gives everything except the 90-minute input half. Steps 1 and 2 below take about
five minutes and close **#603** (the SVM leg of the coverage suite; `hello` PASS after `vmexit`'s
triple fault and `vmexitstorm`'s force-off) and put the pinned restart on record for **#525/#698**
on SVM at the current tree. If the Pico and a second keyboard are to hand, do step 3 as well and
leave the rest of the twenty minutes running: the first #792 stall came at 537 s in boot 42, so a
single `XHCIRESET ... reset #1` inside the window is possible but not owed, and `KBDCHARS` gives a
#788 sample. Without them, power off after the second login and the microtests. #426, #641 and
the rate bound keep their 90-minute run.

## Before you boot

- Banner sha matches the staged build.
- `XHCIOWN: log sink on ctrl[1], boot medium on ctrl[1]` -- the keyboards are on ctrl[2].
- Admission grants all four VMs.

## The sequence

1. Boot, stay on the dashboard. The microtests finish themselves in the first minute (`hello`
   PASS, `vmexit` triple-faults on purpose, `vmexitstorm` is force-powered-off by its watchdog).
2. The script logs in, pins the reboot to CPU 1, reboots, logs in again and prints
   `reboot-pin-nonbsp`. **Do not type until it has.**
3. Then arm the Pico: **BOOTSEL once**, confirm `a0001` in the guest. Keep the Logitech and the
   Keychron attached; leave both spare USB drives plugged in (#780's condition).
4. Leave it 90 minutes from the second login. Type on the Logitech now and then.
5. Power off. Bring back `HYPE.LOG` and `RUN1A.LOG`.

## What to read

| Ticket | Read | Passes when |
| --- | --- | --- |
| #603 (SVM leg) | `micro/hello` PASS after `vmexit`'s triple fault and `vmexitstorm`'s force-off; no `PROBE FAIL` | the coverage suite's second leg; closes #603 |
| #426 | 90 minutes with input live at the end | no `left dead`, no `REFUSED`, keyboards typing at power-off |
| #775 | `CTRLSILENCE` / `XHCIRESET` | every stall followed by `reset #N done ... keyboards=3`; close as answered by #781-#785 |
| #790 | `cmdring timeouts=` equals the `CTRLSILENCE` count; 0 `REVIVE` | the 90-minute form the ticket asked for |
| #792 | `XHCIRESET ctrl[2]: reset #N begins (M in the last 10 min, previous S s ago)` | a fourth reset is allowed when the stalls are minutes apart |
| #525 #698 | the restart chain and `TMRLATE vm0/1` after it | SVM hardware record on the current tree |
| #641 | `APVCPU vm0/N: exits=`, `PERF: hlt_wait=` | recorded |
| #788 | doubled characters per 1,000 in `KBDCHARS` | recorded (boot 42: 2.7) |

## Result -- run 1, 2026-09-03, 40 minutes (logs in `logs/bootAMD1-1/`)

Build `70d6b0f-dirty` (the dirty files are `tools/` configs). `XHCIOWN: log sink on ctrl[1], boot
medium on ctrl[1]`. Admission granted all four VMs: vm0 and vm1 two whole cores each, vm2 and vm3
one each, 6 cores of the 16. Powered off at t=2411 s. No `PANIC`, no `PROBE FAIL`.

| Ticket | Evidence | Outcome |
| --- | --- | --- |
| #603 (SVM leg) | `MICRO PASS: hello` on ttyS0 and screen; `micro/vmexit: 0 probe(s) failed out of the non-fatal set` then `MICRO FAIL: vmexit: deliberately triple-faulting` and `vm1 'micro/vmexit' STOPPED -- ... triple fault`; `WATCHDOG vm2: faulted: unhandled-exit storm at one RIP (reason=0x400 rip=0x100055f, 4096 repeats) -- forcing THIS vm off`; vm0 and vm3 ran on | **PASS -- closes #603** (Intel-A carried the VMX leg) |
| #525 #698 (SVM record) | `vm0 vCPU 1 guest reset via ACPI reset register (0xCF9) -> restart`, `vm0 restarted (M8-4)`, second login, `SCRIPT vm0: PASS pass (21 directive(s))`, `at line 69: reboot-pin-nonbsp`. `TMRLATE vm0/1` after the restart: deliveries 7452 -> 48395 by power-off, so the AP timer kept firing. `worst_late` peaks: vm0/1 4.81 s, vm0/2 5.00 s, vm0/3 4.92 s (boot 42: 2.24 s), all set during the reboot itself | recorded; the restart chain works on SVM at the current tree |
| #775 #790 #792 | `CTRLSILENCE` 0, `XHCIRESET` 0, `REVIVE` 0, `cmdring timeouts=0` on all three HID ticks, `left dead` 0, `REFUSED` 0 | no stall occurred in 40 minutes (boot 42's first came at 537 s); the reset path was not exercised |
| #426 | input live to the end: `KBDCHARS` 12,055 characters handed to the guest, first at t=76 s, last at t=2399 s, 12 s before power-off; Pico counter `a0001`..`a0180`, 180 sequences; the Pico's own self hot-plug fired 6 times at 388-392 s spacing, re-enumerated each time as slot6 | 40 of the 90 minutes; keeps its 90-minute run |
| #641 | `APVCPU vm0/1: exits=1507101394` (1.5 G in 39 min), `PERF: elapsed=2364866ms hlt_wait=2177362ms (92%)`, `hlt_if1=231143 hlt_if0=2` | recorded |
| #788 | 29 pure doubles in 11,155 Pico alphabet characters = **2.6 per 1,000** (boot 42: 2.7). Examples: `tuvwwxyz`, `01234456789`, `abbcdefg`, `abcddefg`. The 30 runs of 51-70 `h` are the Pico's own sequence (boot 42 shows the same) | recorded; unchanged |
| #799 | `HOUSECOST vm0: s2=27431ms ... s75=2359746ms` of 2411 s; `LOOPPHASE: diag=90442ms persist=86ms house=36587ms dispatch=8621ms` | the 5950X spends 15 us per exit in housekeeping, not the laptop's 235 us |

`RUN1A.LOG` ends at the second login (uptime 28 s). The guest wrote nothing more to ttyS0 (`UARTTX:
COM1 written=5117` constant from then on); boot 42's `HB-` lines came from a heartbeat loop typed
by hand, which this run did not have. Typed characters are never captured by the screen scrape
in either run, so `KBDCHARS` is the input-liveness record.
