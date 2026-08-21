---
name: task-board
description: How to track work for the hype hypervisor on its GitHub Project board — creating and moving tickets, Status columns, milestones, labels, the feature-vs-bugfix routing rule, and keeping plan.md and the board in sync. Use when creating, updating, closing, or ordering any issue, or when deciding whether new work needs a plan.md change first.
---

# Task board workflow — hype

The **GitHub Project board is the single source of truth for task progress**:
<https://github.com/users/bizzehdee/projects/3>. It has five Status columns —
**To Do**, **Doing**, **On Hold**, **Done**, **Rejected**. Each task is its own
issue in `bizzehdee/hype` (never combine tasks into one ticket). A ticket carries:

- the short task title (e.g. `GLADDER-5: Fedora Server, single-VM …`);
- the full description in the issue body;
- progress/engineering notes as issue **comments**;
- the milestone as the native **Milestone** field (`GLADDER`, `INSTALLER`,
  `VALID`, `VIDEO`, …) — *not* a label;
- dependencies as native **"is blocked by"** relationships (a task is blocked
  by every task it requires);
- parent/child breakdowns (e.g. `M4-6` → `M4-6b` → `M4-6b2`) as **sub-issues**.

`plan.md` is the live design doc; the board is the actionable breakdown of it.

## Task workflow

- **New task** → create a new issue placed in **To Do**, with its full
  description, the right **Milestone**, and honest **"is blocked by"** links to
  whatever it depends on. Don't do undocumented work.
- **Every new issue MUST end up on project 3 with Status = To Do. No ticket may
  exist outside the board.** A ticket missing from it is invisible — it will not
  be picked up, ordered, or counted. Repo automation adds new issues to the
  project and sets To Do, so **verify, don't assume**: after creating an issue,
  check it (`gh issue view <n> --json projectItems`) and add it plus set its
  Status yourself if the automation did not.
- **Starting a task** → move it to **Doing**.
- **Blocked or waiting task** → add a comment saying what it is waiting for,
  then move it to **On Hold**. Anything that has been through **Doing** and is
  now waiting on something outside the working session belongs there: a
  real-hardware run, an intermittent fault to recur, another ticket to land, a
  decision, hardware or media not to hand. It does **not** go back to **To Do**
  (To Do means "not started"). Move it back to **Doing** when the thing it waits
  for arrives.
- **Completed with a positive outcome** → move to **Done** (and close the issue).
- **No longer needed, with no outcome** → move to **Rejected**.
- **Never move a closed issue back to To Do or Doing. Raise a follow-up
  instead.** If work must resume on something already closed — a regression, an
  incomplete fix, a reverted change — create a **new** issue in **To Do** that
  references the original by number and says what brought it back. Leave the
  original closed and in **Done**. Reopening rewrites the record so the
  original's close date and Done state silently become false.
- Reference the task ID (e.g. `M5-3`, `VALID-2`) in commit messages/PRs so the
  dependency graph stays trustworthy.

## Milestones

- **Every milestone name is a single word in ALL CAPS** (`STORAGE`, `CONFIG`,
  `MULTIVM`, `BSD`). No spaces, no hyphens into phrases, no two milestones with
  the same name. The milestone **description** carries the long explanation
  (e.g. `STORAGE = "M10: physical disks and host filesystems"`).
- **A ticket's milestone must match its subject, not where it was found.** A
  config key belongs in `CONFIG`, a media-source defect in `STORAGE`, a disk
  front-end in `VDISK` — even when the work was discovered chasing a bug in
  another area. See `.learnings/milestone-by-subject.md`.
- A ticket whose subject spans several milestones, or fits none, may stay
  unmilestoned — better than a wrong assignment. Say why in the ticket.
- Do not rename or merge milestones ad hoc; a rename touches every ticket in it
  and every doc that names it. Treat it as its own reviewed task.

## Labels

- **Every issue carries at least one label.** Labels are descriptors; the
  milestone stays in the Milestone field, never duplicated as a label.
- One **kind** label per issue, chosen by what the ticket asks for:
  - `bug` — a defect report: something behaves wrongly today;
  - `enhancement` — a new capability or an extension of one;
  - `refactor` — behaviour-preserving restructuring;
  - `spike` — a time-boxed question to answer, not code to deliver;
  - `documentation`, `testing` — when the deliverable is docs or test
    infrastructure itself.
- Plus any **domain** labels that genuinely apply (several are fine):
  `storage`, `config`, `usb`, `networking`, `input`, `diagnostics`,
  `performance`, `guest-compat`, `amd-svm` / `intel-vmx`,
  `hardware-validation` (needs a real-hardware run to close).
- Create a missing label rather than overloading a near-miss, but check
  `gh label list` first — a synonym of an existing label is clutter.

## Feature requests vs. bugfixes

- A **bugfix** (existing behavior doesn't match what `plan.md` or the ticket
  already specify) can go straight to a code change — no planning detour, just
  fix it and move the relevant ticket to **Done** if it wasn't already.
- Anything bigger than a bugfix — new capabilities, new config surface, new
  devices/drivers, or any change to behavior beyond restoring the documented
  spec — **must go through `plan.md` first**: work out the design, log any new
  forks as numbered entries in §10 (with alternatives considered), and update
  whichever `plan.md` section the feature belongs to. Only then does it become
  **a new board ticket** (in **To Do**), assigned to the right Milestone, with
  honest **"is blocked by"** links. Do not add net-new tickets without a
  corresponding `plan.md` change — design and tasks must stay in sync.

## Keeping plan.md and the board in sync

- Task IDs are referenced from commit messages/PRs where practical so the
  dependency graph stays trustworthy.
- Move a ticket to **Done** only when the task is actually done and validated
  per its milestone's testing bar (QEMU + real hardware where applicable) — not
  when the code merely compiles.
- If a change makes a `plan.md` §10 decision obsolete or wrong, update that
  decision's entry (don't delete the history — note what changed and why,
  matching the existing entries' style).

## Keeping user-facing docs in sync

- The top-level `README.md` is written for someone downloading a packaged
  build/installer — no build/toolchain instructions there. Build/toolchain docs
  belong in `fw/README.md` / `tools/` instead.
- Any change that affects what an end user sees or does — install steps,
  supported host/guest OS list, first-run behavior, config file format,
  packaging/installer mechanism — must update `README.md` (and any other
  affected doc) in the same change, not as a follow-up. Treat stale user-facing
  docs the same as a stale ticket or `plan.md`.

## gh limits (see also `.learnings/gh-cli-limits.md`)

- `gh project item-list` silently truncates — pass `--limit 800`.
- The board's **Priority** single-select cannot be read by `item-list`; use a
  batch GraphQL query (~40 ids at a time).
