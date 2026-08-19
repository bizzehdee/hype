# Agent Rules — hype

This repo builds a thin UEFI type-1 hypervisor. Full design lives in
[`plan.md`](plan.md); the actionable, dependency-tracked breakdown of that
design lives on the **GitHub Project board** (see "Task tracking" below).
Read `plan.md` before making non-trivial changes — this file is the condensed
rule set, not a replacement for it.

## Task tracking

The **GitHub Project board is the single source of truth for task progress**:
<https://github.com/users/bizzehdee/projects/3>. It has five Status columns —
**To Do**, **Doing**, **On Hold**, **Done**, **Rejected**. Each task is its own issue in
`bizzehdee/hype` (never combine tasks into one ticket). A ticket carries:

- the short task title (e.g. `GLADDER-5: Fedora Server, single-VM …`);
- the full description in the issue body;
- progress/engineering notes as issue **comments**;
- the milestone as the native **Milestone** field (`GLADDER`, `INSTALLER`,
  `VALID`, `VIDEO`, …) — *not* a label;
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
- **Every new issue MUST end up on project 3 with Status = To Do. No ticket may
  exist outside the board.** The board is the single source of truth for
  progress, so a ticket missing from it is invisible — it will not be picked up,
  ordered, or counted. Repo automation currently adds new issues to the project
  and sets To Do for you, so in practice this means **verify, don't assume**:
  after creating an issue, check it (`gh issue view <n> --json projectItems`) and
  add it plus set its Status yourself if the automation did not. An issue left
  off the board is unfinished work, not a shortcut.
- **Starting a task** → move it to **Doing**.
- **Blocked or waiting task** → add a comment saying what it is waiting for,
  then move it to **On Hold**. Anything that has already been through **Doing**
  and is now waiting on something outside the working session belongs there:
  a real-hardware run, an intermittent fault to recur, another ticket to land,
  a decision, hardware or media that is not to hand.
  It does **not** go back to **To Do**. To Do means "not started"; a ticket that
  has been worked on and is waiting is a different state, and collapsing the two
  loses the fact that work exists and the reason it stopped. It also makes the
  board lie about how much is untouched.
  Move it back to **Doing** when the thing it waits for arrives — a stick comes
  back, the fault fires again, the blocking ticket closes.
- **Completed with a positive outcome** → move to **Done** (and close the
  issue).
- **No longer needed, with no outcome** → move to **Rejected**.
- **Never move a closed issue back to To Do or Doing. Raise a follow-up instead.**
  If work has to resume on something already closed — a regression, an
  incomplete fix, a reverted change — create a **new** issue in **To Do** that
  references the original by number and says what brought it back. Leave the
  original closed and in **Done**.
  A closed ticket in an active column is a contradiction (the board says there
  is work to do, the issue says there is not), and reopening it is worse than it
  looks: it rewrites the record so the original's close date, its Done state and
  any release notes derived from them silently become false. A follow-up keeps
  the history of what was actually finished and when, and makes the *reason* work
  resumed a first-class, reviewable thing rather than a status flip.
- Reference the task ID (e.g. `M5-3`, `VALID-2`) in commit messages/PRs so the
  dependency graph stays trustworthy.

### Milestones

- **Every milestone name is a single word in ALL CAPS** (`STORAGE`, `CONFIG`,
  `MULTIVM`, `BSD`). No spaces, no hyphens into phrases, no two milestones with
  the same name. The milestone **description** carries the long explanation
  (e.g. `STORAGE = "M10: physical disks and host filesystems"`); the name is
  only the handle.
- **A ticket's milestone must match its subject, not where it was found.** Work
  discovered while chasing a FreeBSD bug is not automatically `BSD`: a config
  key belongs in `CONFIG`, a media-source defect in `STORAGE`, a disk front-end
  in `VDISK`. Filing a ticket under the milestone you happened to be working in
  is how `BSD` accumulated 19 non-BSD tickets before the 2026-08-06 audit.
- A ticket whose subject spans several milestones, or fits none, may stay
  unmilestoned — that is better than a wrong assignment. Say why in the ticket.
- Do not rename or merge milestones ad hoc; a rename touches every ticket in it
  and every doc that names it, so treat it as its own reviewed task.

### Labels

- **Every issue carries at least one label.** Labels are descriptors; the
  milestone stays in the Milestone field, never duplicated as a label.
- One **kind** label per issue, chosen by what the ticket asks for:
  - `bug` — a defect report: something behaves wrongly today;
  - `enhancement` — a new capability or an extension of one;
  - `refactor` — behaviour-preserving restructuring;
  - `spike` — a time-boxed question to answer, not code to deliver;
  - `documentation`, `testing` — when the deliverable is docs or test
    infrastructure itself.
- Plus any **domain** labels that genuinely apply (several are fine):
  `storage`, `config`, `usb`, `networking`, `input`, `diagnostics`,
  `performance`, `guest-compat` (a specific guest OS misbehaves),
  `amd-svm` / `intel-vmx` (backend-specific), `hardware-validation` (needs a
  real-hardware run to close).
