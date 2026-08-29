# Boot 26 -- the first build that tries to FIX the deafness

**Five to ten minutes after arming.** Everything before this measured the fault. This one
attempts to undo it.

## Why this build exists

Boot 25 answered the question that eighteen boots could not:

```
Keychron  last report @ poll 70,504 -> probe @ 70,982:  connected=1 enabled=1 SUSPENDED=0
Pico      last report @ poll 31,646 -> probe @ 32,866:  connected=1 enabled=1 SUSPENDED=0
```

At the moment both went permanently silent they were **attached, enabled and awake**. Not
asleep -- so your Keychron's power-saving state is NOT the cause, and it is one fault, not
two. The endpoint was armed, the doorbell rung, no error, no controller event, nothing lost
in software.

And boot 23 showed what fixes it: a hot-plug. Tearing the endpoint down and building it
again brings it straight back.

## What is new

hype now does that itself. When an interrupt-IN endpoint sits armed and silent past a
threshold, it issues **Stop Endpoint** then **Set TR Dequeue Pointer**, restarts the ring,
and re-arms -- the same rebuild a re-plug performs, without the hand.

It applies to HUB status endpoints too, which matters as much as the keyboards: once those
go deaf, hype cannot see a device leave or arrive, and that is why your manual re-plugs never
recovered anything.

## Set-up

Unchanged from boot 25.

1. **Pico in BEFORE powering on**, same hub as the Keychron.
2. Cold boot. Stay on the dashboard.
3. Press BOOTSEL once. Confirm `a0001` appears within ~10 s.
4. Type on the Keychron for ~30 s, then stop and leave both alone.
5. **Leave it five to ten minutes.** Longer is better here than in previous runs: the point
   is to see whether input comes BACK, which takes a revive plus a tag interval.

## What to watch on screen

This is the first run where you may see something happen on its own. If the Pico's tags stop
and then **resume a minute or so later**, that is the fix working, and it is worth noting
roughly how long the gap was.

## What I will read

```
fw-1 HIDTICK[n]: ... reports=N arms=N lost=0 ... revives=N | ...
host-xhci: REVIVE slot=N ep=N -- armed and silent for N polls. Stop Endpoint + Set TR Dequeue
```

| reading | meaning |
|---|---|
| `revives` climbs and `reports` resumes after | **the fix works.** The deafness is recoverable in software and hype can self-heal |
| `revives` climbs and `reports` never resumes | the rebuild is not enough -- the fault is deeper than the endpoint's ring and context |
| `revives` stays 0 while an endpoint is silent | the trigger never fired; the threshold or the armed-check is wrong |
| hub reports resume after a revive | hot-plug detection survives, which is what makes everything else recoverable |

## Honest status

This is a real attempt at a fix, not another measurement -- the first of the session. But it
treats the symptom: it rebuilds an endpoint that has stopped working without knowing WHY it
stopped. If it works, the machine becomes usable while the root cause is still open, and that
is worth having. It is not the same as understanding the bug.

Cost check from the rig: 43 revives fired on idle endpoints with no slow poll over 20 ms, so
the two blocking commands should not produce visible hitching. If you see new stalls, that
matters and is worth reporting.
