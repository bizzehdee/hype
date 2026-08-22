---
name: testing
description: hype's testing requirements — QEMU/KVM nested-virt for fast iteration, the mandatory Intel+AMD real-hardware validation pass at every milestone gate, and the 90% line/branch unit-test coverage floor with its narrow hardware-shim exemption. Use when writing tests, judging whether work is done, or deciding what must be covered.
---

# Testing — hype

## Iteration and the real-hardware gate

- Use QEMU/KVM nested virtualization (`-cpu host,+vmx`) for fast iteration.
- A **mandatory real-hardware validation pass** (both Intel and AMD, per
  `plan.md` §10 decision #18) is required at every milestone gate. QEMU alone is
  necessary but not sufficient — nested VMX/SVM emulation does not reproduce
  every edge case. Move a ticket to **Done** only after this pass where the
  milestone requires it, not when the code merely compiles.
- **Before running a real-hardware validation session, read the `hwval-*`
  learnings** — each cold boot is expensive, and every one of these was found
  the hard way: [`.learnings/hwval-config-validate-before-boot.md`](../../../.learnings/hwval-config-validate-before-boot.md)
  (validate `hype.cfg` against the real parser first), [`.learnings/hwval-live-image-no-persistence.md`](../../../.learnings/hwval-live-image-no-persistence.md)
  (a live/diskless guest image can't prove persistence), and
  [`.learnings/hwval-repro-before-reboot.md`](../../../.learnings/hwval-repro-before-reboot.md)
  (reproduce a suspected storage bug host-side before spending another boot on it).

## Unit testing

- **Unit testing is a core requirement, not optional, on all testable code.**
  "Testable" means anything expressible as pure(-ish) logic that does not require
  privileged CPU state, real hardware, or a running hypervisor: the `hype.cfg`
  parser, admission-control checks, guest-address bounds-checking logic,
  `blk_backend` LBA/length validation, ACPI table synthesis, watchdog
  fault-classification, power-lifecycle state records.
- **90% line/branch coverage is the floor**, not a target to approach, on every
  testable module. Falling short blocks the change — treat it like a failing
  build.
- Hardware-touching shims (VMXON/VMCS/VMCB setup, inline asm, VM-exit
  trampolines, real MMIO/PIO) are exempt, but write the shim as thin as possible
  and push the decision logic behind it into a plain, testable function. Do not
  use "it touches hardware somewhere in the call stack" to excuse a whole module.
- Host-native unit tests live in `core/tests/`; `core/tests/run.sh` builds and
  runs them with coverage. An aborted test binary prints no `FAIL:` line — gate
  on the exit code, not a `grep`.

## Microtests

Guest-side tests of hype's own emulation are **microtests** — see the
**`microtests`** skill for how to build, run, and score them (verdicts, NOBOOT,
INVALID retries).
