# Boot 13 -- the ring desync

Boot 12's keyboard freeze was mine, introduced two boots ago, and the diagnostic I left
running is what caught it.

## DO NOT UNPLUG THE HYPE DRIVE

`HYPEBOOT` is the boot medium **and** the log medium. Only the keyboard and mouse move.

## What boot 12 found

The #764 lines said the opposite of what I expected:

```
slot=4 ep=3 dequeue 0x141b8b000 is past our TRB 0x141b8b0a0
```

`0x...000` is the ring base. The controller's dequeue was *behind* hype's TRB -- index 0
against index 10 -- on **all four** interrupt-IN endpoints of that controller at once. The
controller parked on a TRB whose cycle bit says not-ready; hype waiting for a completion
that could never come.

Cause: my #761 change matched completions on (slot, endpoint) and ignored the TRB pointer,
so a stale event was claimed as the current transfer. `armed` clears, hype re-arms, the
enqueue pointer advances, and the controller never consumed what was actually outstanding.
One TRB of drift per mis-claim until the ring wraps and the endpoint is silent for good.

That is also why re-plugging three times did nothing: the **hub** endpoints had desynced
too, so hype could not see the unplug. There was no way back.

**And no, it was not the kernel load** -- you asked. The boot/ISO medium is on controller 1
with its own event ring; everything that broke is on controller 2. What was on controller 2
was a hub reporting `port 1 changed` 104 times, and the drift accrues per event. GRUB is
when it tipped over, not why.

## The run

Same sequence as boot 12.

| step | do | wait |
|------|----|------|
| 1 | Press **Right-Ctrl + Right-Alt + D** | -- |
| 2 | Unplug the **mouse** from the hub | 15 s |
| 3 | Re-plug it, move it | 15 s |
| 4 | Unplug the **keyboard** from the hub | 15 s |
| 5 | Re-plug it, chord again | 15 s |
| 6 | Move the keyboard to a **direct port on the machine**, chord again | 15 s |
| 7 | Move it back to the hub, chord again | 15 s |
| 8 | Stop touching it, and let the guest boot all the way | rest of run |

**Please let it run well past GRUB this time even if everything looks fine** -- that is
where boot 12 died, and a clean pass through it is the result I need.

## The one line that decides it

```
host-xhci: interrupt-IN slot=N ep=M dequeue ... NOT re-armed
```

**It should not appear at all.** Both rigs now report zero. If it appears on hardware, the
fix is incomplete and the log says which endpoint and by how much.

(Earlier boots saw these fire harmlessly at the ring's link TRB. That case is understood and
excluded now, so any that remain are real.)

Otherwise as before: `giving up on hub slot N port M` once or twice early and never again,
and `HUBPOLL hub-devices=5 ... reports=` small and non-zero.

## Also

I have written up why so much of this only showed on your desk --
`docs/qemu-vs-hardware.md`, staged here as `QEMU-VS-HARDWARE.md`. Short version: mostly our
test rigs, not QEMU. The rigs model one controller, one hub and four devices; your machine
has two controllers, five hub devices and twelve devices.

## Afterwards

    cp \HYPE.LOG \RUN1A.LOG  tools/hw-val-2026-08-25/logs/boot-13/
