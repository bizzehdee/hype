# Boot 16 -- catch the keyboard dying, with the evidence attached

This boot does not fix anything. Its whole job is to record what the ring looks like at the
instant the keyboard stops, and what the dashboard freeze actually is.

## DO NOT UNPLUG THE HYPE DRIVE

`HYPEBOOT` is the boot medium **and** the log medium.

## What you need to do

**Make the keyboard die, then stop.** Nothing else.

1. Boot. Wait for the guest to reach a login prompt.
2. Press **Right-Ctrl + Right-Alt + D** a few times, so there is a healthy baseline in the log.
3. Then use the keyboard **continuously** -- hold a key down, type steadily, whatever you
   like -- until it stops responding. Last time that was a few seconds in, with a dashboard
   freeze as it went.
4. **The moment it stops: stop touching everything.** Do not hot-plug, do not re-cable. The
   diagnostic is written when it happens, and the log needs a minute of hype running quietly
   afterwards to flush it.
5. Leave it for two minutes, then power off.

If the keyboard does **not** die this time, let the run go the full twenty minutes and say
so -- that is also a result, and a more interesting one.

## Why "then stop" matters

Every previous boot ended with a burst of hot-plugging after the failure, which is
reasonable -- you were trying to get the keyboard back. But it buries the moment of failure
under hundreds of enumeration lines, and on boot 14 that backlog was itself enough to hide a
real event. This boot needs the seconds *after* the death to be quiet.

## What the build now records

Two things that were not in any previous boot:

```
host-xhci: #764 DIVERGED slot=N ep=M | controller deq=+0x.. hype trb=+0x.. enq=.. cyc=..
                                     | reports=.. silent=.. rearms=..
host-xhci: #764   claim[-1] trb=+0x.. cc=..      (the last eight completions hype claimed)
```

The controller's position against hype's, checked every 64 polls instead of every 4,000, so
it is caught near the moment rather than half a minute later. Previous boots printed only a
late summary whose numbers no longer described the failure.

```
host-xhci: #764 SLOW POLL slot=N ep=0x.. took NNN ms -- this is what a dashboard freeze is
```

Every keyboard poll is timed. A freeze is something blocking, and this says whether it is in
the USB path at all and which device owns it.

## Worth knowing before you run it

**I reproduced this in QEMU while building the boot.** The new rig -- which now mirrors your
machine: two controllers, four hub devices, a hub behind a hub, a composite keyboard+mouse
like your Logitech receiver, and a device that will not enumerate -- caught one:

```
#764 DIVERGED slot=3 ep=3 | controller deq=+0x10 hype trb=+0x0 enq=1 cyc=1 armed=1
                          | reports=0 silent=1471 rearms=0
```

A hot-plugged keyboard: the controller finished TRB 0 and moved on, hype is still waiting on
TRB 0, and no report ever arrived. Enumerated, claimed, deaf -- your symptom.

So this boot may not be necessary. If you would rather I chase the QEMU reproduction first
and save you the boot, say so; it is the cheaper path and I would default to it. The drive is
staged either way, and a hardware capture would still tell us whether the desk fails the same
way or differently.

## Afterwards

    cp \HYPE.LOG \RUN1A.LOG  tools/hw-val-2026-08-25/logs/boot-16/
