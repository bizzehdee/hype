# Boot 15 -- unplugging the hub

Boot 14: the keyboard worked and survived its own hot-plug. Both #768 and #769 landed. What
failed was unplugging the hub itself -- and the cause was the noise, not the hub.

## DO NOT UNPLUG THE HYPE DRIVE

`HYPEBOOT` is the boot medium **and** the log medium. Only the keyboard, mouse and the hub
they are on.

## What boot 14 found

**`hub slot 6 port 1 changed` appeared 4,610 times** -- most of an 864 KB log, 68 bytes
apart. #763 stopped that port being *re-enumerated*, but not the hub *reporting* it: the
device there is already known, so no arrival ever fails and the give-up never engaged. Each
report still cost two control transfers from the guest dispatch loop.

**That is why the hub's own unplug was missed.** Ten port events in the whole run, all the
later ones on the other controller. The event ring holds 16 entries and a controller cannot
advance past hype's dequeue pointer, so events back up rather than drop -- and the hub's own
root-port change sat behind a backlog the storm kept refilling. hype never got to it.

**Also fixed:** the receiver's keyboard endpoint had been claimed twice, two entries sharing
one transfer and stealing each other's completions.

## The run

| step | do | wait |
|------|----|------|
| 1 | **Right-Ctrl + Right-Alt + D** | -- |
| 2 | Unplug the **keyboard** from the hub, re-plug it, chord | 15 s each |
| 3 | Move the keyboard to the **front USB-C**, chord | 15 s |
| 4 | Unplug and re-plug it there, chord | 15 s |
| 5 | Put it back on the hub, chord | 15 s |
| 6 | **Unplug the whole hub**, with the keyboard and mouse still in it | 20 s |
| 7 | **Plug the hub back in**, chord | 20 s |
| 8 | Stop touching it | rest of run |

**Steps 6 and 7 are the point of this boot.**

## What to read

| line | means |
|---|---|
| `hub slot N port M changed` -- a handful, not thousands | #770 fixed. If it is thousands again, it is not |
| `giving up on hub slot 6 port 1 ... the hub will stop being asked about it` | expected, once |
| `port 4 on this controller changed -- now empty` at step 6 | the hub's departure was finally SEEN. This is the new result |
| everything behind the hub released at step 6, re-enumerated at step 7 | the full outcome |
| two `HID[i/n]` entries with the same slot AND endpoint | #771 back |

If the hub's departure still is not seen, say so and stop -- the log will show whether any
port event arrived at all, which is the thing to know.

## Still open, so not a surprise if you see it

- One `NOT re-armed` line. Counting only, deliberately -- I do not act on it until I can
  explain every case.
- A device behind one SuperSpeed hub that cannot be addressed at all. hype gives up on it
  after three tries and now stops asking; that is by design.

## Afterwards

    cp \HYPE.LOG \RUN1A.LOG  tools/hw-val-2026-08-25/logs/boot-15/
