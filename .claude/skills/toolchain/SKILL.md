---
name: toolchain
description: How hype.efi is built and what language rules apply — freestanding C11 targeting x86_64-unknown-uefi via the clang/lld (or GNU-EFI) pipeline, not EDK2 and not Rust, with no libc; the separate EDK2 OVMF firmware pipeline; and why every module runs at full privilege. Use when touching the build, adding a source file, choosing a dependency, or hitting a freestanding link error.
---

# Toolchain & language — hype

- `hype.efi` itself: **C**, freestanding, targeting `x86_64-unknown-uefi`, built
  with the lightweight clang/lld-or-GNU-EFI pipeline — **not EDK2, not Rust**
  (`plan.md` §8, §10 decision #17). **No libc.**
- The guest firmware blob is a separate concern, built via EDK2, vendoring a
  stripped OVMF (`plan.md` §10 decision #1). Do not conflate the two build
  pipelines.
- Every device-emulation and host-driver module runs at the most privileged
  level with no OS underneath and no process boundary to contain a bug — code
  review here weighs a missed bounds check as a full-machine compromise, not a
  crash.

## Freestanding traps

- No libc means whole-struct assignment of anything containing an array emits a
  hidden `memcpy` call that fails to link ("undefined symbol: memcpy"). Copy
  field-by-field. Only `make all` catches this — the host unit-test build does
  not.
- `make` ignores `EXTRA_CFLAGS` changes on an unchanged source mtime; `touch`
  the file or `make clean` when switching build variants, and gate every run on
  the banner sha.
- The build bakes `HYPE_BUILD_ID` from `git describe --always --dirty
  --abbrev=7`; a stale banner means the binary was not recompiled.
