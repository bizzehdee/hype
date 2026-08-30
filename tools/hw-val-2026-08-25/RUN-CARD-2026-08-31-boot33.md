# Boot 33 -- the port-power fix, and the Pico does the typing

Boot 32 found a real bug in about eighty seconds, and it is a good one.

You unplugged the Keychron from its rear root port. hype saw it go, correctly:

```
[0000228626] PORT EVENT port=10 (event #6 on this controller) [#760]
[0000228693] port 10 on this controller changed -- now empty [#744]
[0000228758] keyboard 3434:0da4 on port 10 slot5 DEPARTED -- releasing its endpoint and slot
[0000228855] Disable Slot: 5
```

Then you plugged it back in and **nothing happened at all** -- not a port event, not an
enumeration attempt, nothing, for the remaining two minutes of the run.

## Why

Acknowledging a port event means writing PORTSC's change bits back, because they are
write-1-to-clear and the controller raises no further event for a port whose change bits are
still set. hype wrote `sc & CHANGE_MASK` -- which clears the change bits correctly, and
writes 0 into every other read/write bit in the register on the way past. One of those is
**PP, Port Power**.

So hype acknowledged the unplug and switched the port off in the same write. An unpowered
port never reports a connect. The keyboard was plugged into a dead socket.

Fixed in this build: the ACK is now a read-modify-write that preserves everything, clears
only the change bits, and fires none of the write-1 strobes. There is a unit test.

## Before you boot: reflash the Pico

`tools/pico-kbd/build/hype_pico_kbd.uf2` has changed. Hold BOOTSEL while plugging the Pico
into **this** machine, drag the .uf2 onto the drive that appears, then move the Pico back to
its usual socket on the front hub.

It now types the #773 measurement itself. Boot 32 asked you to type a pangram fast, and you
reported the obvious flaw -- at speed you dropped and doubled keys yourself, so the test
measured the typist. The Pico types `abcdefghijklmnopqrstuvwxyz0123456789` instead: strictly
increasing, every character exactly once, so a dropped character leaves a visible gap and a
doubled one a visible repeat, with nothing to count and no reference copy to diff against. It
sends four passes at 8 ms per character (faster than any human) and four at 30 ms as the
control, on a rotation, for the whole run.

## The sequence

1. Reflash the Pico as above and put it back on the front hub.
2. Boot. Stay on the dashboard. Press BOOTSEL once to arm the Pico, confirm `a0001`.
3. **Unplug the Keychron from its rear socket, wait five seconds, plug it back into the same
   socket.** This is the one thing you have to do by hand.
4. Type a few characters on it to confirm it came back.
5. Leave the machine alone for **at least 90 minutes**.
6. Power off normally. Bring back `HYPE.LOG` and `RUN1A.LOG`, plus `HYPE.1.LOG`,
   `RUN1A.1.LOG` and `hype-log-prev.txt` if a warm reboot happened.

## What this run settles, and how

| Ticket | How | Needs you? |
| --- | --- | --- |
| **#745** root-port arrival and claim | Step 3. The log should show a `PORT EVENT port=10` on the re-plug, then enumeration and `USB keyboard CLAIMED`. | Yes, step 3 |
| **#744** root-port departure and teardown | Step 3's unplug half. Boot 32 already proved it, but on code whose ACK has since changed, so it is re-proven here rather than closed on stale evidence. | Yes, step 3 |
| **#773** keypress lost in the unarmed window | The Pico's fast and medium passes, all run. Compare the echoed lines in `RUN1A.LOG` against the alphabet. | No |
| **#774** held key never repeats | The Pico holds `h` for 12 seconds every sixth tag. | No |
| **#777** typematic repeats forever | The same 12-second hold crosses the 10-second bound. | No |
| **#779 / #775** command ring and revive | The 90 minutes. | No |
| **#641** idle-vCPU HLT storm | `APVCPU` and `PERF` print every run. | No |

## Reading the result

**For #773**, the two speeds are the measurement, not either one alone. If the fast passes
drop characters and the medium passes do not, the loss is rate-dependent and the unarmed
window is the cause. If both are clean, the window is closed. If both drop, the window is not
the explanation and something else is.

**For #779**, input alive at the end with `cmdring timeouts=0` proves nothing about the fix --
the ring never stopped, so the recovery was never exercised. The result worth having is input
alive with `cmdring recoveries` non-zero.

**And remember which controller is which.** The Keychron is on controller[1] with the log
stick; the Pico and the Logitech receiver are on controller[2], where boot 31 died. If
controller[2] stops again the Keychron will keep working and the machine will feel fine --
the Pico's `reports=` count in `HIDTICK` is the witness for that controller, not your hands.
