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

## Copyright

The manuals archived here are **copyright of their respective owners**
(AMD, Intel) and are redistributed by them for developer reference. They
are kept in this directory only as an offline engineering reference for
building this project; they are not part of the project's own GPLv3
source and their copyright/licensing is unchanged by inclusion here. Do
not treat them as project-licensed material.

## Archived documents

| File | Document | Revision | Source |
|------|----------|----------|--------|
| `24593_3.44_APM_Vol2.pdf` | AMD64 Architecture Programmer's Manual, Vol. 2 — System Programming (SVM/VMCB) | pub. 24593, Rev. 3.44 | https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2 |
| `325462-092-sdm-vol-1-2abcd-3abcd-4.pdf` | Intel® 64 and IA-32 Architectures Software Developer's Manuals — combined volume set (Vol. 1, 2ABCD, 3ABCD, 4) | order 325462, rev. 092 | https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html |

### Key extracts captured against tasks

- **AMD APM Vol 2 (`24593_3.44_APM_Vol2.pdf`).** SVM/VMCB work — §15 (SVM:
  VMRUN, #VMEXIT, EVENTINJ/VINTR §15.20/§15.21, intercepted-#PF semantics
  §15.12.15, decode assists, MSRPM/IOPM layout §15.11) and Appendix B
  (VMCB layout / state-save-area field offsets). Cited throughout the M2
  (SVM), FW-1, CPUMSR, and M4-6b task notes.
- **Intel SDM (`325462-092-sdm-vol-1-2abcd-3abcd-4.pdf`).** The Intel-host
  counterpart reference (VMX/VT-x, IA-32 system programming) for the
  mandatory Intel real-hardware validation pass (AGENTS.md testing gate);
  cite the specific volume/§ against the task when used.
