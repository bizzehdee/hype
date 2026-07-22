# Agent Rules — hype

This repo builds a thin UEFI type-1 hypervisor. Full design lives in
[`plan.md`](plan.md); the actionable, dependency-tracked breakdown of that
design lives on the **GitHub Project board** (see "Task tracking" below).
Read `plan.md` before making non-trivial changes — this file is the condensed
rule set, not a replacement for it.

## Task tracking

The **GitHub Project board is the single source of truth for task progress**:
<https://github.com/users/bizzehdee/projects/3>. It has four Status columns —
**To Do**, **Doing**, **Done**, **Rejected**. Each task is its own issue in
`bizzehdee/hype` (never combine tasks into one ticket). A ticket carries:

- the short task title (e.g. `GLADDER-5: Fedora Server, single-VM …`);
- the full description in the issue body;
- progress/engineering notes as issue **comments**;
- the milestone as the native **Milestone** field (`GLADDER`, `M4`, `VALID`,
  `VIDEO`, …) — *not* a label;
- dependencies as native **"is blocked by"** relationships (a task is blocked
  by every task it requires);
- parent/child breakdowns (e.g. `M4-6` → `M4-6b` → `M4-6b2`) as **sub-issues**.

Every task and its engineering notes live as board tickets (migrated from the
old in-tree task list, whose history remains in git). `plan.md` remains the
live design doc.

### Task workflow

- **New task** → create a new issue placed in **To Do**, with its full
  description, the right **Milestone**, and honest **"is blocked by"** links to
  whatever it depends on. Don't do undocumented work.
- **Starting a task** → move it to **Doing**.
- **Parked task** → add a comment saying why it's parked, then move it back to
  **To Do**.
- **Completed with a positive outcome** → move to **Done** (and close the
  issue).
- **No longer needed, with no outcome** → move to **Rejected**.
- Reference the task ID (e.g. `M5-3`, `VALID-2`) in commit messages/PRs so the
  dependency graph stays trustworthy.

## Before doing anything

1. Find the board ticket covering the work. If none exists, create one (in
   **To Do**, per the workflow above) before starting — don't do undocumented
   work. Move the ticket you're starting to **Doing**.
2. Check that the ticket's **"is blocked by"** links are all **Done**. If they
   aren't, either do them first or stop and ask — do not skip ahead on the
   assumption a prerequisite "probably doesn't matter yet."
3. If the work touches something `plan.md` §10 already decided, follow that
   decision. If you think a decision is wrong, say so and get it changed in
   `plan.md` §10 — don't silently diverge from a documented decision in code.
4. If the work surfaces a genuinely new decision (a fork not already
   covered by §10), resolve it and add it to `plan.md` §10 as a new
   numbered entry before writing the code that depends on it.

## Diagnose first, decide on evidence — not assumptions

- **When something breaks or behaves unexpectedly, diagnose it before
  changing anything.** Get the root cause early and cheaply — read the
  actual failure output, add a targeted trace/probe, compare a working path
  against the broken one — rather than guessing at a cause and building a fix
  on top of the guess. A fix aimed at an assumed cause usually wastes more
  time than the diagnosis would have taken, and often masks the real bug.
- **Back every non-trivial decision with evidence you actually gathered**,
  not with a plausible-sounding theory. "The MADT must be missing, so build
  MADT synthesis" is an assumption; a byte-level trace showing OVMF's fw_cfg
  probe reading one byte instead of four is evidence — and it pointed at a
  completely different fix (string-I/O emulation). If you catch yourself
  saying "it's probably X," stop and get the measurement that confirms or
  refutes X first.
- **State what you measured and how, so the conclusion is checkable.** When
  you report a root cause or close a task, cite the observation that proves
  it (the log line, the trace, the diff in behavior), not just the
  conclusion. If a belief is still an untested hypothesis, label it as one.
- This is the same measure-first discipline the testing and real-hardware
  gates enforce; it applies to debugging and design calls too, not only to
  merging code.

## Hard invariants — do not weaken these without updating plan.md §10 first

- **The host↔guest and guest↔guest security boundaries are paramount —
  above performance, features, or convenience.** Nothing may cross either
  boundary unintentionally. The host must never expose its own state, memory,
  or hardware to a guest except through a deliberately designed, mediated
  interface; one guest must never be able to observe or affect another guest
  (its memory, its I/O, its timing side-channels, shared emulation state that
  should be per-VM) except where the operator has *explicitly* configured a
  channel between them. Intentional, configured inter-VM or external
  communication (e.g. `net_peers` networking, or VMs talking over a real
  network) is fine — the rule is against *unintentional* leakage, not against
  designed communication. When in doubt, treat a potential cross-boundary
  path as a leak and prove it isn't before relying on it; a performance or
  simplicity win that erodes a boundary is not a win (see the rejected
  port-0x80 passthrough and the per-vCPU de-globalization work for why
  file-global emulation state is a guest↔guest leak).
