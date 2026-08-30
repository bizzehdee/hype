---
name: board-create
description: How to create a ticket on the hype board — the plan.md gate for anything larger than a bugfix, the ticket body template, milestone and label selection, blocked-by links, sub-issues, and the mandatory verification that the new issue reached the board. Load when raising a new issue.
---

# Creating a ticket — hype

## 1. Check the routing gate first

- A **bugfix** — behavior does not match what `plan.md` or an existing ticket
  already specify — needs no planning detour. Raise the ticket, or point at the
  existing one, and let the work proceed.
- Anything **larger than a bugfix** — a new capability, new config surface, a
  new device or driver, any change beyond restoring the documented spec —
  **must go through `plan.md` first**. The design must be written into the right
  `plan.md` section, and any new fork must be logged as a numbered entry in
  `plan.md` §10 with the alternatives considered.
- If the caller asks for a feature ticket and `plan.md` does not cover it, do
  not create the ticket. Say what is missing from `plan.md` and ask for it to be
  added, or add it if the caller tells you to. Design and tasks must stay in
  sync.

## 2. Check for a duplicate

Search open and closed issues before creating anything:

```sh
gh issue list --repo bizzehdee/hype --state all --search "<keywords>" --limit 50
```

If a closed issue covers the same subject and the work has come back, create a
**new** ticket that references the old number. Never reopen. See `board-status`.

## 3. Write the ticket

One task, one issue. Follow the `doc-style` skill for the body.

- **Title** — the short task name, prefixed with its task ID where the milestone
  uses them (for example `GLADDER-5: Fedora Server, single-VM boot`).
- **Body** — the full description. State the observable problem or the required
  capability, the acceptance condition, and the evidence you already have.
  For a bug: what was observed, on what hardware or rig, and the log excerpt.
- **Milestone** — set the native Milestone field, never a label. Pick by the
  ticket's subject, not by the work you were doing when you found it. Read the
  live list first (`board-gh`); milestone names change over time.
- **Labels** — at least one `kind` label, plus any domain labels that genuinely
  apply. See `board-triage` for the vocabulary.
- **"is blocked by"** — link every ticket this one truly requires, as native
  relationships. Prose such as "split out of #N" leaves the blocked-by gate
  blind.
- **Sub-issues** — use them for a parent/child breakdown (`M4-6` → `M4-6b`),
  never a checklist inside one body.

## 4. Place it on the board and verify

Every new issue must end up on project 3 with **Status = To Do**. No ticket may
exist outside the board.

Repo automation usually does this and sometimes fails silently. After creating
the issue:

1. Read it back: `gh issue view <n> --repo bizzehdee/hype --json projectItems`.
2. If it is not on the project, add it yourself.
3. If Status is empty, set it to **To Do** yourself.

Commands are in `board-gh`.

## 5. Report

State the new issue number, its milestone, its labels, its blocked-by links, and
the confirmed board Status. If you skipped a link because an API call failed,
say so.