- Create a missing label rather than overloading a near-miss, but check
  `gh label list` first — a synonym of an existing label is clutter, not
  precision.

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
- **A vCPU never loses CPU-time isolation — but which mechanism assures it
  depends on the tier** (plan.md §3, §6g, §10 decision 39). Two tiers exist,
  chosen per VM by `cpu_mode`:
  - **`dedicated` (the default): 1:1 exclusive vCPU-to-pCPU pinning.** No
    shared pCPU between two VMs, ever. Isolation holds *by construction* —
    no mechanism has to work correctly for it to hold. Do not weaken this;
    it is what latency-sensitive and security-critical guests are for.
  - **`shared`: cores are pooled and time-sliced.** Isolation holds *only
    because preemption is mandatory*. Any change that lets a guest defer or
    suppress its own preemption breaks the guarantee outright, however
    harmless it looks locally.

  Two things follow, and neither is negotiable: a core may never appear in
  both a dedicated `cpu_set` and the shared pool, and distrusting
  `isolation_group`s may never occupy one physical core simultaneously
  (default is one group per VM, so configuring nothing is the strict case).
- **A hardware thread is the unit of execution; a physical core is the unit
  of allocation** (plan.md §10 decision 40) — **and a vCPU IS a physical
  core** (decision 47). `vcpus = N` costs exactly N cores on every host, and
  SMT is a **bonus**: a granted core is granted whole, so a dedicated VM
  given one 2-thread core gets one vCPU whose guest sees two logical CPUs,
  and the same config on a non-SMT host costs the same core and yields one.
  Never idle a sibling thread to satisfy an isolation rule and never disable
  SMT for the pool — the rule forbids two
  *distrusting* owners on one core at the same time, which core-granular
  allocation already delivers. If (package, core, thread) cannot be proven
  for this host, fall back to treating every logical processor as its own
  single-threaded core; wasting threads is safe, pairing distrusting owners
  by accident is not.
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

### Microtest guests (`tests/micro/`)

Guest-side tests of hype's own emulation. Each is a small freestanding kernel
built to a bzImage-shaped artifact and booted as an ordinary configured VM
(`boot = kernel`, `plan.md` §10 decision 45) — so it goes through the same
config parse, admission, RAM carve, device model and dispatch loop a real guest
does. They replace the in-binary self-test battery (#534).

```
make micro                                            # build the artifacts
tools/micro/run-micro.sh                              # every test, one VM each
tools/micro/run-micro.sh ram1                         # just one
tools/micro/run-micro.sh --suite tests/micro/suite-all.cfg   # all in one boot
```

Rules that are not optional:

- **A microtest reports its own verdict**, `MICRO PASS: <name>` or
  `MICRO FAIL: <name> <what and what was expected>`, on the guest UART. hype
  relays it into that VM's log. This is #282's rule one level down: the verdict
  is a line in the log, not an exit code.
- **A missing verdict is a FAILURE, not an absence of news.** A guest that wedges
  or triple-faults prints neither, and silence is the failure mode that looks most
  like success. The harness fails on no-verdict and on a host panic, and reports a
  boot that never reached hype as NOBOOT (#371) — which is neither a pass nor a
  fail, and must not be scored as either.
- **Never pad a guest payload with zeros.** `0x00 0x00` decodes as
  `add byte [rax], al`, so zeroed guest RAM is a NOP slide: a guest entered at the
  wrong address slides into the payload and reports a perfectly correct PASS.
  That happened (#535). `tests/micro/crt0.S` fills its pre-entry region with
  `0xCC` for exactly this reason.
- **The artifacts are built once and selected by config**, never by a rebuild of
  hype. "Run only test N" is an edit to a `.cfg`, not `-D` on the build line.
- `make clean` removes `build/micro/`, so re-run `make micro` after one.

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

# Writing Style
 
Write in a controlled technical style.
 
## Principles
 
- Prioritize clarity over elegance.
 - Use simple, common English.
 - Prefer short sentences (10–20 words).
 - Express one idea per sentence.
 - Use active voice unless passive voice is clearer.
 - Use the same term for the same concept throughout the document.
 - Avoid unnecessary adjectives and adverbs.
 - Avoid marketing, conversational, or emotional language.
 - Use concrete, measurable statements instead of vague language.
 - Use "must" for requirements, "should" for recommendations, "may" for optional actions, and "can" for capabilities.
 
## Instructions
 
- Present actions in execution order.
 - One action per numbered step.
 - State conditions before dependent actions.
 - Describe expected results when helpful.
 - Use numbered lists for procedures and bullet lists for unordered information.
 
## Language
 
Do not:
 - Use synonyms for established technical terms.
 - Use filler words.
 - Use idioms, metaphors, or colloquialisms.
 - Assume prior knowledge without explanation.
 - Use pronouns when the reference could be ambiguous.
 
Before responding, verify that terminology is consistent, instructions are unambiguous, and each sentence communicates one primary idea.
