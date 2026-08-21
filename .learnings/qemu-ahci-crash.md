# QEMU's own AHCI crash on some hosts

**Upstream:** `qemu-project/qemu#437`, fixed in QEMU 11.1.0. **Backs:** the
INVALID-retry rule in the `microtests` skill.

## What happened

On hosts running QEMU below 11.1.0, QEMU can die on a signal inside its own AHCI
emulation. This is a QEMU defect, not a hype defect. It presents as QEMU exiting
on a signal with no useful hype output.

## The lesson

- Treat a QEMU signal death as an INVALID boot, not a hype failure. The harness
  retries it up to `BOOT_ATTEMPTS` and counts it apart.
- Hosts at QEMU 11.1.0 or later do not hit it.
- Do not build a hype-side fix for a symptom that only appears under old QEMU
  AHCI emulation — confirm the QEMU version first.
