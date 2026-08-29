# Boot 20 -- does the keyboard survive, and if not, WHICH failure is it?

Build: the interrupt-IN completion rework. Two reviewers, one real defect fixed, and a
second one caught in review before it shipped.

**Read this first: this run is worth doing even if the keyboard dies again.** Boots 8-19
could only tell us "it stopped". This build can tell us WHY it stopped, and the four
answers need different fixes. That is the point of the run.

## What changed since boot 19

1. **Completions are retired where they are routed, not where they are polled.** hype used
   to leave a finished transfer marked outstanding until the endpoint's next poll, so a
   second completion arriving in the same pass matched nothing and went to a shared table
   that evicts. An evicted interrupt-IN completion is unrecoverable -- the endpoint stays
   armed for ever and goes deaf. That path is gone.
2. **Report buffers are no longer reused while their report is unread.** Caught in review,
   not on your desk. Without it the fix above would have handed the controller a buffer a
   queued report still occupied, and you would have seen garbage keys instead of no keys.
3. **Rings are 256 TRBs, not 16.** The memory was always allocated. Removes event-ring
   overflow as a variable and cuts transfer-ring address reuse from every 15 to every 255.
4. **Key repeat is bounded to 10 seconds** and sends the key-up when it gives up, so the
   stuck key from boot 19 cannot repeat for ever.

## What to do

1. Cold boot from this stick. Wait for the dashboard.
2. **Type for about 30 seconds.** Mixed: slow, then fast, then pause, then fast again.
   Include a few chords (Ctrl+key, AltGr+key) and hold one key down for ~3 seconds.
3. **Hot-plug the keyboard**: unplug it, wait 5 seconds, plug it back into the SAME port.
   Type again.
4. **Hot-plug the hub itself**: unplug the hub the keyboard is on, wait 5 seconds, plug it
   back. Type again. This is the path the QEMU rig shows is weakest, so it matters most.
5. Let it run another minute so the periodic DIAG lines are written, then shut down and
   bring the stick back.

If the keyboard dies at any point, **stop hot-plugging and let it sit for a minute** --
the DIAG lines written while it is dead are the ones that classify the failure.

## What I will read, and what each answer means

The HID line now ends with counters that did not exist before:

```
fw-1 DIAG: HID[0/2] 3434:0da4 polls=N reports=N arms=N errors=N
           handed=N deliv=N own=N topark=N lost=N skipped=N
           | ctrl hcevt=N ringfull=N evict=N (slotN ep=0xNN)
```

If `reports` stops climbing, exactly one of these will be true:

| reading | meaning | what it costs to fix |
|---|---|---|
| `ringfull` or `hcevt` non-zero | the CONTROLLER stopped, not hype. xHCI 4.9.4. | different bug entirely; needs event-ring recovery |
| `evict` non-zero | a completion still reached the evicting table | the fix is incomplete; the remaining path is findable |
| `lost` non-zero | a completion arrived naming a TRB hype does not hold | attribution bug; ring pointers have drifted |
| all zero, `reports=0`, `arms=4` | no completion was EVER generated | the fault is upstream of hype's bookkeeping -- TT, scheduling, or the device |

That last row is what boot 18 looked like, and what the QEMU rig still produces. It is the
one I most want to see confirmed or ruled out on your hardware, because every software fix
so far has been aimed at the other three.

## Honest status

The eviction defect was real and is fixed, and its logic is now unit-tested (92% branch)
outside the firmware file where it was untestable.

It is **not** proven to be your bug. The QEMU rig is too noisy run-to-run for a single run
to prove anything -- the same build produced 837 hub reports on one run and 21 on the next,
so I am not claiming a QEMU result as evidence. And the rig still shows behind-hub
departures going unnoticed, which is a separate unfixed defect.

So: this may fix it, and if it does not, it should finally say why.
