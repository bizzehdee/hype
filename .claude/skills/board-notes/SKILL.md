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
capture. Name the file the full capture is in.

## Required comments

- **Going On Hold** — always comment first, saying exactly what the ticket waits
  for and what will unblock it. A ticket in On Hold with no reason is dead
  weight.
- **A real-hardware run** — record the machine, the build identity (banner sha
  and any `EXTRA_CFLAGS` echoed), the outcome, and where the log is archived.
  Each cold boot is expensive; the note must be good enough that nobody repeats
  the boot to re-learn the result.
- **Closing as Done** — state the validation that justifies Done.
- **Closing as Rejected** — state why the task is no longer needed.
- **A negative result** — a theory that was tested and disproved is worth as
  much as a fix. Record it so nobody retests it.

## What not to write

- Do not restate the issue body.
- Do not report progress with no evidence ("still working on it").
- Do not paste an unfiltered multi-thousand-line log.
- Do not record a conclusion you did not measure.
- Do not include secrets, credentials, or personal data.

## Cross-references

Reference other tickets by number so the graph stays navigable. If the comment
records a genuine dependency, add a native "is blocked by" link as well — prose
alone leaves the gate blind (see `board-triage`).

Commands are in `board-gh`. Write long comment bodies to a file and pass
`--body-file`; shell quoting mangles multi-line text.
