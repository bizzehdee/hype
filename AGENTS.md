# Agent Rules — hype

This repo builds a thin UEFI type-1 hypervisor. The full design lives in
[`plan.md`](plan.md); the actionable, dependency-tracked breakdown lives on the
**GitHub Project board**. Read `plan.md` before making non-trivial changes —
this file is the condensed always-on rule set, not a replacement for it.

This file holds only the rules that apply to *every* change. Detailed process
lives in on-demand **skills**; project-specific war-stories that justify a rule
live in **`.learnings/`**. Both are indexed at the end of this file.

## Before doing anything

1. Find the board ticket covering the work. If none exists, create one (in **To
   Do**) before starting — don't do undocumented work. Move the ticket you're
   starting to **Doing**. Details: the **`task-board`** skill.
2. Check that the ticket's **"is blocked by"** links are all **Done**. If they
   aren't, either do them first or stop and ask — do not skip ahead on the
   assumption a prerequisite "probably doesn't matter yet."
3. If the work touches something `plan.md` §10 already decided, follow that
   decision. If you think a decision is wrong, say so and get it changed in
   `plan.md` §10 — don't silently diverge from a documented decision in code.
4. If the work surfaces a genuinely new decision (a fork not already covered by
   §10), resolve it and add it to `plan.md` §10 as a new numbered entry before
   writing the code that depends on it.

A **bugfix** (behavior doesn't match what `plan.md` or the ticket already
specify) can go straight to a code change. Anything bigger — new capabilities,
new config surface, new devices/drivers, any change beyond restoring the
documented spec — **must go through `plan.md` first**, then become a board
ticket. See the **`task-board`** skill.

## Diagnose first, decide on evidence — not assumptions

- **When something breaks or behaves unexpectedly, diagnose it before changing
  anything.** Read the actual failure output, add a targeted trace/probe,
  compare a working path against the broken one — rather than guessing at a
  cause and building a fix on the guess. A fix aimed at an assumed cause usually
  wastes more time than the diagnosis would have taken, and often masks the real
  bug.
- **Back every non-trivial decision with evidence you actually gathered**, not a
  plausible-sounding theory. If you catch yourself saying "it's probably X,"
  stop and get the measurement that confirms or refutes X first. Worked example:
  `.learnings/fw-cfg-string-io.md`.
- **State what you measured and how, so the conclusion is checkable.** Cite the
  log line, the trace, the behavior diff — not just the conclusion. Label an
  untested hypothesis as one.

## Hard invariants — do not weaken these without updating plan.md §10 first

- **The host↔guest and guest↔guest security boundaries are paramount — above
  performance, features, or convenience.** Nothing may cross either boundary
  unintentionally. The host must never expose its own state, memory, or hardware
  to a guest except through a deliberately designed, mediated interface; one
  guest must never observe or affect another (its memory, its I/O, its timing
  side-channels, shared emulation state that should be per-VM) except where the
  operator has *explicitly* configured a channel (e.g. `net_peers`). The rule is
  against *unintentional* leakage, not designed communication. When in doubt,
  treat a potential cross-boundary path as a leak and prove it isn't. A
  performance or simplicity win that erodes a boundary is not a win — see
  `.learnings/file-global-state-leak.md`.
- **Guest isolation is the point of this project.** Each of these exists because
  of `plan.md` §6g/§6j/§10's security-review decisions (#19–22):
  - Every device-emulation path that touches a guest-supplied address, offset,
    or length (virtio descriptors, AHCI/NVMe command buffers, block I/O
    LBA+count) **must** validate it against that specific VM's own EPT/NPT-mapped
    range and the backing resource's real size before the host dereferences it
    or performs the I/O. No raw guest pointer is ever trusted. This is the actual
    guest-escape vector — EPT/NPT alone does not prevent it.
  - No two VMs' `cpu_set` ranges, `target_disk` paths, or varstore files may
    overlap/collide — enforced at startup admission control, not assumed.
  - Guest-to-guest networking is default-deny; a pairing is allowed only when
    explicitly named via `net_peers`, validated at startup.
  - A misbehaving/faulted guest is torn down alone (Force power off) — never a
    hypervisor-wide halt or reset in response to one guest's fault.
  - A fault-isolation watchdog catches hangs/anomalies; it is **not** a
    substitute for the input-validation rule above.
- **A vCPU never loses CPU-time isolation — but the mechanism depends on the
  tier** (plan.md §3, §6g, §10 decision 39), chosen per VM by `cpu_mode`:
  - **`dedicated` (default): 1:1 exclusive vCPU-to-pCPU pinning.** No shared pCPU
    between two VMs, ever. Isolation holds by construction. Do not weaken this.
  - **`shared`: cores are pooled and time-sliced.** Isolation holds *only because
    preemption is mandatory*. Any change that lets a guest defer or suppress its
    own preemption breaks the guarantee, however harmless it looks locally.

  Two things follow and neither is negotiable: a core may never appear in both a
  dedicated `cpu_set` and the shared pool, and distrusting `isolation_group`s
  may never occupy one physical core simultaneously (default is one group per
  VM).
