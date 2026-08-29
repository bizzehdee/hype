# Boot 29 -- does the revive actually bring an endpoint back?

**Run it until input dies, then leave it running another five minutes.** That second part is
the whole experiment.

## Why boot 28 could not answer this

Boot 28 looked like it said the revive fails: hub reports froze at 3,970 across three
revives. It does not say that. A hub's status-change endpoint only reports when a port
CHANGES, so with nothing being plugged in its silence is correct behaviour.

Meanwhile the one endpoint that MUST speak -- the Pico, every ten seconds -- revived zero
times, and your Keychron revived once while you were not typing. Three endpoints and not one
of them under any obligation to report. No conclusion was available.

## What was wrong, and what changed

The silence counter was fixed last time. The CHECK was not: it still sat in the branch
reached only when the shared event ring was empty, so a poll that dequeued another endpoint's
event returned before testing it. A hub reviving in a loop kept the ring busy and starved the
Pico's check completely.

The check now runs on every poll of that endpoint, whatever is on the ring.

## The experiment

The Pico types a tag every ten seconds. So when it goes deaf and a revive fires, there are
exactly two outcomes and no interpretation:

| outcome | meaning | next step |
|---|---|---|
| tags **resume** within ~10 s of a revive | Stop Endpoint + Set TR Dequeue is enough. hype can self-heal, and the machine is usable while the root cause stays open | make the recovery robust and quiet |
| a revive fires and tags **never** resume | the endpoint's ring and context are not where the fault lives | escalate to what a hot-plug does -- Disable Slot, Enable Slot, Address Device, Configure Endpoint -- which boot 23 proved works |

The second outcome is not a failure of the run. It would be the first result all session that
narrows WHERE the bug is rather than only what recovers from it.

## Set-up

Unchanged. Pico in before power-on, same hub as the Keychron. Boot, stay on the dashboard,
press BOOTSEL, confirm `a0001`, type on the Keychron ~30 s, then leave it.

**When input stops, do not intervene and do not power off for at least five more minutes.**
The revive needs 4,000 silent polls -- about 35 seconds -- and then doubles. The evidence is
in what happens after it fires, so ending the run at the moment of death destroys exactly the
data this build exists to collect.

## Watch the dashboard

- **`** LOG FLUSH FAILING **`** means the run will produce no evidence -- restart it.
- **Tags stopping and then resuming on their own.** Note roughly the length of the gap.

## Honest status

The root cause is still unknown. Also still unexplained: boot 26 ran forty-five minutes,
boots 27 and 28 died in three to eight, and nothing between those builds should affect
whether an endpoint survives. I have not chased that and it may matter more than it looks.
