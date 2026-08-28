# Boot 11 -- hot-plug, now proven in QEMU first

Boot 10 fixed the hitching and kept the keyboard working. This boot is hot-plug, and this
time the behind-hub cycle is proven in QEMU before it reaches your desk.

## DO NOT UNPLUG THE HYPE DRIVE

`HYPEBOOT` is the boot medium **and** the log medium. Only the keyboard and mouse move.

## What boot 10 found

Your log answered both questions.

**Why the keyboard died on the first hot-plug.** One event on root port 4 -- the port the
hub is on -- was read as "now empty", so hype released *everything* behind the hub: both
keyboards and the mouse. Chords were working right up to that moment (`modseen=0x55`).
Your first hot-plug attempt destroyed the working keyboard.

**Why behind-hub hot-plug never worked at all.** `HUBPOLL polls=86155 reports=0`. Two bugs:

| # | what |
|---|---|
| 761 | `control_transfer()` and `cmd_submit_wait()` **dropped** any transfer event that was not the one they wanted. For an interrupt endpoint that is permanent: it stays armed, is never re-armed, and never reports again. A hub's endpoint is armed during enumeration, when those calls are constant, so it lost its first completion essentially every boot -- one event for the hub's slot in a whole run, then silence |
| 762 | Once reports worked, one unplug produced 5,504 reports of the same bitmap: only `C_PORT_CONNECTION` was being cleared, and an unplug sets several change bits. Now 5 |

Both are fixed and the full behind-hub cycle -- depart, re-enumerate, re-claim, report --
now passes in QEMU as an assertion, not a hope.

**Worth saying plainly:** rig 746 had been claiming QEMU could not test this. It can; that
claim was wrong, and it is why this reached you broken twice. The rig asserts the real
cycle now.

## The run

Boot, wait for the login prompt and the heartbeat, then:

| step | do | wait |
|------|----|------|
| 1 | Press **Right-Ctrl + Right-Alt + D** | -- |
| 2 | Unplug the **mouse** from the hub | 15 s |
| 3 | Re-plug it, move it | 15 s |
| 4 | Unplug the **keyboard** from the hub | 15 s |
| 5 | Re-plug it, chord again | 15 s |
| 6 | Move the keyboard to a **direct port on the machine**, chord again | 15 s |
| 7 | Move it back to the hub, chord again | 15 s |
| 8 | Stop touching it | rest of run |

If the keyboard dies at any step, that step is the answer -- note which one and stop.

## What should appear

Behind the hub (steps 2-5), the hub reports and hype acts:

```
host-xhci: hub slot N status bitmap 0x..
host-hid: keyboard ... behind hub slot N port M DEPARTED
host-usb: hub slot N port M ARRIVED -- 3434:0da4 ... enumerated
```

On a direct port (step 6), the root-port ladder instead:

```
host-xhci: PORT EVENT port=N
host-usb: port N ... changed -- something is attached
```

Two things to check afterwards:

- `HUBPOLL ... reports=` should be **small and non-zero** -- a handful per plug event. Zero
  means #761 is not fixed; hundreds means #762 is back.
- Nothing behind the hub should be released unless you actually unplugged it. A single
  `port 4 ... now empty` releasing three devices is boot 10's failure recurring.

## Afterwards

    cp \HYPE.LOG \RUN1A.LOG  tools/hw-val-2026-08-25/logs/boot-11/

`\HYPE.LOG` needs `LC_ALL=C grep -a`.
