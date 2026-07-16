# research/ — archived hardware & specification sources

This directory is the local archive of external primary sources —
vendor developer manuals (AMD APM, Intel SDM), datasheets, and any other
hardware/spec documents consulted while building this hypervisor. The
rule is in `AGENTS.md` ("Hardware/spec research provenance"); the short
version:

**Check order before any web search or download:** (1) the relevant
`task.md` task's summary/notes, then (2) this directory, then — only if
neither has it — (3) the web. The per-task summaries in `task.md` are the
first stop; this directory holds the full documents behind them.

**When a manual/datasheet is fetched:** drop the PDF (or exact source
document) here with a descriptive, versioned filename, add a row to the
table below (what it is, revision, origin URL), and write the specific
facts used — section/table numbers, field offsets, bit meanings, exact
values — into the `task.md` entry it was for, pointing back at the file.

In-tree primary sources (the vendored `edk2/` tree, QEMU headers) are
authoritative for their own formats and are cited by repo path instead;
this archive is only for external documents not already in the repo.

## Archived documents

| File | Document | Revision | Source |
|------|----------|----------|--------|
| _(none archived yet)_ | | | |

## Known external references used (PDF to be archived on next fetch)

These were consulted in earlier work and are cited in `task.md`; the PDFs
were not saved at the time. Archive them here the next time they are
fetched, then move them into the table above.

- **AMD64 Architecture Programmer's Manual, Vol. 2 (System Programming),
  pub. 24593, Rev. 3.44.** Used for the SVM/VMCB work — §15 (SVM: VMRUN,
  #VMEXIT, EVENTINJ/VINTR §15.20/§15.21, intercepted-#PF semantics
  §15.12.15, decode assists, MSRPM/IOPM layout §15.11) and Appendix B
  (VMCB layout / state-save-area field offsets). Cited throughout the
  M2 (SVM), FW-1, CPUMSR, and M4-6b task notes. Fetch:
  https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2
