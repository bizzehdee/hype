# Measure before deciding: the MADT assumption vs. the fw_cfg trace

**Backs:** the "diagnose first, decide on evidence" rules in `AGENTS.md`.

## What happened

A guest firmware problem was diagnosed by assumption: "the MADT must be missing,
so build MADT synthesis." That would have been a large, wrong piece of work.

A byte-level trace of OVMF's fw_cfg probe told a different story: it was reading
**one byte instead of four**. The real defect was in string-I/O emulation, not
in ACPI table synthesis. The measurement pointed at a completely different, much
smaller fix.

## The lesson

- "It's probably X" is a signal to stop and get the measurement that confirms or
  refutes X, not a license to start building the fix for X.
- A byte-level trace of the actual transaction beats any plausible theory about
  the layer above it.
- State what you measured and how, so the conclusion is checkable: cite the log
  line or trace (one byte vs. four), not just the root cause.
