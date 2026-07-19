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

## Archived wiki exports

| Source | Snapshot | Derived Markdown | Notes |
|------|----------|----------|------|
| `OSDev+Wiki-20260719190820.xml` | 2026-07-19 19:08:20 | `osdev-wiki/` | One Markdown file per non-template article; exported templates are expanded into their use sites. |

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

## Online reference links (external, not archived)

Code/spec links gathered for reference. Not downloaded into this tree (code
repos under their own licenses; the Intel PDFs are superseded by the archived
combined SDM above). Ratings are relative to hype's actual surfaces (AMD SVM
host; guest device models + MMIO decode; the Intel-VMX path is future work).

| Link | What it is | Usefulness to hype |
|------|------------|--------------------|
| http://www.intel.com/Assets/PDF/manual/253669.pdf | Intel SDM Vol. 3B (system programming, incl. APIC) | ★★ now — the **APIC/LAPIC-timer/IPI** chapter backs M8-0b inc 5 (AP LAPIC timer) and the `sysvec_call_function` spin lead. Superseded by the archived combined SDM (325462); use that copy. |
| http://www.intel.com/Assets/PDF/manual/253667.pdf | Intel SDM Vol. 2B (instruction set reference) | ★★ — instruction encoding, cross-checks `mmio_decode.c`. Also in the archived combined SDM. |
| http://lxr.free-electrons.com/source/arch/x86/kvm/vmx.c | KVM VMX implementation (GPLv2) | ★ future — reference for the Intel-VMX ops path (currently a stub). NOT for SVM (AMD APM is the SVM authority). License: GPLv2 — read for understanding, don't copy into GPLv3-with-care. |
| http://bochs.cvs.sourceforge.net/viewvc/bochs/bochs/cpu/vmx.cc | Bochs VMX emulator (LGPLv2) | ★ future — clean, readable VMX behavior reference for the Intel path. |
| https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/vmx_pio.c | ZeldaOS PIO exit sub-handler | ★★ — small VMM's port-I/O dispatch; cross-check for hype's IOIO handling + the **spin investigation** (guest polling a mis-modeled port). |
| https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/vmx_instruction_decoding.c | ZeldaOS MMIO mov decode | ★★ — direct comparison for `arch/x86_64/cpu/mmio_decode.c`. |
| https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/device_8259pic.c | ZeldaOS 8259 PIC model | ★★ — cross-check `devices/pic.c` (esp. spurious-IRQ / ISR-read behavior). |
| https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/device_8253pit.c | ZeldaOS 8253 PIT model | ★★★ — directly relevant to the **spin/timer investigation**: compare channel counting + calibration behavior against `devices/pit.c`. |
| https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/device_keyboard.c | ZeldaOS 8042 keyboard model | ★ — cross-check `devices/ps2_keyboard.c`. |
| https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/device_serial.c | ZeldaOS 16550 serial model | ★ — cross-check `devices/guest_uart.c`. |
| https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/device_video.c | ZeldaOS 16-color video (MMIO) | ✩ low — hype uses GOP/ramfb, not legacy 16-color text MMIO. |

Note on licenses: the KVM (GPLv2) and Bochs (LGPLv2) sources are for
*understanding*, not copy-paste — hype is GPLv3 and its device/decode logic is
written fresh from the primary specs. ZeldaOS (check its repo license) is a
useful "how another small VMM structured this" comparison, same rule.
