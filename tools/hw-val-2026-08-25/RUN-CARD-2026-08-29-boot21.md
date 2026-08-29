# Boot 21 -- catch the moment the keyboard dies, and see if the hitching is gone

**This build is not a fix for the deafness. It is aimed at making the next failure
legible, plus one real performance defect boot 20 exposed.** Say so plainly rather than
let a fourth "this might be it" go out.

## What boot 20 established

The keyboard reported 16 times, then stopped, and the counters said which of four things
happened:

```
handed=15 deliv=15 own=1 topark=0 lost=0 skipped=0 | hcevt=0 ringfull=0 evict=0
```

All zero. So no completion was lost, misattributed, or evicted, and the controller never
reported stopping. Four TRBs were armed with the doorbell rung and simply never completed.

That RULES OUT the whole family of defects fixed in boot 20 as the cause. Those were real
bugs and the QEMU rig confirms they are fixed, but they were not your bug.

Both keyboard endpoints stopped at exactly 16 reports -- different devices, different
slots, same controller. That points at something shared.

## What changed in this build

1. **The mouse is polled at 125 Hz instead of ~11 kHz.** Boot 20 measured 354,529 mouse
   polls against 1,851 keyboard polls in the same 31 seconds. The mouse poll was running
   from the guest's dispatch loop rather than the input tick, so it was hammering the
   controller's event ring on the guest's core. This is the best remaining explanation for
   the hitching, because boot 20 measured the input tick itself at a healthy 114 Hz and
   the slow-poll detector at zero.
2. **A HIDTICK line every two seconds.** Boot 20's diagnostic printed twice in 55 seconds;
   the keyboard died at 24 and the nearest sample was at 31. Two seconds brackets it.

A "went deaf" detector was written for this build and then deleted: the rig fired it four
times on a keyboard nobody was typing on. An interrupt IN transfer stays outstanding while
the device NAKs, so "armed and quiet" is exactly an idle keyboard. It would have filled
this log with alarms about a working keyboard.

## What to do

1. Cold boot from this stick. Wait for the dashboard.
2. **Say out loud, or note, roughly when things happen.** The log has no wall clock, but
   HIDTICK lines are every 2 seconds, so "died about 25 seconds in" is enough to find it.
3. **Type for about 30 seconds** -- slow, fast, pause, fast. A few chords. Hold a key ~3s.
4. **Judge the hitching specifically.** Is it better, the same, or worse than boot 20? That
   is a direct test of the mouse change, and your impression is the measurement here.
5. When the keyboard dies, **keep typing for another 10 seconds**, then stop and let it sit
   for a minute. The HIDTICK lines across that window are the whole point of the run.
6. Optional, only if it is still alive: hot-plug the keyboard, then the hub.

## What I will read

```
fw-1 HIDTICK[0]: 3434:0da4 slot4 ep=0x81 polls=N reports=N arms=N
                 lost=N skipped=N hcevt=N ringfull=N evict=N | mouse polls=N reports=N
```

- **the tick where `reports` stops climbing while `polls` keeps climbing** -- the moment,
  to within two seconds
- **whether `polls` also stops** -- that would mean the endpoint is not being polled at
  all, which is a different bug from the endpoint not completing
- **whether both keyboards stop on the same tick** -- shared cause, versus one at a time
- **`mouse polls`** -- confirms the 125 Hz gate is actually in effect on your hardware
- any of `lost` / `ringfull` / `hcevt` / `evict` moving off zero

If the keyboard survives the whole run this time, that is worth knowing too, but I would
not read it as fixed on one boot -- boot 17 survived and boot 18 died from the same build.
