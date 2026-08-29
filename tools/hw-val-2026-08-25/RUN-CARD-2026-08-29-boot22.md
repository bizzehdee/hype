# Boot 22 -- overnight, unattended, with a robot at the keyboard

**Nothing to do but plug in, boot, press one button, and walk away.** The Raspberry Pi Pico
(#778) does the typing. This is the first run that does not need you at the machine.

## What changed since boot 21

1. **The typing latency is fixed.** Boot 21 caught its cause: 159 failed resets on one hub
   port, each reporting `status 0x0511` with PORT_RESET still SET -- hype was calling a port
   that was still resetting normally a failure. The wait was ~13 ms of spin against the 10 ms
   MINIMUM a hub is allowed to take, so it was a coin flip. It is now 200 ms in real 10 ms
   steps. And that failure was the one arrival path that never fed the #763 retry cap, so it
   retried for ever: each retry cost a 100 ms debounce plus the reset wait, INSIDE the input
   tick. About 17 seconds of blocked input across the run. That is the delay you felt between
   pressing a key and seeing it.

2. **The mouse is polled at 125 Hz**, not ~11 kHz from the guest dispatch loop.

Neither is a fix for the deafness. That is still unexplained.

## Set-up

1. Plug the **Pico** into the 5950X. Use the **same hub as the Keychron** -- that is the
   topology that fails. Leave your own keyboard plugged in as well; hype merges them.
2. Cold boot from this stick. Wait for the dashboard.
3. **Press the BOOTSEL button on the Pico once.** Its LED goes from blinking to solid. That
   is the arm: it types nothing until you do this, and a power cycle disarms it again.
4. Leave it running overnight.

That is all. No typing, no judging, no timing.

## What the Pico does, so the log can be read without you

- Types a tag every ten seconds: `a0001`, `a0002`, ... **The last tag in the log is the
  timestamp.** hype's log prefix is a byte offset, not a clock, so this is how we learn the
  exact second input stopped.
- Rotates through the things that have broken: chords on both modifier sides (#734), a
  twelve-second hold that crosses the #777 typematic bound, fast bursts against slow typing,
  and deliberate idle gaps.
- **Unplugs and re-plugs itself every five minutes.** Overnight that is roughly a hundred
  hot-plug cycles. You managed three by hand after boot 21 and none recovered; a hundred
  attempts is a different experiment.

## What I will read

```
fw-1 HIDTICK[n]: cafe:4b44 slot? ep=0x81 polls=N reports=N arms=N
                 lost=N skipped=N hcevt=N ringfull=N evict=N | mouse polls=N
```

- **the last tag typed vs the last one hype recorded** -- how long input survived, in seconds;
- **whether `reports` stops while `polls` keeps climbing**, and on which tick;
- **whether a self hot-plug ever brings it back.** If even one of a hundred cycles recovers
  the endpoint, that is the strongest lead available: it would mean the endpoint can be
  revived, and by what;
- whether the Keychron and the Pico die together or separately. Boot 21 had both endpoints
  stop on the identical poll, which says shared cause -- two independent devices dying
  together would confirm it, one dying alone would refute it;
- any of `lost` / `ringfull` / `hcevt` / `evict` moving off zero. All were zero in boots 20
  and 21, which is what rules out everything fixed so far.

## Honest status

The eviction defect, the buffer-ownership bug, the ring sizes, the mouse poll rate and the
hub reset are all real and all fixed. None of them is known to be the deafness.

What this run buys is the first dataset with a precise timestamp, a repeatable script, and a
hundred recovery attempts instead of three. If it survives the night, that is worth knowing
but is not proof -- boot 17 survived and boot 18 died on the same build.
