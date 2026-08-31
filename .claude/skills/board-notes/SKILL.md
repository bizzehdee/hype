---
name: board-notes
description: How to write progress notes and findings comments on hype tickets — what a useful comment contains, evidence over conclusions, the required note when a ticket goes On Hold, and how to record a real-hardware run. Load when adding a comment to a ticket that is being worked.
---

# Ticket comments — hype

Engineering notes live as issue **comments**. The issue body describes the task
and stays stable. Comments carry what happened.

Follow the `doc-style` skill: short sentences, active voice, one idea per
sentence, no marketing language.

## What a comment must contain

Write comments for a reader who arrives in six months with no memory of the
session.

1. **What was done** — the change, the run, or the check. Be specific.
2. **The evidence** — the command, the counter, the log excerpt, the exit code.
   Quote the real output. A conclusion without evidence is a guess, and the
   `diagnose-first` skill forbids acting on one.
3. **What it means** — the conclusion drawn from that evidence, marked as a
   conclusion.
4. **What happens next** — the next action, or the thing being waited for.

Keep log excerpts short. Quote the lines that carry the signal, not the whole
capture.

## The evidence is the data, never a path to it

**Cite the lines and the numbers. A file path is not evidence.**

A comment that says "see `tools/hw-val-2026-08-25/logs/boot-39/HYPE.LOG`" has
recorded nothing. The reader six months from now has a different machine, the
run logs are not in the repository, and even where a path still resolves it puts
the burden of finding the signal back on them — after the person who already
found it decided not to write it down.

So put the actual lines in the comment:

```
fw-1 APVCPU vm0/1: live=1 exits=6851069122
fw-1 PERF: elapsed=10594122ms hlt_wait=9653914ms (91%)
```

and the arithmetic that turns them into the claim — 6.85 billion exits over
10,594 seconds is 647k/s, against the 118k/s this ticket was opened on. Then
the comment is readable on its own, forever, by someone with no access to
anything.

A run identifier (which boot, which machine, which build sha) still belongs in
the comment — that is provenance, and it says where the numbers came from. It is
not a substitute for the numbers.

The same rule applies to a passing test: quote the assertion that passed and the
value it saw, not the name of the script that ran it.

## Required comments

- **Going On Hold** — always comment first, saying exactly what the ticket waits
  for and what will unblock it. A ticket in On Hold with no reason is dead
  weight.
- **A real-hardware run** — record the machine, the build identity (banner sha
  and any `EXTRA_CFLAGS` echoed), the outcome, and **the lines that show it**.
  Each cold boot is expensive; the note must be good enough that nobody repeats
  the boot to re-learn the result — which means the numbers are in the comment,
  not in a file the next reader has to go and find.
- **Closing as Done** — state the validation that justifies Done.
- **Closing as Rejected** — state why the task is no longer needed.
- **A negative result** — a theory that was tested and disproved is worth as
  much as a fix. Record it so nobody retests it.

## What not to write

- Do not restate the issue body.
- Do not report progress with no evidence ("still working on it").
- Do not paste an unfiltered multi-thousand-line log.
- Do not cite a log file INSTEAD of the data in it. Run logs are not in the
  repository (`.gitignore`), so a path is a dead reference the moment the
  session that wrote it ends.
- Do not record a conclusion you did not measure.
- Do not include secrets, credentials, or personal data.

## Cross-references

Reference other tickets by number so the graph stays navigable. If the comment
records a genuine dependency, add a native "is blocked by" link as well — prose
alone leaves the gate blind (see `board-triage`).

Commands are in `board-gh`. Write long comment bodies to a file and pass
`--body-file`; shell quoting mangles multi-line text.
