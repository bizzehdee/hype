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
3. Then arm the Pico: **BOOTSEL once**, confirm `a0001` in the guest. Keep the Logitech mouse receiver and the
   Keychron attached; leave both spare USB drives plugged in (#780's condition).
4. Leave it 90 minutes from the second login. Type on the Keychron now and then. The Logitech (046d:c547) is a mouse; its receiver exposes a keyboard HID interface, which is why hype counts `keyboards=3`. The two real keyboards are the Keychron (3434:0da4) and the Pico (cafe:4b44).
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

## Result -- run 2, 2026-09-03, 119 minutes (logs in `logs/bootAMD1-2/`)

The 90-minute form. Build `447cbb0-dirty` (the dirty files are `tools/` configs); the default
build, not the APICv variant -- no `APICV` marker anywhere in the log. `XHCIOWN: log sink on
ctrl[1], boot medium on ctrl[1]`. Admission granted all four VMs: vm0 and vm1 two whole cores
each, vm2 and vm3 one each, 6 of the 16. Both spare USB drives present on ctrl[2] (`0781:5591`)
and ctrl[1] (`152d:1561`, `0781:5567`), so #780's condition held. Ran to t=7166 s. No `PANIC`,
no `PROBE FAIL`, no `left dead`, no `REFUSED`.

Times in this section are seconds of run time, interpolated from the 7,057 `FBSPEED: t=<ms>`
lines against the byte stamps (log stamps are byte offsets, not time).

| Ticket | Evidence | Outcome |
| --- | --- | --- |
| #603 (SVM leg) | `MICRO PASS: hello` on ttyS0 and screen -- in `HELLO.LOG`, not the combined log, because the per-VM sink took it. `vm1 'micro/vmexit' STOPPED -- ... triple fault`; `WATCHDOG vm2: ... unhandled-exit storm at one RIP (reason=0x400 rip=0x100055f, 4096 repeats) -- forcing THIS vm off`; vm0 and vm3 ran on | second SVM record, agrees with run 1 |
| #775 #790 #792 | **the reset path was exercised 5 times.** `CTRLSILENCE ctrl[2]` at t=787, 2720, 3290, 5393, 6004 s, each `3` or `2 keyboard(s) silent for 30s` with `USBSTS=0x00000010 HCH=0 HSE=0 HCE=0`, `CRCR.CRR=1`, `PORTSC[4]=0x00000e03 CCS=1 PED=1`, `No-Op FAILED cc=0 in ~6041400us`. Each was followed by `XHCIRESET ctrl[2]: reset #N done in 2008-2104 ms ... back: ports=6 devices=8 keyboards=3 mouse=1`. `cmdring timeouts=5` on all three HID ticks = the 5 `CTRLSILENCE`. `REVIVE` 0, `revive_fail` 0, `ringfull` 0, `evict` 0, `lost` 0, `skipped` 0 | **PASS** -- 5 for 5 recovered, full inventory back each time |
| #792 (rate bound) | reset spacing 1933 s, 570 s, 2103 s, 611 s; each `begins` line reported `0` or `1 in the last 10 min`, so the bound never withheld a needed reset | **PASS** -- the bound is not blocking recovery |
| #426 | 119 minutes with input live at the end. `KBDCHARS` 35,249 characters handed to the guest; first at t=85 s, last at t=7157 s, 9 s before the last log line. Pico counter `a0001`..`a0528`. In the whole run the character stream has **exactly 5 gaps over 20 s**: 60 s, 55 s, 50 s, 48 s, 52 s -- one per stall, each ending on the reset. No other gap exceeds 20 s in 7,072 s | **PASS -- the 90-minute form** |
| #788 | the stream was rebuilt from the overlapping 256-character `KBDCHARS` windows: 35,249 characters, none lost to the reconstruction. Of the 704 clean Pico alphabet sequences (25,399 characters): 642 exact, 61 with one doubled character, 1 with dropped characters. **61 doubles / 25,399 = 2.40 per 1,000** (run 1: 2.6; boot 42: 2.7). Examples `abcdefghijkllmno`, `...mnopqqrstu`, `abccdefg`, `...012334567` | recorded; unchanged |
| #788 (drops) | the one non-double token is `abcdefghijklmnovwxyz0123456789` -- `pqrstu`, 6 consecutive characters, gone. It sits at stream offset 26343, and the `KBDCHARS` record before it is the last one at t=5354 s, the final keystroke before the t=5393 s stall. So the only character loss in 25,399 characters is the stall boundary itself, not a steady drop rate | explained; no unexplained loss |
| #525 #698 (SVM record) | `vm0 vCPU 1 guest reset via ACPI reset register (0xCF9) -> restart`, `vm0 restarted (M8-4)`, `SCRIPT vm0: PASS pass (21 directive(s), 80622ms)`, `at line 69: reboot-pin-nonbsp`. After the restart the AP timers kept firing for the whole 119 minutes: `TMRLATE vm0/1` deliveries 4242 -> 128110, vm0/2 3782 -> 223576, vm0/3 5199 -> 121988. `worst_late` peaks vm0/1 4.82 s, vm0/2 4.90 s, vm0/3 4.96 s, all already set at the first sample after the restart, so all three were set during the reboot itself and never moved again | second SVM record on the current tree |
| #641 | `APVCPU vm0/1: exits=4476345568` (4.5 G in 119 min, 630 k/s), `unhandled=0 unclaimed=3`, `timer_irqs=128110`, `lockmax=1052146864 lockmaxsec=2`. `PERF: elapsed=7099117ms hlt_wait=6605850ms (93%) hlt_if1=686656 hlt_if0=2 preempt_if1=647 preempt_if0=346` | recorded |
| #708 | the non-APICv control datum: `HLTSHADOW: bsp hlt exits with STI blocking=731843 of those with RVI pending=0 | rvi wakes=0`. On the plain SVM build the BSP takes 731 k HLT exits under STI blocking and not one has RVI set, which is what the probe should say when APICv is not live. The APICv leg is still owed and belongs to Intel-B | control leg recorded |
| #799 | `LOOPPHASE: diag=227667ms persist=137ms house=92773ms dispatch=20203ms` of 7166 s = diag 3.2%, house 1.3%, dispatch 0.28%. `DRAIN: iters=1574688`, so **59 us of housekeeping per BSP loop iteration** (run 1 recomputed on the same denominator: 36587 ms / 1,029,075 iters = 36 us). `HOUSECOST vm0: s75=7044813ms` of 7166 s -- 98% sits in section 75, as in run 1 (97.9%) | recorded; the per-iteration cost rose 36 -> 59 us between a 40-minute and a 119-minute run on the same box, which is a datum, not yet a cause |

`vm3 'micro/hello' STOPPED -- its guest never reached a stable idle within the exit budget` at
t=787 s, after `BSPPROBE vm3: exits=200000001 last=0x78@0x1000ad0`. This is not new and not a
regression: run 1 printed the same line at the same 200,000,001 exits, and in both runs `hello`
had already reported PASS long before. `hello` idles on `HLT`, hype counts every HLT exit, and
the 200 M budget is reached in about 13 minutes.

`RUN1A.LOG` again ends at the second login; as in run 1 the guest wrote nothing more to ttyS0,
so `KBDCHARS` is the input-liveness record.
