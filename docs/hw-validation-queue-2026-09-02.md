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
