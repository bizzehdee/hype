---
name: diagnose-first
description: The diagnose-on-evidence discipline for hype — get the root cause from real failure output, targeted traces, and working-vs-broken comparison before changing code; back every non-trivial decision with a measurement, not a plausible theory; and state what you measured so the conclusion is checkable. Use whenever something breaks, behaves unexpectedly, or you are about to act on a hypothesis.
---

# Diagnose first, decide on evidence — not assumptions

- **When something breaks or behaves unexpectedly, diagnose it before changing
  anything.** Read the actual failure output, add a targeted trace/probe, compare
  a working path against the broken one — rather than guessing at a cause and
  building a fix on the guess. A fix aimed at an assumed cause usually wastes more
  time than the diagnosis would have taken, and often masks the real bug.
- **Back every non-trivial decision with evidence you actually gathered**, not a
  plausible-sounding theory. If you catch yourself saying "it's probably X," stop
  and get the measurement that confirms or refutes X first. Worked example:
  `.learnings/fw-cfg-string-io.md` — an assumed missing MADT would have been a
  large wrong build; a byte-level fw_cfg trace pointed at a small, different fix.
- **State what you measured and how, so the conclusion is checkable.** Cite the
  log line, the trace, the behavior diff — not just the conclusion. Label an
  untested hypothesis as one.

## Corollaries from real incidents

- A bounded trace reads as *absence of the event* when it overflows — do not read
  silence as proof. Know what a counter actually COUNTS before trusting it.
- Reproduce a scary signature on a known-GOOD run first; a signature consistent
  with success is not a verdict — find the verdict.
- A cluster-boundary-aligned read failure is a structural defect, not a "bad
  block". A read that matches NOTHING on hype's own logs may just need
  `LC_ALL=C grep -a` (the logs are invalid UTF-8), not a "the probe never ran"
  conclusion.

See the `feedback_measure_dont_theorise_318` memory for the fuller catalogue.
