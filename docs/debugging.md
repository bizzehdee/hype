# Debugging workflow (SETUP-5)

Two channels, per `plan.md` §11: GDB-over-QEMU as the primary iteration
loop, serial logging as the channel that also works on real hardware
(where GDB-over-JTAG isn't assumed available, per SETUP-4).

## Primary loop: QEMU `-s -S` + GDB

Build `hype.efi` with debug info (`-g`, no optimization for debug builds),
then:

```sh
qemu-system-x86_64 \
  -machine q35 \
  -m 512 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/usr/share/OVMF/OVMF_VARS.fd \
  -drive format=raw,file=fat:rw:esp \
  -serial stdio \
  -s -S
```

- `-s` — shorthand for `-gdb tcp::1234`.
- `-S` — freeze the vCPU at startup so GDB can attach before any code runs.
- `esp/` is a FAT-formatted directory QEMU exposes as a virtual FAT disk,
  containing `EFI/BOOT/BOOTX64.EFI` (the built `hype.efi`) — no need to
  hand-build a disk image for the dev loop.

In a second terminal:

```sh
gdb
(gdb) target remote :1234
(gdb) file build/hype.efi.debug   # symbols; the built PE32+ carries them too
(gdb) hbreak efi_main
(gdb) continue
```

Caveats specific to this target:
- OVMF relocates the loaded image; symbols only line up once execution
  reaches our own code. Set the breakpoint at `efi_main` (or via a symbol
  file loaded at the right offset) rather than trying to break before the
  firmware hands off.
- Post-`ExitBootServices()` (M1-4 onward) there's no OS underneath GDB is
  used to seeing — plain `hbreak`/`continue`/`stepi` over the remote target
  still works since QEMU's gdbstub operates below that layer, but OS-aware
  conveniences (thread listing, etc.) don't apply.

## Real hardware: serial logging

No GDB-over-JTAG assumed (SETUP-4 confirmed a serial fallback exists on
both Intel and AMD test machines, not a JTAG debugger). The serial console
driver (M1-5) is the only introspection available pre-M1 on real hardware;
before M1-5 exists, the SETUP-6 `ConOut`-based print primitive is the only
output. Capture with a null-modem cable / USB-serial adapter into a
terminal emulator (e.g. `screen /dev/ttyUSB0 115200` or `minicom`) on the
host driving the connection.

Practical flow: get a change working under QEMU+GDB first, then validate
on real hardware via serial log output alone — real hardware is not a
routine GDB-attach target for this project.
