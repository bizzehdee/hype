# INVALID boots are retried; SIGKILL does not lose serial output

**Ticket:** #581. **Backs:** the INVALID-retry and stop-signal rules in the
`microtests` skill.

## What happened

Two outcomes were being counted as results when they are not:

1. QEMU dying on a signal (its own AHCI crash — see `qemu-ahci-crash.md`).
2. QEMU alive but printing no `hype: build` banner.

One hypothesis for the bannerless case was that stopping QEMU with `SIGKILL`
truncated its `-serial file:` output. Measured on 5 boots, the log sizes
differed by under 10 bytes whether stopped with `SIGTERM` or `SIGKILL`. That
candidate was ruled out: a bannerless boot is not the harness destroying its own
evidence.

## The lesson

- `tools/run-guest.sh` retries both INVALID cases up to `BOOT_ATTEMPTS` (3),
  counts them apart, and prints how many a run and a batch consumed — so a
  rising rate is visible instead of absorbed.
- A boot that DOES reach hype and then wedges is **never** retried. It must keep
  failing. A blind "no banner → retry" is the wrong guard because it would hide
  real wedges.
- QEMU is stopped with `SIGTERM`, then `SIGKILL` after `QUIT_GRACE` (5s).
  `STOP_SIGNAL=KILL` restores the old behaviour for an A/B comparison.