- **Guest isolation is the point of this project.** Every one of these
  exists because of `plan.md` §6g/§6j/§10's security-review decisions
  (#19–22):
  - Every device-emulation code path that touches a guest-supplied address,
    offset, or length (virtio descriptors, AHCI/NVMe command buffers, block
    I/O LBA+count) **must** validate it against that specific VM's own
    EPT/NPT-mapped range and the backing resource's real size before the
    host dereferences it or performs the corresponding I/O. No raw guest
    pointer is ever trusted directly. This is the actual guest-escape
    vector — EPT/NPT alone does not prevent it.
  - No two VMs' `cpu_set` ranges, `target_disk` paths, or varstore files may
    overlap/collide — enforced at startup admission control, not left as an
    assumption.
  - Guest-to-guest networking is default-deny; a pairing is only allowed
    when explicitly named via `net_peers` in `hype.cfg`, validated at
    startup. Never make guest-to-guest traffic possible as a side effect of
    how NAT/switching happens to be implemented.
  - A misbehaving/faulted guest is torn down alone (Force power off) — never
    a hypervisor-wide halt or reset in response to one guest's fault.
  - A fault-isolation watchdog catches hangs/anomalies; it is **not** a
    substitute for the input-validation rule above.
- **1:1 exclusive vCPU-to-pCPU pinning.** No shared pCPU between two VMs,
  ever — this is what the fault-isolation guarantee depends on.
- **Guest RAM is zeroed before first execution**, on every (re)start,
  including after Force power off — never reused as-is.
- **No guest gets direct hardware access.** Physical disk/NIC access is
  always mediated through a host-side driver plus an emulated guest-facing
  frontend — never PCI passthrough or guest-initiated DMA to real hardware.
  This is why v1 needs no IOMMU; don't add passthrough without revisiting
  that.
- **Destructive writes to a `physical:` target disk require**: serial/GUID
  match confirmed at VM start, an interactive dashboard confirmation before
  the first write, and a non-empty-partition-table guard — a `physical:`
  config entry alone must never be sufficient to trigger a wipe.

## Toolchain & language

- `hype.efi` itself: **C**, freestanding, targeting `x86_64-unknown-uefi`,
  built with the lightweight clang/lld-or-GNU-EFI pipeline — not EDK2, not
  Rust (`plan.md` §8, §10 decision #17). No libc.
- The guest firmware blob is a separate concern, built via EDK2, vendoring a
  stripped OVMF (`plan.md` §10 decision #1). Don't conflate the two build
  pipelines.
- Every device-emulation and host-driver module runs at the most privileged
  level with no OS underneath and no process boundary to contain a bug —
  code review here should weigh a missed bounds check as a full-machine
  compromise, not a crash.

## License

- Project license is **GPLv3**. Any third-party code adapted in (e.g. the
  AHCI/NVMe or NIC host drivers) must be GPLv3-*compatible*: MIT/BSD,
  Apache, GPLv3, or "GPLv2-or-later" are fine to pull in and relicense.
  **Plain GPLv2-only code is not GPLv3-compatible** — check the specific
  file/module's license header before adapting anything, not just the
  source project's overall stated license.

## Testing

- QEMU/KVM nested virtualization (`-cpu host,+vmx`) for fast iteration.
- A **mandatory real-hardware validation pass** (both Intel and AMD, per
  `plan.md` §10 decision #18) at every milestone gate — QEMU alone is
  necessary but not sufficient; nested VMX/SVM emulation doesn't faithfully
  reproduce every edge case.

### Unit testing

- **Unit testing is a core requirement, not optional, on all testable
  code.** "Testable" means anything expressible as pure(-ish) logic that
  doesn't require actual privileged CPU state, real hardware, or a running
  hypervisor to exercise — this covers more of the codebase than it might
  first appear: the `hype.cfg` parser, all of §6i's admission-control
  checks (memory/vcpu/`cpu_set` overlap, `target_disk`/varstore uniqueness,
  `net_peers` validation), the §6j guest-address bounds-checking logic,
  `blk_backend` LBA/length validation, ACPI table synthesis, the per-vCPU
  watchdog's fault-classification logic, and the host power-lifecycle
  state-record read/write logic.
- **90% line/branch coverage is the floor**, not a target to approach, on
  every testable module. Falling short blocks the change — treat it the
  same as a failing build.
- Code that genuinely can't be unit tested (VMXON/VMCS/VMCB setup, inline
  asm, VM-exit trampolines, real MMIO/PIO register access, anything that
  only makes sense with actual CPU privilege transitions) is exempt, but
  **the exemption is for the hardware-touching shim only** — write that
  shim as thin as possible and push the actual decision logic behind it
  into a plain, testable function. E.g. "decode this VM-exit reason and
  decide what to do" should be a pure function fed a struct of exit info,
  unit tested directly; only the few lines that read the real VMCS/VMCB
  fields into that struct are exempt. Don't use "it touches hardware
  somewhere in the call stack" to excuse an entire module from coverage.
- New code that isn't unit tested where testable, or that drops a module's
  coverage below 90%, doesn't get merged — this is enforced the same way
  as the real-hardware validation gate above, not treated as a nice-to-have
  cleanup for later.

## Feature requests vs. bugfixes

- A **bugfix** (existing behavior doesn't match what `plan.md` or the ticket
  already specify) can go straight to a code change — no planning detour
  needed, just fix it and move the relevant ticket to **Done** if it wasn't
  already.
- Anything bigger than a bugfix — new capabilities, new config surface, new
  devices/drivers, or any change to behavior beyond restoring the documented
  spec — **must go through `plan.md` first**: work out the design, log any
  new forks as numbered entries in §10 (with alternatives considered, same
  style as the existing entries), and update whichever `plan.md` section the
  feature belongs to. Only after that's settled should it become **a new
  board ticket** (in **To Do**), assigned to the right Milestone, with honest
  **"is blocked by"** links to whatever existing tasks it actually depends on
  (and updating any downstream tickets' links if the new work now sits in
  front of them). Do not add net-new tickets without a corresponding
  `plan.md` change to justify them — design and tasks must stay in sync.

## Keeping `plan.md` and the board in sync

- Task IDs are referenced from commit messages/PRs where practical
  (e.g. `M5-3`, `VALID-2`) so the dependency graph stays trustworthy.
- Move a ticket to **Done** only when the task is actually done and
  validated per its milestone's testing bar (QEMU + real hardware where
  applicable) — not when the code merely compiles.
- If a change makes a `plan.md` §10 decision obsolete or wrong, update that
  decision's entry (don't delete the history — note what changed and why,
  matching the existing entries' style).

## Hardware/spec research provenance

Any research against a vendor developer manual (AMD APM, Intel SDM, a
datasheet) — or hardware/spec research in general (a device register
layout, an on-the-wire format, an errata) — must be archived so it is
never re-fetched from the web:

- **Check order, always, before any web search or download:** (1) the
  relevant ticket's description/comments, then (2) the `research/`
  directory, then — only if neither has it — (3) the web. Reaching for
  a web search or download first is a process error; the whole point of
  this rule is that the answer is usually already captured.
- **When you do fetch a manual/datasheet:** save the PDF (or the exact
  source document) under `research/` with a descriptive, versioned name
  (e.g. `research/amd-apm-vol2-24593-r3.44.pdf`), and record in
  `research/README.md` what it is, its version/revision, and where it
  came from.
- **Capture the extract against the task:** in the ticket (or tickets) the
  research was for, write the specific facts used — the
  section/table numbers, the field offsets, the bit meanings, the exact
  values — as a short summary with a pointer to the archived file
  (`research/<file>`, §/table). These per-task summaries are the
  first thing the next agent (or future you) reads, so make them
  self-sufficient: enough to act on without re-opening the PDF.
- Prefer in-tree primary sources when they exist (the vendored `edk2/`
  and QEMU headers are authoritative for their own formats) and cite the
  file path the same way; the `research/` archive is for external
  documents that are not already in the repo.

## Keeping user-facing docs in sync

- The top-level `README.md` (once `DOCS-1` exists) is written for someone
  downloading a packaged build/installer — no build/toolchain instructions
  there. Build/toolchain docs belong in `fw/README.md`/`tools/` instead;
  don't blur the two.
- Any change that affects what an end user sees or does — install steps,
  supported host/guest OS list, first-run behavior, config file format,
  packaging/installer mechanism — must update `README.md` (and any other
  affected doc, e.g. `fw/README.md` for firmware-build-provenance changes)
  in the same change, not as a follow-up. Treat stale user-facing docs the
  same as a stale ticket or `plan.md` — don't merge a behavior change
  without the doc that describes it.
