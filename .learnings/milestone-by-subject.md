# A ticket's milestone follows its subject, not where it was found

**Backs:** the milestone rule in the `task-board` skill.

## What happened

Before the 2026-08-06 audit, the `BSD` milestone held 19 tickets that had
nothing to do with BSD. They were filed under `BSD` because the work was
discovered while chasing a FreeBSD bug — the milestone recorded the working
context, not the ticket's subject.

## The lesson

Assign a ticket's milestone by what the ticket asks for:

- a config key belongs in `CONFIG`;
- a media-source defect belongs in `STORAGE`;
- a disk front-end belongs in `VDISK`.

A ticket whose subject spans several milestones, or fits none, may stay
unmilestoned and say why. That is better than a wrong assignment, which is
invisible until an audit.
