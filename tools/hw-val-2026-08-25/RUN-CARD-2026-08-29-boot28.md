# Boot 28 -- the revive trigger, fixed, with a testable prediction

**Run it as long as you can.** Boot 27 finally kept its log; this one should make the revive
actually fire when it is needed.

## What boot 27 showed

Two things, one good and one a bug of mine.

**The log survived.** 1 MB instead of fifteen seconds' worth, `flush_fail_streak=0`, and only
**7** input ticks skipped for the USB lock across the whole run. Serialising the input poll
did its job and did not starve input.

**The revive stopped firing when it was most needed.**

```
Pico   last report @ poll 20,416   final poll 49,048   -> 28,632 polls silent
       revives=1
```

It needed 8,000 silent polls and had 28,632. It should have fired three more times. It did
not, because the counter was incremented only on polls where the whole shared event ring was
empty -- and with eight interrupt-IN endpoints plus hub status endpoints reporting, the ring
is often not empty. A deaf endpoint's silence went almost uncounted.

Silence is now counted once per poll OF THAT ENDPOINT, reset by a report.

## A prediction, so this run can falsify it

From boot 27's own numbers: an endpoint silent for 28,632 polls should now revive at
**4,000**, then **12,000**, then **28,000**, as `revive_after` doubles each time -- **three
revives where boot 27 produced one.**

If `revives` does not scale like that, the trigger is still wrong and I would rather know
than assume.

## Set-up

Unchanged. Pico in before power-on, same hub as the Keychron. Boot, stay on the dashboard,
press BOOTSEL, confirm `a0001`, type on the Keychron ~30 s, then leave it.

## Watch the dashboard

- **`** LOG FLUSH FAILING **`** on the alert line means the run will produce no evidence --
  restart rather than wait it out. It did not appear in boot 27, which is why that log
  survived.
- **Input stopping and then resuming on its own.** That is the revive working. Note roughly
  how long the gap was.

## What I will read

| reading | meaning |
|---|---|
| `revives` climbs and `reports` resume after each | the revive works; the deafness is recoverable in software |
| `revives` climbs, `reports` never resume | the rebuild is not enough -- the fault is deeper than the ring and context |
| `revives` still ~1 per endpoint | the trigger is STILL wrong, and the prediction above is how I will know |

## Honest status

The root cause remains unknown. Also unexplained: boot 26 ran forty-five minutes and boot 27
died before eight, and the only differences between those builds were the lock and the
dashboard alert -- neither of which should affect whether an endpoint survives. That
inconsistency is real and I have not chased it yet.

The rig cannot validate this change: its longest idle gap is about 3,750 polls against a
4,000-poll threshold, so it exercises the revive mechanism but never the trigger rate. Your
hardware is the only place this can be tested.
