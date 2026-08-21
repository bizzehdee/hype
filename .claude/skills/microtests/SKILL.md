---
name: microtests
description: How to build, run, and reason about hype's microtest guests (tests/micro/) and the run-guest.sh harness — the guest-side tests of hype's own emulation. Use when writing or running a microtest, interpreting a PASS/FAIL/NOBOOT/INVALID outcome, or debugging the boot harness.
---

# Microtest guests (`tests/micro/`)

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

## Rules that are not optional

- **A microtest reports its own verdict**, `MICRO PASS: <name>` or
  `MICRO FAIL: <name> <what and what was expected>`, on the guest UART. hype
  relays it into that VM's log. The verdict is a line in the log, not an exit
  code.
- **A missing verdict is a FAILURE, not an absence of news.** A guest that
  wedges or triple-faults prints neither, and silence is the failure mode that
  looks most like success. The harness fails on no-verdict and on a host panic,
  and reports a boot that never reached hype as NOBOOT (#371) — which is neither
  a pass nor a fail, and must not be scored as either. See
  `.learnings/noboot-vs-wedge.md`.
- **An INVALID boot is retried, and the retry is printed (#581).** Two outcomes
  are not results: QEMU dying on a signal (its own AHCI crash, see
  `.learnings/qemu-ahci-crash.md`) and QEMU alive with no `hype: build` banner.
  `tools/run-guest.sh` retries both up to `BOOT_ATTEMPTS` (3), counts them
  apart, and prints how many a run and a batch consumed. A boot that DOES reach
  hype and then wedges is never retried — that one has to keep failing, which is
  why a blind "no banner → retry" is the wrong guard. See
  `.learnings/bannerless-boot.md`.
- **`SENDKEYS` key loss is FIXED (#582)** — no spacing rule any more. See
  `.learnings/sendkeys-ps2.md` for the cause and the measured before/after.
- QEMU is stopped with `SIGTERM` and only then `SIGKILL` (`QUIT_GRACE`, 5s).
  `STOP_SIGNAL=KILL` restores the old behaviour for an A/B. Serial output is not
  lost to a `SIGKILL`.
- **Never pad a guest payload with zeros.** `0x00 0x00` decodes as
  `add byte [rax], al`, so zeroed guest RAM is a NOP slide: a guest entered at
  the wrong address slides into the payload and reports a perfectly correct
  PASS. `tests/micro/crt0.S` fills its pre-entry region with `0xCC` for exactly
  this reason. See `.learnings/nop-slide.md`.
- **The artifacts are built once and selected by config**, never by a rebuild of
  hype. "Run only test N" is an edit to a `.cfg`, not `-D` on the build line.
- `make clean` removes `build/micro/`, so re-run `make micro` after one.

## Reading hype's on-stick logs

hype's logs (`hype.log` + per-VM guest logs) contain invalid UTF-8. `grep`
silently matches nothing without `LC_ALL=C grep -a`. See
`.learnings/hype-logs-lc-all-c.md`.
