# Toolchain versions (SETUP-2)

Pinned per `plan.md` §8/§11. This records the minimum versions this
project's build/dev loop has been validated against, not one specific
machine — any machine building `hype.efi` or running the QEMU loop
should meet or exceed these (or the change should be a deliberate,
recorded bump to this file).

| Component | Package | Minimum validated version |
|---|---|---|
| C compiler | `clang` | 22.1.8 |
| Linker | `lld` (`ld.lld`) | 22.1.8 |
| Target triple | — | `x86_64-unknown-uefi` |
| Emulator | `qemu-system-x86` | 10.2.2 |
| Guest firmware (dev loop) | `edk2-ovmf` | 20260508-4 |
| Debugger | `gdb` | 17.1 |

## OVMF firmware paths

The `run` target needs a non-SEV/TDX, non-secure-boot OVMF code/vars
pair (Secure-Boot-capable variants also exist, typically named
`OVMF_CODE.secboot.fd`/`OVMF_VARS.secboot.fd` — not used for v1 per §10
decision #5). Where these live depends on the distro's packaging:

| Distro | Typical path |
|---|---|
| Fedora | `/usr/share/OVMF/OVMF_CODE.fd` / `OVMF_VARS.fd` |
| Debian/Ubuntu | `/usr/share/OVMF/OVMF_CODE.fd` / `OVMF_VARS.fd` (package `ovmf`) |

The `Makefile`'s `OVMF_CODE`/`OVMF_VARS` variables default to the
Fedora path above; override them if your install differs:

```sh
make OVMF_CODE=/path/to/OVMF_CODE.fd OVMF_VARS=/path/to/OVMF_VARS.fd run
```

QEMU machine type: `q35` (validated against `pc-q35-9.2`; any `q35`
revision that still supports this project's feature set — AHCI/fw_cfg/
nested SVM under `-cpu host` — should work).

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

## Installing this toolchain

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
