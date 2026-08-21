# Learnings — hype

Project-specific learnings extracted from real incidents. Each file records one
finding: what happened, the lesson, and the rule it backs. The rules in
`AGENTS.md` and the skills stay short; the story that justifies a rule lives
here.

Add a learning when an incident teaches something a future agent would
otherwise re-learn the hard way. Keep each file to one finding. Cite the ticket
number and the observable evidence, not just the conclusion.

## Invariants — the hard rules, one per doc

These are not war-stories; they are the project's non-negotiable rules, above
performance, features, and convenience. **Do not weaken any of them without
updating `plan.md` §10 first.** `AGENTS.md` lists them as one-liners and points
here for the full text and rationale.

- [invariant-security-boundaries.md](invariant-security-boundaries.md) — the
  host↔guest and guest↔guest boundaries are paramount; no unintentional leakage.
- [invariant-guest-isolation.md](invariant-guest-isolation.md) — validate every
  guest-supplied address/length against the VM's own mapped range; no colliding
  cpu_set/target_disk/varstore; default-deny guest networking; fault one guest
  alone.
- [invariant-cpu-time-isolation.md](invariant-cpu-time-isolation.md) — a vCPU
  never loses CPU-time isolation; `dedicated` holds by construction, `shared`
  only because preemption is mandatory.
- [invariant-core-allocation.md](invariant-core-allocation.md) — a physical core
  is the unit of allocation and a vCPU IS a physical core; SMT is a bonus, never
  idle a sibling or disable SMT.
- [invariant-guest-ram-zeroed.md](invariant-guest-ram-zeroed.md) — guest RAM is
  zeroed before first execution, every (re)start.
- [invariant-no-direct-hw-access.md](invariant-no-direct-hw-access.md) — no
  guest gets direct hardware access; always host-driver + emulated frontend.
- [invariant-physical-write-guard.md](invariant-physical-write-guard.md) —
  destructive `physical:` writes are triple-guarded (serial match, confirm,
  non-empty-PT guard).

## Index

- [milestone-by-subject.md](milestone-by-subject.md) — a ticket's milestone
  follows its subject, not where the work was found (BSD's 19 mis-filed tickets).
- [nop-slide.md](nop-slide.md) — zeroed guest RAM is a NOP slide; a guest
  entered at the wrong address reports a false PASS (#535).
- [sendkeys-ps2.md](sendkeys-ps2.md) — why typed guest commands lost keys, and
  the PS/2 IRQ1 fix (#582).
- [noboot-vs-wedge.md](noboot-vs-wedge.md) — a boot that never reaches hype is
  NOBOOT, not a pass or a fail (#371).
- [bannerless-boot.md](bannerless-boot.md) — INVALID-boot retries, and why
  SIGKILL does not lose serial output (#581).
- [qemu-ahci-crash.md](qemu-ahci-crash.md) — QEMU's own AHCI crash on some
  hosts, and how the harness treats it.
- [fw-cfg-string-io.md](fw-cfg-string-io.md) — measure before deciding: the
  MADT assumption vs. the byte-level fw_cfg trace.
- [file-global-state-leak.md](file-global-state-leak.md) — file-global
  emulation state is a guest↔guest leak (port-0x80, per-vCPU de-globalization).
- [gh-cli-limits.md](gh-cli-limits.md) — `gh project item-list` truncation and
  the unreadable Priority field.
- [hype-logs-lc-all-c.md](hype-logs-lc-all-c.md) — hype's logs are invalid
  UTF-8; grep needs `LC_ALL=C grep -a`.
