Addendum to `hw-validation-queue-2026-08-30.md`, 2026-09-02.

## Boot 40 (AMD) goes first -- the input set, with the wedge trigger removed

Analysis of boots 8-39 showed every command-ring wedge was a `Stop Endpoint` from the #775
silence revive (0 wedges in 18 boots without it, 10 in 13 with it). #790 turns that revive off
by default; #788 fixes the one-report-per-tick drain. Both are QEMU-clean (tools/767 ALL PASS,
`make test` rc=0).

Run card: `tools/hw-val-2026-08-25/RUN-CARD-2026-09-02-boot40-amd-input.md`, already set as
`RUN_CARD` in `stage.sh`. Tickets it can settle: #790, #788, #775, #787, plus the standing
#426 gate. Intel boots 40 and 41 from the 2026-08-30 queue are unchanged and follow it.

If boot 40 runs 90 minutes with `cmdring timeouts=0`, the #781 reset series (#782-#786) drops
from "needed for input to survive a run" to "defence in depth" and can be re-prioritised.

## Boot 40 result (AMD 5950X, 2026-09-02)

Build `5b423ed-dirty` (dirty = uncommitted `tools/` cfg edits only), `silence_revive=0` on every
`HIDTICK`. Run 54 min, stopped by the operator for machine time. Logs archived under
`tools/hw-val-2026-08-25/logs/boot-40/` (gitignored). An earlier 230 s boot the same day was
power-cycled because the Pico was on the wrong hub; it is `HYPE.1.LOG` there and is not evidence.

| Ticket | Result |
| --- | --- |
| **#790** | Gate met: `cmdring timeouts=0`, 0 `REVIVE`, 0 `DEADMAN` for 54 min. Boots 34-39 wedged by 1180 s. No xHCI command was issued after 2341 s, so the ring was not exercised after the input died |
| **#775 #781** | Input still died, at 2450 s: the Pico, the Logitech c547 (steady ~2 reports/s all run) and the hub status polls (`HUBPOLL reports=`) all froze within 2444-2473 s. No port event, no transfer error, no command. Boot 39 showed the same at 291-301 s before its revive wedged the ring. The root fault is ctrl[2] ceasing to deliver interrupt-IN events for every endpoint at once; the revive only made it a command-ring wedge. #781 stays justified |
| **#787** | The Pico left the bus 6 times at a 376 s period (461, 839, 1215, 1589, 1963, 2340 s), each preceded by 3 `cc=4` transaction errors; hype re-enumerated it within 1-6 s each time. The 2450 s death was 109 s after a re-arrival with no port event: a separate fault |
| **#788** | Not fixed: 4 doubled characters in 11,542 (245 alphabet passes, 0 missing), at both the 8 ms and 30 ms speeds. Boot 38 was 1 in 1,395 |
| **#426** | Not met: 54 min, input dead from 41 min |
| new | Typematic rate is lost on re-claim: guest set 250/33 ms at 71 s; held-key runs were 67-70 repeats before the first Pico drop and 23-28 after. Re-claim calls `hype_usb_hid_typematic_init` (defaults 500/92 ms) and the guest's F3 is never re-applied |

The `.bcd` segment before every 6k+5 marker is what the Pico types; it is not a fault.

## Boot 41 (AMD) -- the controller-silence probe

