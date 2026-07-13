# Toolchain versions (SETUP-2)

Pinned per `plan.md` §8/§11. Recorded from the current dev machine
(Fedora Linux 44) so "works on my machine" doesn't creep in later. Any
machine building `hype.efi` or running the QEMU loop should match these
(or the change should be a deliberate, recorded bump to this file).

| Component | Package | Version |
|---|---|---|
| C compiler | `clang` | 22.1.8 (Fedora 22.1.8-1.fc44) |
| Linker | `lld` (`ld.lld`) | 22.1.8 |
| Target triple | — | `x86_64-unknown-uefi` |
| Emulator | `qemu-system-x86` | 10.2.2 (qemu-10.2.2-1.fc44) |
| Guest firmware (dev loop) | `edk2-ovmf` | 20260508-4.fc44 |
| Debugger | `gdb` | 17.1-6.fc44 |

## OVMF firmware paths (this machine)

Non-SEV/TDX, non-secure-boot pair used for the QEMU dev loop:

- Code: `/usr/share/OVMF/OVMF_CODE.fd`
- Vars: `/usr/share/OVMF/OVMF_VARS.fd`

Secure-Boot-capable variants also exist (`OVMF_CODE.secboot.fd` /
`OVMF_VARS.secboot.fd`) — not used for v1 per §10 decision #5 (Secure Boot
disabled on test hardware; STRETCH-2 revisits signing later).

QEMU machine type: `q35` (tested against `pc-q35-9.2`).

## Smoke-tested

Clang 22 targets `x86_64-unknown-uefi` directly (no GNU-EFI headers
needed) and emits a valid PE32+ COFF object; `ld.lld -flavor link
-subsystem:efi_application` links it into a `file`-verified "PE32+
executable for EFI (application), x86-64". Command line used:

```sh
clang --target=x86_64-unknown-uefi -ffreestanding -fshort-wchar \
  -mno-red-zone -c smoke.c -o smoke.o
ld.lld -flavor link -subsystem:efi_application -entry:efi_main \
  -out:smoke.efi smoke.o
```

Full boot-in-QEMU validation of this pipeline is M0-4/M0-5, not SETUP-2 —
this only confirms the compiler/linker invocation itself produces a
loadable EFI image.

## Rebuilding this toolchain elsewhere

```sh
# Fedora
sudo dnf install clang lld qemu-system-x86 edk2-ovmf gdb

# Debian/Ubuntu (package names differ; GNU-EFI is the fallback per §8
# if clang+lld's UEFI target support is unavailable)
sudo apt install clang lld qemu-system-x86 ovmf gdb
```

If a given machine's package manager pulls different exact versions,
bump the table above in the same commit that changes the build — don't
let it drift silently.
