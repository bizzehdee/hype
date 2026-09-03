# Boot AMD-L0 (AMD **laptop**, not the 5950X) -- five minutes for #799

This is the cheapest boot in the queue and it gates the other five. It answers one question:
**where does the AMD laptop's vCPU-0 loop spend its time?** Everything else planned for this
machine needs a guest kernel to reach userspace, and on this laptop it does not.

Build: default, no `EXTRA_CFLAGS`. Active config `\hype.cfg` = `hype1a.cfg` -- one 2-vCPU Alpine
live boot. This is deliberately the **same config and input script** as the 2026-09-03 laptop
attempt that measured 235 us per exit, so the two runs compare directly. Do not "improve" the
config for this boot; a different config makes the comparison worthless.

The laptop is **cold-boot only and has no serial port.** `HYPE.LOG` on the drive is the entire
record. There is nothing to watch on a terminal.

## Before you boot

- Banner reads `build 17785e9-dirty`. That is the expected value: the binary was built at
  `17785e9`, and the commits after it (`564fbcf`) changed only this run card and `stage.sh`, no C
  code. `-dirty` is the `edk2` submodule and an untracked `tools/789/` probe file, neither of
  which is compiled in.
- The active build must NOT carry the APICv marker -- verified at staging time
  (`HYPE_ENABLE_APICV set` absent, `HOUSECOST` present).
- Nothing else needs checking. No spare disks are touched by this config; `target_disk` is a file
  on the drive and exists only to satisfy admission's "needs a disk" check.

## The sequence

1. Cold-boot the laptop from the drive.
2. Stay on the dashboard. **Do not type.** The guest kernel spins from about t=10 s, so the
   evidence is complete long before five minutes are up.
3. After five minutes, power off.
4. Bring back `HYPE.LOG`.

Leaving it longer than five minutes costs machine time and adds nothing: the counters are
cumulative and the shape is set in the first minute.

## What to read

One line, printed next to `LOOPPHASE`:

```
fw-1 HOUSECOST vm0: s2= ... s75=
```

The section holding the mass is the answer. Read it alongside:

```
fw-1 LOOPPHASE: diag= persist= house= dispatch=
fw-1 DRAIN: iters=
```

`house` divided by `iters` is the per-iteration housekeeping cost. Use that denominator, not exit
counts, so the number is comparable with the reference figures below.

| Ticket | Read | Outcome |
| --- | --- | --- |
| #799 | `HOUSECOST vm0: s<N>=` names one dominant section | root cause identified; fix it, then run L1 |
| #799 | the Alpine kernel **does** reach userspace this time | #799 was build- or config-specific, not a property of this laptop; skip straight to L1 and say so on the ticket |

## Reference figures to compare against

| Run | Machine | Housekeeping |
| --- | --- | --- |
| 2026-09-03 laptop attempt (`70d6b0f`) | AMD laptop, 4 cores | `house=77.9 s of 180 s` = **235 us per exit**; kernel never left its early RDTSC delay loops |
| boot AMD-1 run 2 (`447cbb0`, 119 min) | 5950X, 16 cores | `house=92773ms`, `DRAIN: iters=1574688` = **59 us per BSP loop iteration** |
| boot AMD-1 run 1 (`70d6b0f`, 40 min) | 5950X, 16 cores | `house=36587ms`, `iters=1029075` = **36 us** |

So the desktop sits at 36-59 us and the laptop at 235 us. The gap, not the absolute number, is
what #799 is about.

## Result -- boot AMD-L0

_(fill in after the run)_