Boot 40's death could not be attributed: no counter distinguished "ctrl[2] stopped delivering
events" from "the hub behind root port 4 died". The next build reads the controller's own
registers and issues a No-Op command 30 s after every keyboard on a controller falls silent
(#781), and re-applies the guest's typematic rate to a re-claimed keyboard (#791).

Run card: `tools/hw-val-2026-08-25/RUN-CARD-2026-09-02-boot41-amd-input.md`, set as
`RUN_CARD` in `stage.sh`. One `CTRLSILENCE` line after the death is the whole point of the run.

## Boot 41 result (AMD 5950X, 2026-09-02)

Build `67a2afd-dirty` (dirty = uncommitted `tools/` cfg edits only), `silence_revive=0`. Run
2505 s of guest uptime (42 min), stopped by the operator once the Pico and the keyboard were both
dead. Logs under `tools/hw-val-2026-08-25/logs/boot-41/` (gitignored). Times below are guest
uptime, taken from the `HB-<uptime>` heartbeats in `RUN1A.LOG`; the `[NNNNNNNNNN]` log stamp is a
byte count of the log, not a clock.

The probe fired, and it answered boot 40's question: **the controller itself stopped, not the hub.**

```
CTRLSILENCE ctrl[2]: 3 keyboard(s) silent for 30s
  USBSTS=0x00000010 HCH=0 HSE=0 EINT=0 PCD=1 HCE=0 | USBCMD=0x00000001 CRCR.CRR=1
  IMAN=0x00000000 IMOD=0x00000fa0 ERDP=0x141adb840 sw_deq=0x141adb840 pending_event=0
  PORTSC[4]=0x00000e03 CCS=1 PED=1 PLS=0
  No-Op FAILED cc=0 in 6043295us, cmd timeouts=1
command ring still RUNNING 5000 ms after Command Abort (crcr=0x00000008 usbsts=0x00000010)
```

Every register says healthy: not halted, no host system error, no host controller error, command
ring Running, root port 4 connected and enabled with its link in U0, the event ring fully consumed
(hardware ERDP equals the software dequeue, no unconsumed event). Yet a No-Op, which touches no
device and no hub, produced no completion in 6 s, and Command Abort left CRR set. A hub failure
cannot do that; a software dequeue fault would show `pending_event=1`. The controller has stopped
producing events of every kind while reporting that it is running. `PCD=1` is the normal resting
state of this controller under hype (hype never clears it) and carries no information.

| Time | Event |
| --- | --- |
| 458, 845, 1230 s | the Pico left the bus (3, 3 and 2 `cc=4` errors first), period 387 s; re-claimed in 1-5 s each time |
| 1235 s | first report from the re-claimed Pico |
| 1262-1271 s | the Pico's last report (1262 s or earlier), the Logitech c547's last report (1269-1271 s), and the hub status polls' last completion (1251-1281 s) |
| 1306 s | the probe: No-Op issued at silence + 30 s, failed 6 s later, abort ineffective |
| 2505 s | operator power-off |

| Ticket | Result |
| --- | --- |
| **#781** | Answered. The fault is the controller ceasing to process both its command ring and every transfer ring at once, with no error bit raised. Software cannot restart it: Command Abort does nothing. The only recovery is a host controller reset, so **#782-#786 move from "defence in depth" to "required for input to survive a run"** |
| **#775** | Confirmed as a symptom of the same fault: the endpoints did not lose a completion, the controller stopped issuing them |
| **#791** | Fixed on hardware. `re-applied: delay 250ms period 33ms` at every Pico re-claim (458, 845, 1230 s); held-key runs 69,69,69,60 before the first drop and 62,66,66,69,61,45,66,68,69,69,61 after. Boot 40 was 68 before, 27 after. The 45 at 837 s ended 8 s before the second bus drop |
| **#788** | Worse this boot: 13 doubled characters in 120 alphabet passes (4,320 characters, 3.0 per 1,000); boot 40 was 4 in 11,542 (0.35 per 1,000), boot 38 was 1 in 1,395. The first 16 passes (to 371 s) were clean, the 13 fall between 386 s and 1152 s. 0 missing characters |
| **#787** | Unchanged: 3 drops at a 387 s period, each preceded by `cc=4` transaction errors. The death came 40 s after the third re-arrival; boot 40's came 109 s after its sixth. Two points are not a pattern, but the re-enumeration is the only host-side activity on this controller in the minutes before either death |
| **#790** | Held: 0 `REVIVE`, 0 `DEADMAN`; the one `cmdring timeout` is the probe's own No-Op |
| **#426** | Not met: 42 min, input dead from 21 min |

## Boot 42 (AMD) -- the reset series, first hardware run (#786)

Boot 41 left one recovery: a host-controller reset. The #781 series lands it: #782 injects
the wedge under QEMU (`-DHYPE_781_WEDGE_MS`, absent from a default build), #783 records which
controller carries the log sink and the boot medium and refuses to reset it, #784 releases
everything a controller owns, #785 resets it and re-enumerates through the arrival path.
`tools/781/run-781-wedge.sh 2` shows the input controller wedged on demand and back in 1.2 s
with its hub, keyboard and mouse reporting; `run-781-wedge.sh 1` shows the log controller
refused with the refusal itself on the stick.

Run card: `tools/hw-val-2026-08-25/RUN-CARD-2026-09-02-boot42-amd-reset.md`, set as
`RUN_CARD` in `stage.sh`. The stall recurring and the keyboards coming back on their own is
the whole point; the stall not recurring is a run with no result.

## Boot 42 result (AMD 5950X, 2026-09-02)

Build `5e696dd-dirty` (dirty = uncommitted `tools/` cfg edits only), `silence_revive=0`,
`XHCIOWN: log sink on ctrl[1], boot medium on ctrl[1]`. Run 4262 s of guest uptime (71 min),
stopped by the operator with the Pico and the Logitech both typing. Logs under
`tools/hw-val-2026-08-25/logs/boot-42/` (gitignored). Times are guest uptime from the `HB-<uptime>`
heartbeats in `RUN1A.LOG`.

**The reset works on the real controller. The stall came three times; the controller was reset
three times; every keyboard and the mouse came back every time; the log on ctrl[1] never
stopped.** The bound is now the limit: all 3 resets were spent by 2636 s, so a fourth stall would
have left input dead for the rest of the run. None came in the remaining 27 min.

```
[0001179373] fw-1 CTRLSILENCE ctrl[2]: 3 keyboard(s) silent for 30s | USBSTS=0x00000010 HCH=0 HSE=0 EINT=0 PCD=1 HCE=0 | USBCMD=0x00000001 CRCR.CRR=1 | ... pending_event=0 | PORTSC[4]=0x00000e03 CCS=1 PED=1 PLS=0 | No-Op FAILED cc=0 in 6041256us, cmd timeouts=1 [#781]
[0001182983] fw-1 XHCIRESET ctrl[2]: reset #1 begins -- releasing everything on it, then HCRST [#784 #785]
[0001189814] fw-1 XHCIRESET ctrl[2]: reset #1 done in 2004 ms | released kbd=3 mouse=1 media=0 inventory=8 hubs=3 retry-slots=2 | back: ports=6 devices=8 keyboards=3 mouse=1 [#785]
[0004627932] fw-1 XHCIRESET ctrl[2]: reset #2 done in 2036 ms | released kbd=3 mouse=1 media=0 inventory=8 hubs=4 retry-slots=0 | back: ports=6 devices=8 keyboards=3 mouse=1 [#785]
[0005223426] fw-1 XHCIRESET ctrl[2]: reset #3 done in 1972 ms | released kbd=3 mouse=1 media=0 inventory=8 hubs=4 retry-slots=0 | back: ports=6 devices=8 keyboards=3 mouse=1 [#785]
```

Every stall had the boot 41 signature: all registers healthy, `CRCR.CRR=1`, `ERDP == sw_deq`,
`pending_event=0`, No-Op failed at 6.04 s, Command Abort left CRR set after 5 s. Stalls 2 and 3
say `2 keyboard(s) silent` because the Keychron had 0 reports since reset 1 and so had no
silence to measure.

| Stall | Last report before (Pico / Logitech) | `CTRLSILENCE` | Reset done | First report after | Dead for | Pico re-arrival before it |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 491 s / 499 s | 537 s | 542 s (2004 ms) | 545 s / 545 s | 46 s | 408 s (129 s earlier) |
| 2 | 2282 s / 2287 s | 2328 s | 2331 s (2036 ms) | 2335 s / 2336 s | 49 s | 1987 s (341 s earlier) |
| 3 | 2593 s / 2594 s | 2632 s | 2636 s (1972 ms) | 2638 s / 2638 s | 44 s | 2411 s (221 s earlier) |

The dead time is the detector, not the reset: 30 s of silence, a 6 s No-Op, a 5 s abort, then a
2 s reset. The reset itself is 2 s.

| Time | Event |
| --- | --- |
| 46 s | 3 keyboards + mouse claimed on ctrl[2], `INVENTORY -- 14 device(s) across 2 controller(s)` |
| 405, 828, 1194, 1600, 1984, 2407, 2826, 3210, 3594, 3978 s | the Pico left the bus (10 drops; 8 `cc=4` errors in total, 3+3+1+1 on the first four, none on the last six), re-claimed in 1-6 s each time; intervals 366-423 s, median 384 s |
| 537, 2328, 2632 s | controller stall, reset, recovered (table above) |
| 3992-3993 s | operator hot-plug: Keychron `3434:0da4 behind hub slot 2 port 2 DEPARTED`, re-claimed 1 s later, 18 reports typed on it afterwards |
| 4262 s | operator power-off |

Keystrokes: 20,938 handed to the guest, reconstructed from `KBDCHARS` with 0 gaps; Pico markers
`a0001`..`a0312`, 305 of 312 present. The 7 missing markers fall in the three dead windows (the
Pico types through them). 412 alphabet passes: 372 exact, 40 with one doubled character, 0 with a
missing character.

| Ticket | Result |
| --- | --- |
| **#786** | Met. Three real stalls, three resets, `keyboards=3 mouse=1` back each time, the Pico and the Logitech reporting within 3 s of each reset, the operator typing at 71 min. `fw-1` lines continue through all three resets: the log on ctrl[1] survived |
| **#785** | Works, and the bound is now the open question: 3 resets were used in 44 min (stalls 25 min, 5 min and 27+ min apart). A fourth stall would have been `left dead`. The stall is the controller's steady state on this machine, not a one-off, so "3 per run" is the wrong shape; a follow-up should make it a rate (N within a window) or drop it and keep the count in the line |
| **#783** | Not exercised (correct): no `REFUSED` line, ctrl[1] never stalled, `XHCIOWN` named ctrl[1] for both roles as predicted |
| **#784** | `released kbd=3 mouse=1 media=0 inventory=8 hubs=3/4 retry-slots=2/0` on every reset; no leaked slot: the re-claims reuse slots 4-6 each time |
| **#781** | Trigger confirmed three times, same signature as boot 41 |
| **#787** | Unchanged: 10 drops at a 384 s median period. The stalls came 129, 341 and 221 s after a re-arrival, and 7 re-arrivals had no stall at all. Boot 40/41's "40-109 s after re-enumeration" lead is dead |
| **#788** | 40 doubled characters in 14,832 alphabet characters (2.7 per 1,000); boot 41 was 3.0, boot 40 0.35. Not fixed, not worse |
| **#790** | Held: 0 `REVIVE`, 0 `DEADMAN`; the 3 `cmdring timeout`s are the probe's No-Ops |
| **#734 / #746** | Hot-plug of a keyboard on the reset controller at 66 min: departed and re-claimed in 1 s, typing resumed |
| **#426** | 71 min with input working at the end. First run since boot 30 to end with a live keyboard |

## Boot A result (Intel i5-13420H, 2026-09-03)

Build `1daa028-dirty`, config `hype2g.cfg` (Alpine reboot-pin guest, 2 vCPUs granted as 4 logical
CPUs, plus the three #603 microtests). Run 190 s, operator-stopped: the Alpine guest took a
minute to leave firmware, reached GRUB, loaded the kernel and then stopped making visible
progress. Logs under `tools/hw-val-2026-08-25/logs/bootA-intel/` (gitignored).

**The microtests passed; the Alpine guest was starved by hype's own loop.**

| Ticket | Result |
| --- | --- |
| **#729** | Met: `micro/vmexit: MSR round-trip (MTRR var0 base) wrote 0x123456000, read back 0x0000000123456000`, `0 probe(s) failed out of the non-fatal set` |
| **#603** (VMX leg) | Met: every probe ok, `vmexit` triple-faulted on purpose and `vm1 'micro/vmexit' STOPPED`, `WATCHDOG vm2: faulted: unhandled-exit storm ... forcing THIS vm off; others keep running`, `MICRO PASS: hello`, vm0 kept running. SVM leg = boot AMD-1 |
| **#525 #698** (VMX legs) | No result: vm0 never reached the restart |

Why vm0 stalled, from its own counters:

```
fw-1 COSTHIST: mean_per_exit vmrun=25271ns body=741965ns (vmrun_tot=6000ms body_tot=181000ms)
fw-1 LOOPPHASE: diag=3716ms persist=11ms house=176710ms dispatch=923ms [#365]
fw-1 BSPPROBE vm0: exits=244434 last=0x1f@0xffffffffac09b1c7 IN-HOST for 7812ms section=5 [#483]
fw-1 GUESTPC vm0: exits=244435 ... cr0=0xc0050033 cr3=0x113a4000 cr4=0x2070 ...
```

The guest executed for 6 s of the 190 s. The other 176 s were hype's housekeeping between two
exits, 1-13 s per exit. Every stalled sample shows the guest's CR0 with CD (bit 30) and NW
(bit 29) set: 0xc0000033 while OVMF programmed its MTRRs, 0xc0050033 while Linux's mtrr code
did the same. The only two samples with CD clear (0x80000033) sit exactly where the exit count
jumped 836 to 937 and 939 to 69634, the two fast bursts. Boot 2a on this machine (build 976e71a)
never showed CD set in 140 samples and spent 148 us per MSR exit against 34,990 us here. The
difference is #729's MTRR model: with round-tripping MTRRs, Linux runs its full cache-disable
programming sequence, and hype let that CD reach the physical CR0. The same config under nested
SVM (`tools/698/run-2g.sh`) passed the whole script in 66 s.

Fix: f26b67c owns CR0.CD and CR0.NW in the VMX CR0 guest/host mask, strips them from the hardware
guest CR0 and keeps them in the read shadow, as KVM does. Drive re-staged at f26b67c with the
same card; boot A is owed again.

## Boot A2 result (Intel i5-13420H, 2026-09-03)

Build `f26b67c-dirty` (the CR0.CD fix), same config. The stick's `HYPE.LOG` ends at t=69 s;
the machine ran on for some minutes with the dashboard freezing and stuttering, VM switching
struggling and the dashboard's log counter reaching 139 KB behind, then hype locked completely.
Logs under `tools/hw-val-2026-08-25/logs/bootA2-intel/` (gitignored).

**The CR0.CD fix held.** Every `GUESTPC vm0` sample reads `cr0=0x80050033`; the guest booted
through GRUB into the kernel, brought its APs up (`TMRLATE vm0/1 deliveries=1043 -> 2015`) and
reached "verifying modloop" on screen. #795's mechanism is gone.

**The BSP is starved by the PS/2 poll.** Same number in both Intel runs, absent on the 5950X:

```
fw-1 BSPCOST input 82% total=49766ms hits=2475422 mean=20us [#773]        (A2, i5-13420H)
fw-1 BSPCOST input 81% total=1911145ms hits=1052720529 mean=1us [#773]    (boot 42, 5950X)
```

`fw_1_host_input_poll()` reads the i8042 status port once per BSP loop iteration. On this
laptop the i8042 is behind the embedded controller and one read costs 20 us; at 42,000
iterations a second that is 84% of the BSP, leaving the dashboard, the leader chord and the log
flush the rest. Fixed in 92c23a0: the polled read is gated to 4 kHz (a PS/2 byte takes ~1 ms on
the wire), the IRQ path is untouched, and `KBDIRQ` now prints `ps2reads=` and the slowest read.
Ticket #796.

**The final lock is unrecorded.** Nothing after t=69 s reached the stick, although the flush was
healthy at that point (`USBFLUSH ... behind=1151B`). The last breadcrumb is `BSPALIVE phase=2`
(input). Whether the rate limit also removes the lock is the first thing the re-run answers.

Drive re-staged at 92c23a0, same card. Boot A is owed a third time; it still carries #525 and
#698's VMX legs, and now #795 and #796.

## Boot A3 result (Intel i5-13420H, 2026-09-03)

Build `92c23a0-dirty` (CR0.CD and PS/2 fixes), same config, 154 s. Logs under
`tools/hw-val-2026-08-25/logs/bootA3-intel/` (gitignored).

**#796 met.** `BSPCOST input 20% total=30200ms hits=355134176 mean=0us` against 82% at 20 us
before; `KBDIRQ ... ps2reads=554880 max=40us`, about 3,700 status reads a second with the
slowest at 40 us. Flush 5%, dashboard responsive, no lock.

**#525's VMX leg is half met.** The guest booted to login, the script pinned the reboot to CPU 1
and `vm0 vCPU 1 guest reset via ACPI reset register (0xCF9) -> restart` fired on real VMX
hardware. The second boot then hung inside OVMF before MP init: no console output after the
restart, `GUESTPC vm0 ... rip=0xfffd44e3 rflags=0x2 cr0=0x80000033` spinning with only host-tick
exits, no `SIPI received` line, every `TMRLATE vm0/N` frozen.

The cause is VMX-only. `INTDIAG vm0/0 ... staged_eventinj=0xf8 IF=0` from the restart onward:
0xf8 is Linux's REBOOT_VECTOR, the IPI `smp_send_stop()` sends to the other CPUs just before
the reboot CPU writes 0xCF9. vCPU 0's loop had staged it into `VM_ENTRY_INTR_INFO` for the
next entry when the restart landed; the VMCS rebuild never writes that field, so the new
firmware's first VM entry delivered interrupt 0xf8 through a zeroed IVT. SVM's VMCB rebuild
zeroes EVENTINJ, which is why the same config passes under nested SVM. Fixed in 478f6f8
(#797): both VMX reset paths clear the staged event; SVM's reset also drops deferred vectors.

The script's own `STALE-SHELL` fail-if fired one second before the reset because Alpine's
`reboot` takes several seconds and the screen matcher satisfied `expect localhost login:`
against stale text: a #728-class harness race, and the defence worked as designed.

Drive re-staged at 478f6f8, same card. Boot A is owed a fourth time, for #797, #525's second
half and #698.

## Boot A4 result (Intel i5-13420H, 2026-09-03)

Build `478f6f8-dirty` (#797's fix), same config, 127 s. Logs under
`tools/hw-val-2026-08-25/logs/bootA4-intel/` (gitignored).

**#797's fix held** (`INTDIAG vm0/0 ... staged_eventinj=0x0` after the restart), and the second
boot hung again in the same place. This time the numbers said what it was: the first OVMF took
3 s from VM start to `BdsDxe`; the second spent 53 s inside SEC's LZMA decompressor
(`rip=0xfffd3ee6..0xfffd413b`, 64-bit code, the range decoder's `shr $0xb; imul`), executing
continuously at ~1,400 host-tick exits a second with `cr0=0x80000033`. Fifty times slower for the
same code is uncached memory.

**Cause (#798).** `fw_1_vm_reinit()` reset the vCPU with `vm->npt_pml4` as its paging root. On
VMX the launch path picks `vm->ept_pml4` (the #272 block, whose own comment describes this very
defect: NPT permission bits read as EPT R|W|X, but the EPT memory type bits read 0 = UC) and
records it in `vm->used_root`; the SIPI path uses `used_root`; the restart did not. Every
restarted VMX guest therefore ran on an NPT table interpreted as EPT: the same pages, all
uncached. Nested SVM cannot show it, which is why the QEMU rig passes the restart. Fixed in
da3e93d.

Drive re-staged at da3e93d. Boot A is owed a fifth time, now for #798, #797, #525's second half
and #698.

## Boot A5 result (Intel i5-13420H, 2026-09-03) -- PASS

Build `da3e93d-dirty` (#798's fix), same config, 224 s. Logs under
`tools/hw-val-2026-08-25/logs/bootA5-intel/` (gitignored).

```
[0000279580] fw-1: vm0 vCPU 1 guest reset via ACPI reset register (0xCF9) -> restart [#94 #525]
[0000280659] fw-1: vm0 restarted (M8-4): pristine firmware restored, RAM zeroed, vcpu reset
[0000281309] fw-1 vm0 vCPU 1: SIPI received, entering guest [#190]
[0000465423] fw-1 SCRIPT vm0: PASS pass (21 directive(s), 138864ms)
[0000465478] fw-1 SCRIPT vm0:   at line 69: reboot-pin-nonbsp
```

The second OVMF ran MP init within a second of the restart and reached `BdsDxe` seconds later
(boots A3 and A4 never got there); the guest came back to a fresh login. `TMRLATE vm0/1`
climbed every sample after the restart (5093, 5430, 10323, 10659, 10928), no `TIMERSTALL`,
`INTDIAG vm0/1 ... IF=1 shadow=0x0` with nothing pending. `BSPCOST input 20%`.

| Ticket | Result |
| --- | --- |
| **#798** | Met: the only change from A4 is the restart root, and the second boot took seconds instead of never |
| **#797** | Met: the restart completes with a clean staged-event field |
| **#698** | VMX leg met; with the SVM bisect (2/2 frozen before #750, 3/3 climbing after) both legs are done |
| **#525** | VMX leg met (reset from vCPU 1 and a clean restart to login); SVM leg was boot B2 (2026-08-21) plus the QEMU regressions at HEAD |

Five Intel boots in one night, each finding one VMX-only host fault: guest CR0.CD reaching the
physical CR0 (#795), the 20 us i8042 poll (#796), a stale VM-entry event across the VMCS rebuild
(#797), and the NPT root handed to VMX on restart (#798). The Intel side of the runbook is now
down to #599/#605 (APICv), which wait on #708.
