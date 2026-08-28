# Boot 16 -- does your machine fail the same way mine does?

This is a correlation run. I have a reproduction in QEMU now, with a signature that repeats
exactly. One question: does the 5950X produce the same signature, or a different one?

Either answer is worth the boot. The same signature means one bug and I can chase it locally
without spending any more of your time. A different one means the desk has something QEMU
does not, and the difference itself is the lead.

## DO NOT UNPLUG THE HYPE DRIVE

`HYPEBOOT` is the boot medium **and** the log medium.

## What you need to do

**Make the keyboard die, then stop touching everything.**

1. Boot, wait for the guest login prompt.
2. Press **Right-Ctrl + Right-Alt + D** a few times, for a healthy baseline.
3. Use the keyboard steadily until it stops. Last time that was seconds in.
4. **Stop.** No hot-plugging, no re-cabling, nothing.
5. Leave it running quietly for two minutes so the log flushes, then power off.

Step 4 is the whole run. Every previous boot ended in hot-plug attempts -- entirely
reasonable, you were trying to recover it -- but that buries the moment under hundreds of
enumeration lines, and on boot 14 the backlog was itself enough to hide a real event.

If the keyboard does not die, let it run twenty minutes and say so. Also a result.

## What I found in QEMU, so you know what this is testing

The new rig -- your topology: two controllers, four hub devices, a hub behind a hub, a
composite keyboard+mouse like your Logitech receiver, and a device that will not enumerate --
reproduces a dead keyboard on **2 to 3 runs out of 4**, always like this:

```
#764 DIVERGED slot=3 ep=3 | controller deq=+0x10 hype trb=+0x0 enq=1 cyc=1 armed=1
                          | reports=0 | handed=0 deliv=0 from_park=0 own=0 to_park=0
```

The controller has finished the transfer hype is waiting on and moved to the next one, and
hype never saw the completion. The offset is **always exactly one TRB**, whether it happens
on the first transfer or the sixth. Every counter for "where did the completion go" reads
zero.

I had an explanation for this and it was wrong -- I built the fix, measured it, and it never
fired once while the fault carried on. That is on the ticket (#772) rather than buried.

## The one thing to read afterwards

```
host-xhci: #764 DIVERGED ...
```

Send me the line. What matters is:

| field | what it tells us |
|---|---|
| `deq` vs `trb` | if the gap is exactly one TRB, your machine and mine have the same bug |
| `handed` | if non-zero, the completion arrived and was filed and never picked up -- a different fault from mine |
| `reports` | whether the endpoint ever worked, or was deaf from its first poll |
| `#764 SLOW POLL` | whether the dashboard freeze is in the USB path at all, and which device owns it |

A gap of exactly one, with every counter zero, means I can stop asking you for boots and
work on it here.

## Afterwards

    cp \HYPE.LOG \RUN1A.LOG  tools/hw-val-2026-08-25/logs/boot-16/

`\HYPE.LOG` needs `LC_ALL=C grep -a`.