- **A hardware thread is the unit of execution; a physical core is the unit of
  allocation** (§10 decision 40) — **and a vCPU IS a physical core** (decision
  47). `vcpus = N` costs exactly N cores on every host, and SMT is a **bonus**: a
  granted core is granted whole, so a dedicated VM given one 2-thread core gets
  one vCPU whose guest sees two logical CPUs. Never idle a sibling thread to
  satisfy an isolation rule and never disable SMT for the pool — the rule forbids
  two *distrusting* owners on one core at the same time, which core-granular
  allocation already delivers. If (package, core, thread) cannot be proven for
  this host, treat every logical processor as its own single-threaded core;
  wasting threads is safe, pairing distrusting owners by accident is not.
- **Guest RAM is zeroed before first execution**, on every (re)start, including
  after Force power off — never reused as-is. Never pad a guest payload with
  zeros either: `.learnings/nop-slide.md`.
- **No guest gets direct hardware access.** Physical disk/NIC access is always
  mediated through a host-side driver plus an emulated guest-facing frontend —
  never PCI passthrough or guest-initiated DMA to real hardware. This is why v1
  needs no IOMMU; don't add passthrough without revisiting that.
- **Destructive writes to a `physical:` target disk require**: serial/GUID match
  confirmed at VM start, an interactive dashboard confirmation before the first
  write, and a non-empty-partition-table guard — a `physical:` config entry
  alone must never be sufficient to trigger a wipe.

## Toolchain & language

- `hype.efi` itself: **C**, freestanding, targeting `x86_64-unknown-uefi`, built
  with the lightweight clang/lld-or-GNU-EFI pipeline — not EDK2, not Rust
  (`plan.md` §8, §10 decision #17). No libc.
- The guest firmware blob is a separate concern, built via EDK2, vendoring a
  stripped OVMF (`plan.md` §10 decision #1). Don't conflate the two pipelines.
- Every device-emulation and host-driver module runs at the most privileged
  level with no OS underneath and no process boundary to contain a bug — code
  review here weighs a missed bounds check as a full-machine compromise, not a
  crash.

## Testing

- QEMU/KVM nested virtualization (`-cpu host,+vmx`) for fast iteration.
- A **mandatory real-hardware validation pass** (both Intel and AMD, per
  `plan.md` §10 decision #18) at every milestone gate — QEMU alone is necessary
  but not sufficient; nested VMX/SVM emulation doesn't reproduce every edge case.
- **Unit testing is a core requirement, not optional, on all testable code.**
  "Testable" means anything expressible as pure(-ish) logic that doesn't require
  privileged CPU state, real hardware, or a running hypervisor: the `hype.cfg`
  parser, admission-control checks, guest-address bounds-checking logic,
  `blk_backend` LBA/length validation, ACPI table synthesis, watchdog
  fault-classification, power-lifecycle state records.
- **90% line/branch coverage is the floor**, not a target to approach, on every
  testable module. Falling short blocks the change — treat it like a failing
  build. Hardware-touching shims (VMXON/VMCS/VMCB setup, inline asm, VM-exit
  trampolines, real MMIO/PIO) are exempt, but write the shim as thin as possible
  and push the decision logic behind it into a plain, testable function. Don't
  use "it touches hardware somewhere in the call stack" to excuse a whole module.
- Guest-side tests of hype's own emulation are **microtests** — see the
  **`microtests`** skill for how to build, run, and score them.

## License

Project license is **GPLv3**. Any third-party code adapted in (e.g. AHCI/NVMe or
NIC host drivers) must be GPLv3-*compatible*: MIT/BSD, Apache, GPLv3, or
"GPLv2-or-later" are fine. **Plain GPLv2-only code is not GPLv3-compatible** —
check the specific file/module's license header before adapting anything, not
just the source project's overall stated license.

## Skills (on-demand detail)

Load these when the work matches; they hold the full process, not this file.

- **`task-board`** — the GitHub Project board: creating/moving tickets, Status
  columns, milestones, labels, feature-vs-bugfix routing, plan/board sync,
  user-facing-doc sync, gh CLI limits.
- **`microtests`** — building, running, and scoring the `tests/micro/` guests
  and the `run-guest.sh` harness (verdicts, NOBOOT, INVALID retries).
- **`research-provenance`** — sourcing and archiving vendor-manual/spec research
  under `research/`.
- **`doc-style`** — the controlled technical writing style for README, spec
  docs, and ticket bodies.

## Learnings

`.learnings/` holds project-specific findings from real incidents — the story
behind a rule. See [`.learnings/README.md`](.learnings/README.md) for the index.
Add a learning when an incident teaches something a future agent would otherwise
re-learn the hard way.
