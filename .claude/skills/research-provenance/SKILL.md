---
name: research-provenance
description: How to source and archive hardware/spec research for hype — vendor manuals (AMD APM, Intel SDM), datasheets, device register layouts, on-the-wire formats, errata. Use before any web search or download for a spec fact, and whenever you fetch or cite a manual.
---

# Hardware/spec research provenance

Any research against a vendor developer manual (AMD APM, Intel SDM, a
datasheet) — or hardware/spec research in general (a device register layout, an
on-the-wire format, an errata) — must be archived so it is never re-fetched from
the web.

- **Check order, always, before any web search or download:**
  1. the relevant ticket's description/comments;
  2. the `research/` directory;
  3. only if neither has it — the web.

  Reaching for a web search or download first is a process error; the answer is
  usually already captured.
- **When you do fetch a manual/datasheet:** save the PDF (or the exact source
  document) under `research/` with a descriptive, versioned name (e.g.
  `research/amd-apm-vol2-24593-r3.44.pdf`), and record in `research/README.md`
  what it is, its version/revision, and where it came from.
- **Capture the extract against the task:** in the ticket(s) the research was
  for, write the specific facts used — section/table numbers, field offsets, bit
  meanings, exact values — as a short summary with a pointer to the archived file
  (`research/<file>`, §/table). These per-task summaries are the first thing the
  next agent reads, so make them self-sufficient: enough to act on without
  re-opening the PDF.
- Prefer in-tree primary sources when they exist (the vendored `edk2/` and QEMU
  headers are authoritative for their own formats) and cite the file path the
  same way; the `research/` archive is for external documents not already in the
  repo.
