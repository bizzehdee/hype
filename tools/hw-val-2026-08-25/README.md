# Staging layout for the 2026-08-25 on-hold clear-down

**Status: staged onto the real drive on 2026-08-25.** This is not a plan any
more for the parts below marked done — `/dev/sdd` (serial
`115E0735191800123920`) is a two-partition drive: `sdd1` FAT32 (label
`HYPEBOOT`, boot ESP) + `sdd2` exFAT (label picked at format time, currently
`EADE-CA36`, all vdisks/ISOs). Full reasoning for every boot is in
`docs/hw-val-runbook-2026-08-25.md` — this directory is the artifacts that
runbook points at, and this file is what actually landed where.

## Drive layout (as staged)

```
sdd1 (HYPEBOOT, FAT32):
  \EFI\BOOT\BOOTX64.EFI        <- ACTIVE binary, currently hype-default.efi
  \EFI\hype\hype-default.efi   <- the default build (banner 976e71a-dirty)
  \EFI\hype\hype-apicv.efi     <- the -DHYPE_ENABLE_APICV=1 build, ONLY for 2c
  \EFI\hype\OVMF_CODE.fd, OVMF_VARS.fd
  \EFI\hype\micro\{vmexit,vmexitstorm,hello}.bin
  \hype1a.cfg ... \hype4b.cfg  <- copy whichever is next to \hype.cfg
  \input\run1a.txt, run1c.txt, run2a.txt, run2b.txt, run2c.txt, run2f.txt
  \RUNBOOK-README.md, \hw-val-runbook-2026-08-25.md  <- this file + the full runbook, for reference on-site

sdd2 (EADE-CA36, exFAT), referenced from every cfg via source_disk/media_disk
= 115E0735191800123920 (the DRIVE's serial, not a partition number -- hype's
own resolver walks that drive's partitions to find the path):
  \iso\test.iso        <- DONE (Alpine standard 3.21.7, from this repo's own disk-images/)
  \iso\windows.iso      <- DONE (Win11 24H2, from this repo's own disk-images/)
  \iso\freebsd-install.iso  <- DONE (FreeBSD 15.0-RELEASE disc1, staged but NOT referenced
                                by any cfg yet -- hype2e.cfg needs a disk IMAGE that has
                                already reached the boot loader, not raw install media;
                                use this ISO to produce \hype\disks\freebsd.img first)
  \hype\disks\           <- directory created, EMPTY -- see "Still needed" below
```

## Still needed before Runs 1/2/4 can boot -- real installs, not staging

These three images do not exist yet and need an actual install run each
(none of this is achievable unattended from a dev sandbox):

| image | needed by | how to produce it |
|---|---|---|
| `\hype\disks\alpine2vcpu.img` | 1a, 2a, 2b, 2c, 3b, 3c, 4b | install once with `boot=installer`, `vcpus=2`, against this path on `sdd2` (`source_disk = 115E0735191800123920`), then those configs' `boot=disk` reuses it |
| `\hype\disks\alpine2vcpu-2.img` | 3c (second VM) | a second, independent install the same way -- two VMs can't share one backing file concurrently |
| `\hype\disks\freebsd.img` | 2e, 4b | install (or at least reach the boot loader) from `\iso\freebsd-install.iso`, `os_hint=bsd` |

`\hype\disks\windows.img` is NOT in this list -- Boot 4a creates it itself
(`size_gb = 40` on its own `[disk.windisk]` entry).

## Real hardware serials still needed -- genuinely can't be filled in from here

| placeholder | file(s) | what it is |
|---|---|---|
| `<EXFAT-SCRATCH-SERIAL>` | `hype1c.cfg` | AMD 5950X's sanctioned exFAT scratch drive |
| `<EXFAT-SCRATCH-SERIAL-INTEL>` | `hype2f.cfg` | Intel box's sanctioned exFAT scratch drive |
| `<STICK-B-SERIAL>` | `hype3a.cfg` | a SECOND USB stick, separate from this one |
| `<USB-SATA-SERIAL>` | `hype3b.cfg` | #218's own USB-SATA drive |
| `<AHCI-SCRATCH-SERIAL>` | `hype3c.cfg` | a sanctioned AHCI/SATA scratch drive |

`5ME3N005713803V2W` in `hype3c.cfg` is real (#715's own hwstick) and
`115E0735191800123920` (this drive's own serial) is real and already filled
in everywhere else. **Standing exclusions from `docs/hw-val-211.md` apply to
every placeholder above**: never the BitLocker NVMe, never a known-dying
drive, by serial only, never by index.

## Two binaries — only one boot needs the second one

Every boot except **2c** uses `hype-default.efi`. Before Boot 2c, and only
then: copy `hype-apicv.efi` over `\EFI\BOOT\BOOTX64.EFI`, run the boot, then
copy `hype-default.efi` back before continuing to 2d. Getting this backwards
silently invalidates whichever comparison you're trying to make — double
check the version banner in the serial log at the start of 2c's boot
matches the APICv build before trusting the result. Both binaries were
verified to differ (distinct SHA-256) before staging.

## Rebuilding either binary later

```sh
make all CC=clang LD=ld.lld                                            # -> default
make clean && EXTRA_CFLAGS="-DHYPE_ENABLE_APICV=1" make all CC=clang LD=ld.lld  # -> APICv
```

`make` ignores `EXTRA_CFLAGS` on an unchanged source mtime — `make clean`
before switching variants is required, not optional. This bit us while
staging: a `make all` with no flags right after the APICv build silently
relinked nothing and left the APICv binary sitting at `build/hype.efi`
un-flagged as such. Always diff the SHA-256 (or the embedded `hype: build`
banner) against the other variant before trusting which one you have.

## Note on the FAT32 partition's prior contents

`sdd1` carried real prior hw-val work (#689 round 3, #710/#603 round 1,
physwrite round 2 -- `RUN-CARD.md`/`README.md` documented it in detail) before
this pass. Wiped with explicit authorization ("old run data"). Its
`\iso\test.iso` and `\hype\disks\apicv-test.img` were deleted along with
everything else before being individually re-checked -- `test.iso` was
trivially recovered from this repo's own cached Alpine ISO (byte-identical
source); `apicv-test.img`'s specific prior content was NOT preserved and is
not reconstructable from anything on hand. Worth a moment's thought if that
image mattered, though nothing in the current runbook depends on it.
