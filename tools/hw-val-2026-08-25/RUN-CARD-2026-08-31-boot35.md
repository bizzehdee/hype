# Boot 35 -- can the wedged command ring be aborted at all

Boot 34 was the most informative run yet, because the machinery finally said what it tried.

**The guest booted.** #780 is fixed: `media_disk` resolved, the ISO streamed, and the guest
reached a login prompt with the spare USB drives still plugged in. That was the condition that
broke boots 32 and 33.

**Your input observations were both real, and they are two different faults.**

- The Keychron went quiet from t=30 s to t=62 s and came back. A revive on controller[1] did
  that, unaided. It happened again at t=175 s and recovered again. Working as intended.
- The Pico and the Logitech receiver stopped at t=90 s and t=97 s and never returned, and your
  Pico re-plug was never seen -- because the hub that reports it sits on the same controller.

At t=114 s the log says exactly why:

```
#266 command TIMEOUT waiting for trb=0x141ad8350 (no event arrived, timeout 1)
command ring stopped answering -- aborting it. usbsts=0x00000010 crcr=0x00000008 (CRR=1)
command ring still RUNNING 200 ms after Command Abort -- it cannot be restarted from software
REVIVE FAILED slot=3 ep=3 -- ... This endpoint is deaf and hype cannot rebuild it
```

Controller[2]'s command ring wedged. The recovery fired -- that is new -- and the Command
Abort was ignored. `usbsts=0x00000010` is Port Change Detect alone: no halt, no host error.
By its own account the controller is fine; only its command ring is stuck.

## What changed in this build

Two reasons the abort might have been ignored, both mine rather than the hardware's:

- **The write was 32 bits.** CRCR is one 64-bit register (xHCI 5.4.5) and only its low half
  was written. Now written as 64.
- **The wait was 200 ms.** xHCI 4.6.1.2's own bound for an abort to take effect is **five
  seconds**. 200 ms was my number, not the spec's, and boot 34 could not tell "ignored" from
  "not finished yet". Now five seconds.

Also new: `int_in_revive()` had three silent `return -1`s and now names which step failed --
Stop Endpoint, Set TR Dequeue, or a Set TR Dequeue the controller REFUSED (with its completion
code). And a command abandoned because 64 events came off the ring without its completion is
counted as `guard=` rather than sharing a timeout's silence.

## The sequence

1. Boot. Stay on the dashboard. Press BOOTSEL once to arm the Pico, confirm `a0001`.
2. Check the guest reaches a login prompt.
3. Leave the machine alone for **at least 90 minutes**. Nothing to do by hand.
4. Power off normally. Bring back `HYPE.LOG` and `RUN1A.LOG`.

If input on the Pico stops and does not come back within a couple of minutes, that is a
result, not a failed run -- leave it going anyway, because the whole point is what the log
says about the abort.

## What decides it

The wedge may not recur; it has happened in two runs out of nine. If it does, `HIDTICK`'s
`cmdring` field settles the question:

| What the log says | What it means |
| --- | --- |
| `timeouts=0` all run | The ring never wedged. Nothing proved either way -- run it again. |
| `recoveries=` non-zero, input alive | **The abort worked.** A 64-bit write or the honest five-second wait was the missing piece, and #781 can be rejected unbuilt. |
| `... DEAD`, still "command ring still RUNNING 5000 ms after Command Abort" | The controller genuinely will not abort. Software is out of moves and #781 -- reset the controller and re-enumerate -- is the only remaining recovery. |
| `guard=` non-zero | A third thing: the completion never came back because 64 other events crowded it out. Different fix again. |

Everything else still rides along: #773, #774 and #777 from the Pico's typing into the guest,
#775 from the revives, #641 from `APVCPU` and `PERF`.
