# Learnings — hype

Project-specific learnings extracted from real incidents. Each file records one
finding: what happened, the lesson, and the rule it backs. The rules in
`AGENTS.md` and the skills stay short; the story that justifies a rule lives
here.

Add a learning when an incident teaches something a future agent would
otherwise re-learn the hard way. Keep each file to one finding. Cite the ticket
number and the observable evidence, not just the conclusion.

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
