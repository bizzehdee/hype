# A boot that never reaches hype is NOBOOT — not a pass or a fail

**Ticket:** #371. **Backs:** the verdict rules in the `microtests` skill.

## What happened

Roughly 1 QEMU boot in 4 never reached hype and left a ~113-byte log that reads
exactly like a guest wedge. Scoring that log as a fail (or, worse, as a pass
because it did not print FAIL) caused two misdiagnoses in one day.

## The lesson

- Gate every boot outcome on the `hype: build` banner.
- Report three distinct outcomes, never two: **ok**, **wedge** (reached hype,
  then hung — a real failure), and **noboot** (never reached hype — an
  environment/harness event, neither pass nor fail).
- A missing verdict is a failure only once you have confirmed the boot reached
  hype. Silence before the banner is NOBOOT.
