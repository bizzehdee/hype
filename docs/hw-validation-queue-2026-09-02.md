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
