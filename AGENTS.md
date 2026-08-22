# Agent Rules — hype

This repo builds a thin UEFI type-1 hypervisor. The full design lives in
[`plan.md`](plan.md); the actionable, dependency-tracked breakdown lives on the
**GitHub Project board**. Read `plan.md` before making non-trivial changes —
this file is the condensed always-on rule set, not a replacement for it.

This file holds only the rules that apply to *every* change. Detailed process
lives in on-demand **skills**; the hard invariants and the war-stories that
justify a rule live in **`.learnings/`**. Both are indexed at the end.

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

Before changing code to fix a break, diagnose it on real evidence first — the
**`diagnose-first`** skill.

## Hard invariants — do not weaken any of these without updating plan.md §10 first

These are the project's non-negotiable rules, above performance, features, and
convenience. Each is a one-liner here; the full text and rationale is its own
document in `.learnings/`. Read the document before touching anything near it.

- **Security boundaries are paramount** — nothing crosses host↔guest or
  guest↔guest unintentionally. [`.learnings/invariant-security-boundaries.md`](.learnings/invariant-security-boundaries.md)
- **Guest isolation is the point** — validate every guest-supplied
  address/offset/length against the VM's own EPT/NPT range and the backing
  resource's real size; no colliding `cpu_set`/`target_disk`/varstore;
  default-deny guest networking; fault one guest alone.
  [`.learnings/invariant-guest-isolation.md`](.learnings/invariant-guest-isolation.md)
- **A vCPU never loses CPU-time isolation** — `dedicated` holds by construction;
  `shared` holds only because preemption is mandatory.
  [`.learnings/invariant-cpu-time-isolation.md`](.learnings/invariant-cpu-time-isolation.md)
- **A physical core is the unit of allocation; a vCPU IS a physical core** — SMT
  is a bonus; never idle a sibling or disable SMT to satisfy isolation.
  [`.learnings/invariant-core-allocation.md`](.learnings/invariant-core-allocation.md)
- **Guest RAM is zeroed before first execution**, every (re)start.
  [`.learnings/invariant-guest-ram-zeroed.md`](.learnings/invariant-guest-ram-zeroed.md)
- **No guest gets direct hardware access** — always host-driver + emulated
  frontend, never passthrough/guest DMA.
  [`.learnings/invariant-no-direct-hw-access.md`](.learnings/invariant-no-direct-hw-access.md)
- **Destructive `physical:` writes are triple-guarded** — serial match,
  interactive confirm, non-empty-partition-table guard.
  [`.learnings/invariant-physical-write-guard.md`](.learnings/invariant-physical-write-guard.md)

## License

Project license is **GPLv3**. Any third-party code adapted in (e.g. AHCI/NVMe or
NIC host drivers) must be GPLv3-*compatible*: MIT/BSD, Apache, GPLv3, or
"GPLv2-or-later" are fine. **Plain GPLv2-only code is not GPLv3-compatible** —
check the specific file/module's license header before adapting anything, not
just the source project's overall stated license.

## Skills (on-demand detail)

Load these when the work matches; they hold the full process, not this file.

- **`task-board`** — the GitHub Project board: tickets, Status columns,
  milestones, labels, feature-vs-bugfix routing, plan/board sync, gh CLI limits.
- **`diagnose-first`** — diagnose-on-evidence discipline: root-cause from real
  output before changing code; measure, don't theorise.
- **`toolchain`** — how `hype.efi` is built: freestanding C11 for
  `x86_64-unknown-uefi` (not EDK2, not Rust, no libc), the EDK2 OVMF firmware
  pipeline, freestanding traps.
- **`testing`** — the testing gates: QEMU iteration, the mandatory Intel+AMD
  real-hardware pass, the 90% unit-test coverage floor.
- **`microtests`** — building, running, and scoring the `tests/micro/` guests
  and the `run-guest.sh` harness (verdicts, NOBOOT, INVALID retries).
- **`research-provenance`** — sourcing and archiving vendor-manual/spec research
  under `research/`.
- **`doc-style`** — the controlled technical writing style for README, spec
  docs, and ticket bodies.

## Learnings

`.learnings/` holds the hard invariants (above) and project-specific findings
from real incidents — the story behind a rule. See
[`.learnings/README.md`](.learnings/README.md) for the index. Add a learning
when an incident teaches something a future agent would otherwise re-learn the
hard way.

**Before running a real-hardware validation session** (any `HW-VAL`/hardware-
validation ticket, or working `rig/hw-queue/`), read the `hwval-*` learnings
first — each cold boot is expensive, and each of these cost one:
[`hwval-config-validate-before-boot.md`](.learnings/hwval-config-validate-before-boot.md),
[`hwval-live-image-no-persistence.md`](.learnings/hwval-live-image-no-persistence.md),
[`hwval-repro-before-reboot.md`](.learnings/hwval-repro-before-reboot.md).
