---
name: board-status
description: The Status column rules for the hype board — what To Do, Doing, On Hold, Done and Rejected each mean, the bar a ticket must clear to reach Done, when to use On Hold, and the never-reopen rule with its follow-up ticket procedure. Load when moving, closing, or rejecting a ticket.
---

# Moving a ticket — hype

Five columns. Each has one meaning. Do not stretch them.

| Column | Meaning |
| --- | --- |
| **To Do** | Not started. |
| **Doing** | Someone is working on it now. |
| **On Hold** | Started, now waiting on something outside the working session. |
| **Done** | Finished with a positive outcome, and validated. Issue closed. |
| **Rejected** | No longer needed, with no outcome. Issue closed. |

## Transitions

- **Starting work** → move to **Doing**. Do this before the work starts, not
  after it finishes.
- **Blocked or waiting** → add a comment saying exactly what it waits for, then
  move to **On Hold**. Anything that has been through Doing and now waits on
  something outside the session belongs there: a real-hardware run, an
  intermittent fault to recur, another ticket to land, a decision, or hardware
  or media not to hand.
  **On Hold is not To Do.** To Do means "not started". A started ticket must
  never go back to To Do.
- **The thing it waited for arrived** → move from On Hold back to **Doing**.
- **Finished and validated** → move to **Done** and close the issue as
  completed.
- **No longer needed** → move to **Rejected** and close the issue as not
  planned. Comment with the reason first.

## The bar for Done

Move a ticket to Done only when the task is actually done and validated against
its milestone's testing bar — QEMU, plus a real-hardware run where the ticket
carries `hardware-validation`. "The code compiles" is not Done. "The tests pass
locally" is not Done for a ticket that needs hardware.

If you are told a ticket is finished but you cannot see the validation evidence,
ask before moving it. Do not guess.

## Never reopen

**Never move a closed issue back to To Do or Doing.** Never reopen one.

If work must resume on something already closed — a regression, an incomplete
fix, a reverted change — then:

1. Create a **new** issue in **To Do**.
2. Reference the original by number and state what brought the work back.
3. Link it to the original with a native relationship.
4. Leave the original closed and in **Done**.

Reopening rewrites the record. The original's close date and Done state
silently become false.

## Verify the move

`gh project item-edit` reports success without proving the field changed. After
every move, re-read the item and confirm the Status option. Commands and field
ids are in `board-gh`.

## Report

State each transition as `#<n> <from> → <to>`, plus the close reason where you
closed an issue.
