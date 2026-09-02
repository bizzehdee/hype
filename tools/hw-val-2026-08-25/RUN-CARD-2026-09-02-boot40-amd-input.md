# Boot 40 (AMD 5950X) -- input with the silence revive OFF

Build: default, no `EXTRA_CFLAGS`. Commit `6dabf79` or later. Config unchanged from boot 39.

## What changed since boot 39

Every command-ring wedge from boot 27 to boot 39 was a `Stop Endpoint` issued by the #775
silence revive. Boots 8-26 had no revive and no wedge. The revive is now off by default (#790),
and every `HIDTICK` line ends with `silence_revive=0` so this log states which build it is.

The HID drain now takes every queued report per tick instead of one (#788). A release report
no longer waits a tick behind its press, so a stall between ticks cannot make the typematic
clock repeat the key.

## Before you boot

- Confirm the banner sha matches the staged build.
- Confirm the first `HIDTICK` line says `silence_revive=0`. If it says `1`, stop: wrong build.

## The sequence

Unchanged from boot 39.

1. Boot, stay on the dashboard.
2. **Right-Ctrl + Right-Alt + 1** (both modifiers right-hand).
3. Wait for `localhost login:`, type **`root`** and Enter, get `localhost:~#`.
4. **BOOTSEL once** -- LED solid, `a0001` appears in the guest.
5. Leave it **90 minutes**.
6. Power off normally. Bring back `HYPE.LOG` and `RUN1A.LOG`.

## What the 90 minutes is for

| Ticket | What to read | Passes when |
| --- | --- | --- |
| **#790** | `cmdring timeouts=` on every `HIDTICK`; any `REVIVE` or `command TIMEOUT` line | `timeouts=0` for the whole run, no `REVIVE` lines, no `stopped answering`. Boots 34-39 wedged between 320 s and 1180 s; 90 minutes clean is the pass |
| **#781** | the same lines | if a wedge still happens with no `REVIVE` before it, the trigger is not the revive and the #781 reset series stays justified. Record the command that timed out |
| **#788** | the Pico's alphabet passes in `RUN1A.LOG` and `KBDCHARS` | no doubled character across all passes. Boot 38 had 1 in 1,395 |
| **#775** | `reports=` on every keyboard | all three keyboards keep reporting for the whole run with `revives=0`. If one goes deaf with the ring healthy, that is a real deaf endpoint and #775 reopens with evidence the revive could not have masked |
| **#787** | when the Pico dies, if it does | without the wedge, does the Pico still go silent? If yes, #787 is a Pico or hub fault, not hype |
| **#426** | the run reaching 90 minutes | the standing HW-VAL gate |

## A/B if #790 passes

A second boot built with `EXTRA_CFLAGS=-DHYPE_INT_IN_SILENCE_REVIVE=1` (after `make clean`)
should wedge again inside 20 minutes. One such run closes the question; it is optional.
