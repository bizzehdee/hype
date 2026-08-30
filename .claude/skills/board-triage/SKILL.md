---
name: board-triage
description: How to triage a hype board ticket — the kind/domain label vocabulary, choosing a milestone by subject, priority and size conventions, duplicate handling, and honest blocked-by links. Load when an incoming or existing issue needs classifying, or when auditing tickets for missing fields.
---

# Triaging a ticket — hype

Triage answers five questions for one ticket: what kind of work is it, what
subject does it belong to, what does it depend on, how urgent is it, and is it
already covered elsewhere.

## Labels

Every issue carries at least one label. Labels are descriptors. The milestone
lives in the Milestone field and is never duplicated as a label.

**Exactly one `kind` label**, chosen by what the ticket asks for:

- `bug` — a defect: something behaves wrongly today;
- `enhancement` — a new capability, or an extension of one;
- `refactor` — restructuring that preserves behavior;
- `spike` — a time-boxed question to answer, not code to deliver;
- `documentation` — the deliverable is docs;
- `testing` — the deliverable is test infrastructure.

**Any number of domain labels** that genuinely apply. Read the live list before
choosing (`gh label list --repo bizzehdee/hype --limit 200`). Examples in use
today: `storage`, `config`, `usb`, `networking`, `input`, `diagnostics`,
`performance`, `guest-compat`, `amd-svm`, `intel-vmx`, `security`, `release`,
`hardware-validation` (cannot close without a real-hardware run),
`qemu-validate` (can close on QEMU alone). Treat that list as an example, not as
the full set.

Create a missing label rather than overloading a near-miss one. Check
`gh label list` first — a synonym of an existing label is clutter.

## Milestone

- Set the native Milestone field.
- **Read the live milestone list before assigning.** Milestones are added,
  described, and retired over time. Never work from a list memorised here.
- **Assign by the ticket's subject, not by where the work was found.** A config
  key belongs to the config milestone, a media-source defect to the storage
  milestone, a disk front-end to the virtual-disk milestone — even when the work
  surfaced while chasing a bug somewhere else. Background:
  `.learnings/milestone-by-subject.md`.
- Every milestone name is a single word in ALL CAPS. The long explanation lives
  in the milestone description, not the name.
- A ticket whose subject spans several milestones, or fits none, may stay
  unmilestoned. Say why in the ticket. That is better than a wrong assignment,
  which stays invisible until an audit.
- Do not rename or merge milestones ad hoc. A rename touches every ticket in it
  and every doc that names it. Treat it as its own reviewed task.

## Dependencies

Link every real prerequisite as a native **"is blocked by"** relationship. Add
the reverse view by linking the blocking ticket's follow-ups too, so the graph
reads correctly from both ends. Do not link tickets that are merely related —
a false blocker stalls work.

Use sub-issues for parent/child breakdowns, not for dependencies.

## Priority and Size

- Priority convention on this board: `Low` for stretch or v2 work, `Normal` for
  mainline work, `High` for anything blocking other tickets.
- Priority cannot be read by `gh project item-list`. Use the GraphQL batch query
  in `board-gh`.
- Size is optional. Set it only when the caller gives you an estimate.

## Duplicates

If two tickets describe the same task, keep the one with the better history
(more comments, existing links), label the other `duplicate`, comment with the
surviving number, close it as not planned, and move it to **Rejected**.

## Report

List each ticket you triaged with what you set: kind label, domain labels,
milestone, priority, and any links added. Name any ticket you deliberately left
unmilestoned and why.
