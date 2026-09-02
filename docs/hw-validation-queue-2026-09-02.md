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
