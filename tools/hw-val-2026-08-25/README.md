# Staging layout for the 2026-08-25 on-hold clear-down

One portable USB-SATA drive, moved between the AMD 5950X and Intel
i5-13420H as each run needs it. Full reasoning for every boot is in
`docs/hw-val-runbook-2026-08-25.md` — this directory is just the artifacts
that runbook points at.

## Drive layout

```
\EFI\BOOT\BOOTX64.EFI        <- ACTIVE binary (see "Two binaries" below)
\EFI\hype\hype-default.efi   <- the default build, kept here as backup/source
\EFI\hype\hype-apicv.efi     <- the -DHYPE_ENABLE_APICV=1 build, ONLY for 2c
\EFI\hype\micro\vmexit.bin        <- needed by hype1b.cfg / hype2d.cfg
\EFI\hype\micro\vmexitstorm.bin   <- needed by hype1b.cfg / hype2d.cfg
\EFI\hype\micro\hello.bin         <- needed by hype1b.cfg / hype2d.cfg
\hype.cfg                    <- copy whichever hypeNN.cfg is next in here
\hype\disks\alpine2vcpu.img    <- pre-installed 2-vCPU Alpine (1a, 2a, 2b, 2c, 3b, 3c, 4b)
\hype\disks\alpine2vcpu-2.img  <- SECOND copy, same install (3c's second VM only)
\hype\disks\freebsd.img        <- FreeBSD image reaching the boot loader (2e, 4b)
\hype\disks\windows.img        <- produced BY hype4a.cfg; reused by hype4b.cfg
\iso\test.iso                 <- Alpine install ISO (3a)
\iso\windows.iso              <- Windows install ISO (4a)
\input\run1a.txt   <- input-scripts/reboot-pin.txt
\input\run1c.txt   <- input-scripts/exfat-write-test.txt
\input\run2a.txt   <- input-scripts/login-only.txt
\input\run2b.txt   <- input-scripts/reboot-pin.txt   (same file as run1a.txt)
\input\run2c.txt   <- input-scripts/login-only.txt   (same file as run2a.txt)
\input\run2f.txt   <- input-scripts/exfat-write-test.txt (same file as run1c.txt)
```

`hype1b.cfg`/`hype2d.cfg`, `hype3a.cfg`, `hype4a.cfg`, `hype4b.cfg`,
`hype2e.cfg` have no input-script (interactive, or self-contained).

## Two binaries — only one boot needs the second one

Every boot except **2c** uses `hype-default.efi`. Before Boot 2c, and only
then: copy `hype-apicv.efi` over `\EFI\BOOT\BOOTX64.EFI`, run the boot, then
copy `hype-default.efi` back before continuing to 2d. Getting this backwards
silently invalidates whichever comparison you're trying to make — double
check the version banner in the serial log at the start of 2c's boot
matches the APICv build before trusting the result.

## Placeholders that need real values before staging

Every one of these is a real serial number, filled in from `lsusb`/`blkid`/
the drive's own label on a real OS — **never guess or reuse a value from a
different drive**:

| placeholder | file(s) | what it is |
|---|---|---|
| `<EXFAT-SCRATCH-SERIAL>` | `hype1c.cfg` | AMD box's sanctioned exFAT scratch drive |
| `<EXFAT-SCRATCH-SERIAL-INTEL>` | `hype2f.cfg` | Intel box's sanctioned exFAT scratch drive |
| `<STICK-B-SERIAL>` | `hype3a.cfg` | the SECOND USB stick (not the boot medium) |
| `<USB-SATA-SERIAL>` | `hype3b.cfg` | #218's own USB-SATA drive |
| `<AHCI-SCRATCH-SERIAL>` | `hype3c.cfg` | a sanctioned AHCI/SATA scratch drive |

`5ME3N005713803V2W` in `hype3c.cfg` is real (#715's own hwstick) — leave it
as-is. **Standing exclusions from `docs/hw-val-211.md` apply to every
placeholder above**: never the BitLocker NVMe, never a known-dying drive,
by serial only, never by index or by guessing which device node an OS
happened to assign this boot.

## Building the two binaries once, ahead of time

```sh
make all CC=clang LD=ld.lld                                    # -> hype-default.efi
EXTRA_CFLAGS="-DHYPE_ENABLE_APICV=1" make clean all CC=clang LD=ld.lld  # -> hype-apicv.efi
```

`make` ignores `EXTRA_CFLAGS` on an unchanged source mtime — the `clean`
before the second build is required, not optional (per this repo's own
toolchain notes). Confirm both banners differ (`hype: version ...`) before
copying either onto the drive.

## Pre-installing the reusable images

`alpine2vcpu.img`/`freebsd.img` need to exist BEFORE any of the runs above —
install each once with `boot = installer` against the same `target_disk`
path these configs use with `boot = disk`, then leave that install alone.
`windows.img` is the one exception: it does not exist yet, and Boot 4a is
what creates it.
