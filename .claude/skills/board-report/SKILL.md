---
name: board-report
description: How to audit the hype board and summarise findings — building a trustworthy snapshot despite silent truncation, the standard audit checks for missing labels, milestones and Status, and the report format. Load when asked what is on the board, what is in flight, or to summarise findings across tickets.
---

# Board reports and audits — hype

A board report is only useful if the underlying read is complete. `gh project
item-list` truncates silently at 400 rows and returns success. Always pass
`--limit 800`, and state the row count you actually read in the report.

## Building a snapshot

1. Read every item: `gh project item-list 3 --owner bizzehdee --limit 800
   --format json`. Save the JSON to a file and work from the file.
2. Compare the row count against the project item total from
   `gh project view 3 --owner bizzehdee --format json`. If the numbers differ,
   the read was truncated. Raise the limit and read again.
3. Priority is not in that output. Fetch it with the GraphQL batch query in
   `board-gh`, about 40 ids per batch, only if the report needs it.
4. Read the live milestone and label lists rather than assuming which exist.

## Standard audit checks

Run these when asked to audit, or before a milestone review:

- Issues that are open but not on project 3.
- Board items with an empty Status.
- Issues with no `kind` label.
- Issues with no milestone, and no stated reason for having none.
- Tickets in **Doing** with no comment in the last working session — either
  stale, or they belong in **On Hold**.
- Tickets in **On Hold** with no comment saying what they wait for.
- Tickets in **Done** that are still open, or closed issues not in Done or
  Rejected.
- Tickets whose "is blocked by" links are all Done but which are still in
  To Do — these are ready to start.
- Tickets blocked by a **Rejected** ticket — the dependency will never land.

Report each finding with the issue numbers. Do not fix anything during an audit
unless the caller asked you to; list the proposed fixes instead.

## Summarising findings across tickets

When the caller hands you engineering findings to record:

1. Group the findings by the ticket each belongs to.
2. Put each finding on its own ticket as a comment (`board-notes`).
3. Raise a new ticket for any finding that is not covered by an existing one
   (`board-create` — check the `plan.md` gate first).
4. Only then write the summary back to the caller.

Never leave a finding only in the summary. A finding that is not on a ticket is
lost.

## Report format

Follow the `doc-style` skill. Lead with the answer. Use a table when the report
lists more than about five tickets:

```
| # | Title | Status | Milestone | Note |
```

State explicitly:

- how many items you read, and whether the read was complete;
- what you changed, as `#<n> <from> → <to>` lines;
- what you did not change and why.

Do not pad the report with tickets the caller did not ask about.
