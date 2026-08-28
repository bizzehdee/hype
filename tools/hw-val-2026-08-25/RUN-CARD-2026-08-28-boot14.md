# Boot 14 -- hot-plug, both paths

Boot 13 got the keyboard through GRUB and kept it. Hot-plug still failed, and it was two
separate faults -- one for the hub, one for the front port.

## DO NOT UNPLUG THE HYPE DRIVE

`HYPEBOOT` is the boot medium **and** the log medium. Only the keyboard and mouse move.

## What boot 13 found

**The behind-hub departure actually worked.** This is in your log:

```
host-hid: keyboard 3434:0da4 behind hub slot 2 port 2 (route 0x00002) DEPARTED
```

So #746 and #761 are doing their job. What went wrong came after.

**#768 -- the re-claim read the wrong device.** When the keyboard arrived on the front port
(controller 1, slot 5), hype claimed it under the name of the Logitech receiver, which is on
controller 2, slot 5. A slot id is per-controller and the search had no controller filter, so
it read one device and filed the answer against another. Both ended up with a block that
polled and never reported -- which is a keyboard that lights up and does nothing.

**#769 -- your front port was on a controller nothing was draining.** hype only notices a
plug when something is already dequeuing that controller's event ring. Your keyboard and
mouse are on controller 2, so controller 2 is pumped constantly by polling them. Controller
1 has no such device: its ring was being drained only as a side effect of the guest reading
the ISO from the drive that sits there. Once the kernel finished loading, controller 1 went
blind -- so the FIRST plug into the front port was seen and none of the later ones were.
That matches exactly what you described.

The sweep now drains every controller itself.

## The run

| step | do | wait |
|------|----|------|
| 1 | Press **Right-Ctrl + Right-Alt + D** | -- |
| 2 | Unplug the **mouse** from the hub | 15 s |
| 3 | Re-plug it, move it | 15 s |
| 4 | Unplug the **keyboard** from the hub | 15 s |
| 5 | Re-plug it into the hub, chord again | 15 s |
| 6 | Move it to the **front USB-C**, chord again | 15 s |
| 7 | Unplug from the front USB-C and re-plug it there, chord again | 15 s |
| 8 | Move it back to the hub, chord again | 15 s |
| 9 | Stop touching it | rest of run |

**Step 7 is the new one and the one that matters most** -- a second plug on the front port is
precisely what #769 broke.

## What to read

| line | means |
|---|---|
| `USB keyboard CLAIMED -- 3434:0da4 ...` after a re-plug | #768 fixed. If it says `046d:c547` when you plugged in the Keychron, it is not |
| `PORT EVENT port=N` on **each** plug into the front port | #769 fixed -- boot 13 got one and then nothing |
| `NOT re-armed` | should be absent. Boot 13 had one, so this is not fully closed |

## Known, and not fixed in this boot

- `hub slot 1 port 1 changed` repeats without end. That hub reports a port whose device is
  already known, so nothing ever fails and #763's give-up never engages. It costs control
  transfers but is not currently breaking anything.
- The one surviving drift report. Counting only, deliberately.

## Afterwards

    cp \HYPE.LOG \RUN1A.LOG  tools/hw-val-2026-08-25/logs/boot-14/
