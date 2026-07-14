# Guest UEFI firmware (M4-1/M4-2)

`OVMF_CODE.fd`/`OVMF_VARS.fd` in this directory are the guest-facing UEFI
firmware pair (per plan.md §7: "reuse a stripped EDK2/OVMF build as a
vendored blob") -- **not** the host's own `hype.efi` boot firmware
(that's whatever UEFI implementation the physical machine already
ships, or `edk2-ovmf` when testing under QEMU per `docs/toolchain.md`).
Guest VMs boot through this pair the same way `hype.efi` itself boots
through the host's own firmware.

## Provenance

- Source: [`tianocore/edk2`](https://github.com/tianocore/edk2), vendored
  as a git submodule at `/edk2`, pinned to tag `edk2-stable202511`, plus
  one local commit on top (`UefiCpuPkg/CpuExceptionHandlerLib: fix X64
  IDT stub build under NASM 3.x`) -- a compatibility fix for building
  against NASM 3.01 (the only version available on this project's
  current dev distro), not an upstream EDK2 correctness fix. See that
  commit's message (`cd edk2 && git log`) for the full explanation.
- Built via `tools/build-fw.sh`: `OvmfPkg/OvmfPkgX64.dsc`, `-a X64 -t
  CLANGDWARF -b RELEASE` (Clang, not GCC -- matches this project's own
  toolchain choice, plan.md §8).
- License: EDK2 is BSD-2-Clause-Patent, compatible with this project's
  GPLv3 terms (see the top of `plan.md` for the general
  compatible-license policy).

## Rebuilding

```
tools/build-fw.sh
```

Requires (separate from `hype.efi`'s own toolchain, deliberately, per
plan.md §8): `nasm`, `iasl` (Fedora: `acpica-tools`), `libuuid-devel`,
`python3-devel`, `clang`, `git`. The script builds EDK2's BaseTools
(forced to clang) and then `OvmfPkgX64.dsc`, copying the resulting
`OVMF_CODE.fd`/`OVMF_VARS.fd` here.

## Validation so far

Smoke-tested standalone in QEMU (reaches BdsDxe's boot manager
correctly reporting no bootable device with nothing attached) and
booting `hype.efi` itself all the way through its own existing test
suite (M2-7's real-mode test guest, M3-5's Linux boot-protocol shim
test guest, timer bring-up) exactly as `edk2-ovmf` already does. Not
yet used as *guest*-facing firmware for an actual VM -- that starts at
M4-3 (emulated flash/varstore) and M4-6 (a real guest OS installer
boot).
