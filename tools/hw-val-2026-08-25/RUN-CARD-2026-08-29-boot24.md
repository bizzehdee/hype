# Boot 24 -- is it asleep, or is it deaf?

**Five minutes after arming is enough. Fifteen if you can spare it.** This run asks one
question, and the answer is a single field in the log.

## The question

An endpoint that stops reporting is either a device with nothing to say or a device hype can
no longer hear, and every previous boot has been unable to tell those apart. The PORT can,
and it answers over a control transfer, so it still answers when the endpoint is dead.

Two things you told me made this the right question:

- **Your Keychron has an idle power-saving state.** It also advertises Remote Wakeup, and
  hype has NO resume path -- so if it sleeps, it stays asleep until physically re-plugged.
- **The Pico went deaf while still typing** (boot 23). It types every ten seconds, so it
  never idles long enough to sleep. That case cannot be power saving.

So there may be TWO bugs wearing one symptom, and this run separates them.

## Set-up

1. **Plug the Pico in BEFORE powering on.** Boot 23 showed the boot walk enumerates
   behind-hub devices over control transfers, so it does not need the hub status endpoint --
   which is why hot-plugging it late in boot 22 was never seen. Same hub as the Keychron.
2. Cold boot from this stick. Wait for the dashboard. Stay on the dashboard, do not switch
   to a guest -- that is what boots 20 to 23 ran.
3. **Press BOOTSEL on the Pico once.** LED blinking to solid.
4. Confirm `a0001` appears on screen within ~10 s, then `a0002` ten seconds later. That is
   your proof it is claimed; you cannot read the log while hype is running.
5. **Type on the Keychron for thirty seconds or so, then stop and leave both alone.**
   The pause is deliberate: it is what would let the Keychron sleep.
6. Leave it at least five minutes after arming. Fifteen covers two Pico self-hot-plug cycles.

## What the log will say

After ~4 s of quiet on any endpoint that had been reporting, hype reads its hub port and
logs one line:

```
fw-1 HIDQUIET[n]: 3434:0da4 hub slot 2 port 2 status=0xXXXX --
    connected=1 enabled=1 SUSPENDED=? reset=0 | reports=N polls=N
```

| reading | meaning | what it means for the fix |
|---|---|---|
| `SUSPENDED=1` | the device slept | hype needs suspend/resume: detect the resume and wake the port. A NEW bug, and the likely reason the Keychron's death time varied with how long you paused |
| `SUSPENDED=0` | awake, connected, and hype cannot hear it | the real deafness. Boot 23 showed a hot-plug rebuilds the endpoint and it works again, so the fix is endpoint recovery -- Stop Endpoint, Set TR Dequeue, re-arm |
| no port found | the route did not resolve | a bug in the probe itself, not in USB |

The most valuable outcome is a **split**: the Keychron `SUSPENDED=1` and the Pico
`SUSPENDED=0`. That would confirm two separate faults and give each its own fix.

## Also in this build

Nothing else changed since boot 23. The probe is one control transfer per quiet spell,
capped at 64 for the whole run, and logs a measurement rather than a verdict -- the last two
detectors judged, and both called normal behaviour a fault.

## Honest status

The deafness is still unexplained. This run does not fix it. What it does is turn the
central ambiguity of the last sixteen boots into a field you can read.
