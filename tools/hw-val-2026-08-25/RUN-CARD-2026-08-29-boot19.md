# Boot 19 -- a lost completion should no longer cost the keyboard

Boot 18's keyboard was dead from the start. Your log says exactly how dead, and the fix is
structural rather than another attempt to detect it.

## DO NOT UNPLUG THE HYPE DRIVE

`HYPEBOOT` is the boot medium **and** the log medium.

## What boot 18 showed

```
HID[0] 3434:0da4 polls=5779   reports=0  errors=0   (slot4 ep=0x81)   <- your Keychron
MOUSE            polls=390935 reports=0  errors=0   (slot5 ep=0x81)
HID[1] 046d:c547 polls=5779   reports=15 errors=0   (slot5 ep=0x82)   <- same device, works
```

Polled about 114 times a second, no reports, no errors -- on an endpoint whose configuration
was **byte-identical** to boot 16, where the same keyboard reported 1,326 times. Your q-p,
a-l, z-m and 1-0 test confirms it: a scancode problem would lose some keys, not all of them.

The cause is structural. hype kept **one** transfer outstanding per endpoint and re-armed it
only when a completion came back. If that completion went missing there was nothing queued
behind it -- so the endpoint stayed armed, deaf, for the rest of the boot. One lost
completion cost the whole keyboard.

It now keeps **four** outstanding. A lost completion costs one report and the next transfer
carries on. That is what real USB host drivers do.

## Three things I tried first, and why they were wrong

Worth saying, because you have been on the receiving end of two of them:

1. Comparing the controller's dequeue pointer against ours. That field is only valid to read
   when an endpoint is stopped -- so every fault it reported on your machine was imaginary.
   And it printed nine lines from inside the keyboard poll, which **was** your hitching:
   26 ms and 45 ms stalls, measured.
2. A silence timer. An idle keyboard and a dead one look identical for a while.
3. "Has never reported at all." Reset a deliberately idle mouse the first time it ran.

Queuing needs no detector, which is the point.

## What to do

Just use it. No hot-plugging.

1. Boot, wait for the guest login prompt.
2. Type -- slowly, then fast. Does the keyboard work at all, and does it keep working?
3. Hold a key. It should repeat.
4. Chords.
5. **Is the hitching gone?**
6. A few minutes, then power off.

## What I will read

`arms=` is new, beside `polls=` and `reports=` in the HID line. Healthy looks like
`arms = reports + 4` -- four outstanding, the rest retired. `arms=4 reports=0` means the
endpoint never answered at all, which is boot 18's failure and should not recur.

## Open, and not fixed here

One rig fails intermittently on an individual unplug behind a hub (#776) -- the whole-hub
pull works, and the dedicated rig for that cycle passes every time. Not something this boot
tests, since there is no hot-plugging in it.

## Afterwards

    cp \HYPE.LOG \RUN1A.LOG  tools/hw-val-2026-08-25/logs/boot-19/
