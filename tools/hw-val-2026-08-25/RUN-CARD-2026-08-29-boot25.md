# Boot 25 -- asleep or deaf, asked of the RIGHT port this time

**Five minutes after arming is enough.** Boot 24 asked the right question of the wrong port.

## What went wrong last time, and what is different

The probe resolved a device to its hub port by matching its ROUTE alone. A route is only
unique within a root port -- two tier-1 hubs both give their port 2 the route 0x00002 -- so
it found the Realtek hub on root port 3 instead of the Genesys on root port 4, and read an
empty port. It reported `connected=0` for a keyboard that was reporting normally.

It now matches the root port too, and prints the resolution it used:

```
fw-1 HIDQUIET[0]: 3434:0da4 rp4 route=0x00002 -> hub slot 2 port 2 status=0x...
```

If `rp4 route=0x00002` does not point at the Genesys hub the Keychron is on, the line says
so itself rather than quietly reading somewhere else. Verified against the rig's inventory.

## What boot 24 established anyway

Your thirty minutes were not wasted. Three things came out of it:

1. **The Pico goes deaf while actively transmitting.** It typed for ~45 s, then nothing for
   136,000 polls -- about twenty minutes -- with every loss counter at zero. It types every
   ten seconds, so this is not idleness. Confirmed twice now.
2. **The hub status endpoints go deaf too**, and once they do, hot-plug recovery becomes
   impossible: hype cannot see a device leave, so it can never re-claim it. That is why your
   manual re-plugs never worked.
3. **Endpoints die at independent times** -- Pico at poll 4,994, Keychron at 61,333,
   receiver at 90,649 -- so this is per-endpoint, not the whole controller.

## Set-up

1. **Pico in BEFORE powering on**, same hub as the Keychron.
2. Cold boot. Stay on the dashboard.
3. Press BOOTSEL once. Confirm `a0001` appears within ~10 s.
4. **Type on the Keychron for ~30 s, then stop and leave both alone.** The pause is the
   point: it is what would let the Keychron sleep, if sleeping is what it does.
5. Leave it five minutes. The Pico died at ~45 s in boot 24, so five is generous.

## The one field that matters

| reading | meaning |
|---|---|
| `SUSPENDED=1` | the device slept, and hype has no resume path -- a SECOND bug, and the likely reason the Keychron's death time tracked how long you paused |
| `SUSPENDED=0`, `connected=1` | awake, attached, and hype cannot hear a running endpoint. The deafness, and boot 23 already showed rebuilding the endpoint fixes it |
| `connected=0` | the device really did leave the bus |

The most useful outcome is a **split**: Keychron `SUSPENDED=1`, Pico `SUSPENDED=0`. That
would mean two separate faults, each with its own fix, and would explain why the evidence has
contradicted itself for seventeen boots.

## Honest status

This does not fix the deafness. It answers one question that everything else waits on.
