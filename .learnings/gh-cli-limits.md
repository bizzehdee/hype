# gh CLI limits on the project board

**Backs:** the gh-limits note in the `task-board` skill.

## What happened

- `gh project item-list` silently truncated at 400 items and hid the Doing / On
  Hold rows. The command returned success and a partial list, so the truncation
  was invisible.
- The board's **Priority** field is a real single-select, but `item-list` cannot
  read it.

## The lesson

- Always pass `--limit 800` (or higher) to `gh project item-list`. A silent
  truncation reads as "that row does not exist."
- To read Priority (or any single-select the item-list omits), use a batch
  GraphQL query, ~40 ids at a time.
- Priority convention on this board: STRETCH / V2 = Low, mainline = Normal,
  blockers = High.
