# Boot 27 -- prove the 45 minutes, with a log that survives to show it

**Run it as long as you did last time.** Boot 26 gave the best result of the project and
almost no evidence for it. This build is aimed squarely at that gap.

## What boot 26 gave us

**Input worked for forty-five minutes**, automated and manual, right up to power-off. Every
previous boot lost it between sixteen seconds and two minutes. That is your observation, not
a counter, and it stands on its own.

**And fifteen seconds of log.** Flushing stopped, nothing said so, and `revives=0` in the
part that survived -- so the recovery had not yet fired at any point we can see. Whether the
revive is WHY input survived is still unknown.

## What is new

1. **The input poll now takes the storage lock.** It has always driven the controller from
   the BSP while guest media reads drive it from AP cores, with only the storage side
   locking. That was harmless while the poll only read the event ring, and stopped being
   harmless when it gained Stop Endpoint and Set TR Dequeue on the shared command ring. This
   is a plausible cause of the dead log, not a proven one.
2. **The dashboard says when the log is dying**, on the alert line above the VM table:

   ```
   ** LOG FLUSH FAILING: 3 in a row (stops at 64) -- \HYPE.LOG is not recording this run **
   ```

   From the FIRST failed flush, not the sixty-fourth. It clears itself if flushing recovers.

## Set-up

Unchanged. Pico in before power-on, same hub as the Keychron. Boot, stay on the dashboard,
press BOOTSEL, confirm `a0001`, type on the Keychron for ~30 s, then leave it.

**Run it for as long as you can -- forty-five minutes again if possible.** The value of this
run is a long one WITH a log.

## Watch the dashboard for two things

- **The alert line.** If `LOG FLUSH FAILING` appears, the log is not recording and the run
  will not produce evidence. Worth restarting rather than waiting it out.
- **Input stopping and then coming back on its own.** If the Pico's tags pause and resume a
  minute or so later, that is the revive working. Note roughly how long the gap was.

## What I will read

| reading | meaning |
|---|---|
| `revives` climbs and `reports` resumes after each | the revive is why input survives. The deafness is recoverable in software |
| `revives` stays 0 across a long healthy run | input survived for some OTHER reason, and one of the earlier fixes is responsible |
| `revives` climbs but `reports` never resume | the rebuild is not enough; the fault is deeper than the ring and context |
| `LOGHEALTH` shows lock misses climbing | the input tick is losing ticks to guest media, and key latency is back by another route |

## Honest status

The root cause is still not understood. Boot 26 may mean the revive works, or it may mean
something else changed and the revive never ran. This run is built to tell those apart, which
is the one thing forty-five good minutes could not.
