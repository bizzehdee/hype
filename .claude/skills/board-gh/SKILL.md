---
name: board-gh
description: Concrete gh CLI and GraphQL recipes for the hype GitHub Project board — ids, field ids, reading and writing Status/Priority/Size, creating issues, linking blocked-by and sub-issues, and the known silent-truncation limits. Load when about to run any command against the board.
---

# Board gh recipes — hype

Constants:

- Owner `bizzehdee`, repo `bizzehdee/hype`, project number `3`.
- Project node id: `PVT_kwHOADIqA84BeGuG`.

Field ids (re-read with `gh project field-list 3 --owner bizzehdee --format json`
if a command rejects one):

| Field | Field id | Option ids |
| --- | --- | --- |
| Status | `PVTSSF_lAHOADIqA84BeGuGzhYjZZU` | To Do `f75ad846`, Doing `47fc9ee4`, On Hold `40a55b03`, Done `98236657`, Rejected `1142c8f0` |
| Priority | `PVTSSF_lAHOADIqA84BeGuGzhYjZqs` | High `79628723`, Normal `0a877460`, Low `da944a9c` |
| Size | `PVTSSF_lAHOADIqA84BeGuGzhYjZq4` | XS `6c6483d2`, S `f784b110`, M `7515a9f1`, L `817d0097`, XL `db339eb2` |

## Known limits — read before trusting output

- `gh project item-list` silently truncates at 400 rows. It returns success and
  a partial list, so the truncation looks like "that row does not exist".
  **Always pass `--limit 800`.**
- `gh project item-list` cannot read single-select fields such as **Priority**.
  Use the GraphQL query below, about 40 ids per batch.
- Never pipe a `gh` command into `head` and judge the result. The pipe hides the
  exit status. Capture the output first, then inspect it.
- Full story: `.learnings/gh-cli-limits.md`.

## Read

List every item with its Status:

```sh
gh project item-list 3 --owner bizzehdee --limit 800 --format json
```

One issue, with its project rows, labels, milestone and links:

```sh
gh issue view <n> --repo bizzehdee/hype \
  --json number,title,state,labels,milestone,projectItems,body
```

Live milestone list (never hardcode milestone names — they change):

```sh
gh api repos/bizzehdee/hype/milestones --paginate \
  -q '.[]|"\(.title) — \(.description // "")"'
```

Live label list:

```sh
gh label list --repo bizzehdee/hype --limit 200
```

Read Priority for a batch of item ids:

```sh
gh api graphql -f query='
query($ids:[ID!]!){ nodes(ids:$ids){ ... on ProjectV2Item {
  id content{... on Issue{number title}}
  fieldValueByName(name:"Priority"){
    ... on ProjectV2ItemFieldSingleSelectValue{name} } } } }' \
  -F ids[]=<id1> -F ids[]=<id2>
```

## Write

Create an issue, then verify it reached the board:

```sh
gh issue create --repo bizzehdee/hype --title "<TITLE>" \
  --body-file <file> --label <kind> --milestone <MILESTONE>
gh issue view <n> --repo bizzehdee/hype --json projectItems
```

Add an issue to the board when automation did not:

```sh
gh project item-add 3 --owner bizzehdee --url https://github.com/bizzehdee/hype/issues/<n>
```

Set Status (get `<item-id>` from `item-list`, not the issue number):

```sh
gh project item-edit --project-id PVT_kwHOADIqA84BeGuG --id <item-id> \
  --field-id PVTSSF_lAHOADIqA84BeGuGzhYjZZU --single-select-option-id <option-id>
```

Comment, label, milestone, close:

```sh
gh issue comment <n> --repo bizzehdee/hype --body-file <file>
gh issue edit <n> --repo bizzehdee/hype --add-label <label> --milestone <MILESTONE>
gh issue close <n> --repo bizzehdee/hype --reason completed   # or: not planned
```

Link a dependency ("is blocked by") — REST, not the CLI:

```sh
gh api -X POST repos/bizzehdee/hype/issues/<n>/dependencies/blocked_by \
  -f issue_id=<blocking-issue-node-or-id>
```

Add a sub-issue:

```sh
gh api -X POST repos/bizzehdee/hype/issues/<parent>/sub_issues \
  -F sub_issue_id=<child-issue-id>
```

If a dependency or sub-issue API call fails, fall back to the issue's web UI
relationship and say in your report that the link is unverified. Do not fake a
link by writing "blocked by #N" in the body only — prose links leave the
blocked-by gate blind.

## After every write

Read the object back (`gh issue view`, or `item-list` filtered to the issue) and
confirm the change is present. Report the verified state, not the command you
ran.
