---
name: task-board
description: Router for hype board work — points at the board-maintainer agent that owns the GitHub Project board, and lists the on-demand board-* skills. Use when creating, updating, closing, or ordering any issue, or when deciding whether new work needs a plan.md change first.
---

# Task board — hype

The **GitHub Project board is the single source of truth for task progress**:
<https://github.com/users/bizzehdee/projects/3>. It has five Status columns —
**To Do**, **Doing**, **On Hold**, **Done**, **Rejected**. Each task is its own
issue in `bizzehdee/hype`. `plan.md` is the live design doc; the board is the
actionable breakdown of it.

## Delegate board work

**All board operations belong to the `board-maintainer` agent.** Hand it the
facts and let it own the board state. Do not run `gh` against the board
yourself.

Delegate: creating a ticket, moving a ticket between columns, triage, labels,
milestones, progress and findings comments, closing or rejecting, and board
audits or summaries.

What the calling agent still must do:

1. Before starting work, make sure a ticket exists and is in **Doing**. If none
   exists, ask `board-maintainer` to create it. Do not do undocumented work.
2. Check the ticket's **"is blocked by"** links are all **Done** before
   starting.
3. Route the work correctly. A **bugfix** — behavior does not match what
   `plan.md` or the ticket already specify — can go straight to a code change.
   Anything bigger must go through `plan.md` first, then become a ticket.
4. Reference the task ID in commit messages and PRs so the dependency graph
   stays trustworthy.
5. Hand `board-maintainer` the evidence when work finishes, blocks, or produces
   a finding.

## The on-demand board skills

`board-maintainer` loads these one at a time, as the job requires:

- `board-create` — raising a ticket, and the `plan.md` gate.
- `board-triage` — labels, milestones, priority, dependencies, duplicates.
- `board-status` — column meanings, the Done bar, the never-reopen rule.
- `board-notes` — progress and findings comments.
- `board-report` — audits, snapshots, summaries.
- `board-gh` — gh/GraphQL recipes, field ids, and the silent-truncation limits.

## Keeping user-facing docs in sync

- The top-level `README.md` is written for someone downloading a packaged
  build or installer. Build and toolchain docs belong in `fw/README.md` and
  `tools/` instead.
- Any change that affects what an end user sees or does — install steps,
  supported host/guest OS list, first-run behavior, config file format,
  packaging or installer mechanism — must update `README.md` in the same
  change, not as a follow-up. Stale user-facing docs are as bad as a stale
  ticket.
- If a change makes a `plan.md` §10 decision obsolete or wrong, update that
  entry. Do not delete the history; note what changed and why.
