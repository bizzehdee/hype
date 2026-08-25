# Staging layout for the 2026-08-25 on-hold clear-down

**Status: staged onto the real drive on 2026-08-25.** `/dev/sdd` (serial
`115E0735191800123920`) is a two-partition drive: `sdd1` FAT32 (label
`HYPEBOOT`, boot ESP) + `sdd2` exFAT (label `EADE-CA36`, all vdisks/ISOs).
Full reasoning for every boot is in `docs/hw-val-runbook-2026-08-25.md` —
this directory is the artifacts that runbook points at, and this file is
what actually landed where.

## Input-scripts: how they actually get picked up (read this before booting)

**There is no cfg key for this at all.** hype auto-loads `\input\vm<N>.txt`
per VM, where `<N>` is that VM's **0-based position in the config file** —
NOT its `[vm.name]`. The first `[vm.*]` section is always `vm0`, regardless
of what it's called. **Absent file = no scripted input at all, silently and
harmlessly** (not an error) — the VM just runs with nothing typing into its
console automatically.

Because every one of these configs' input needs are different but the
filename hype looks for is always the same (`vm0.txt`, or `vm0.txt`+`vm1.txt`
for hype3c.cfg's two VMs), **the right script has to be copied into `\input\`
fresh before each specific boot** — they can't all sit there under their
real names at once. Staged as `input-<id>/` folders, one per boot that needs
scripting:

```sh
# before booting hypeNN.cfg, on the actual machine:
rm -rf \input
cp -r \input-NN \input      # e.g. \input-1a -> \input for hype1a.cfg
```

| boot | needs scripting? | folder |
|---|---|---|
| 1a | yes | `input-1a/vm0.txt` (reboot-pin) |
| 1b, 2d | no (suite-603.cfg's own microtests self-verify) | — |
| 1c | yes | `input-1c/vm0.txt` (exfat-write-test) |
| 2a | yes | `input-2a/vm0.txt` (login-only) |
| 2b | yes | `input-2b/vm0.txt` (reboot-pin) |
| 2c | yes | `input-2c/vm0.txt` (login-only) |
| 2e | no (observed hang, not scripted) | — |
| 2f | yes | `input-2f/vm0.txt` (exfat-write-test) |
| 3a | no (interactive/self-contained) | — |
| 3b | yes | `input-3b/vm0.txt` (usbsata-triple-write) |
| 3c | yes, TWO scripts | `input-3c/vm0.txt` (nvme-dd-write) + `input-3c/vm1.txt` (ahci-dd-write) -- vm0/vm1 order matches the `[vm.run3c-nvme]`/`[vm.run3c-ahci]` order in hype3c.cfg itself |
| 4a, 4b | no (interactive: Setup/OOBE, console-switching) | — |

## No pre-installed disk images needed -- corrected from this file's first draft

Every Alpine/FreeBSD boot below (1a, 1c, 2a, 2b, 2c, 2e, 2f, 3b, 3c, and the
Linux/BSD legs of 4b) boots the **live install ISO fresh**, same proven shape
as `tools/525`'s own rig (root login is passwordless in Alpine's live mode) —
NOT a pre-installed disk. `input-scripts/reboot-pin.txt` is a direct copy of
that proven script. This eliminates the earlier plan's biggest blocker
entirely: there is nothing to "pre-install" for these two OSes at all. Each
VM still carries a small `target_disk` scratch file (created on demand by
hype itself, exFAT has `HYPE_FS_CAP_WRITE_GROW`) purely to satisfy
admission's "needs a disk" check — never actually installed to.

**Windows is the one real exception** (#442/#634/#635/#636's Windows leg):
there is no live-boot equivalent that reaches OOBE, so `hype4a.cfg` performs
a genuine install to `\hype\disks\windows.img`, which `hype4b.cfg` then reuses.

## Drive layout (as staged)

```
sdd1 (HYPEBOOT, FAT32):
  \EFI\BOOT\BOOTX64.EFI        <- ACTIVE binary, currently hype-default.efi
  \EFI\hype\hype-default.efi   <- the default build (banner 976e71a-dirty)
  \EFI\hype\hype-apicv.efi     <- the -DHYPE_ENABLE_APICV=1 build, ONLY for 2c
  \EFI\hype\OVMF_CODE.fd, OVMF_VARS.fd
  \EFI\hype\micro\{vmexit,vmexitstorm,hello}.bin
  \hype1a.cfg ... \hype4b.cfg  <- copy whichever is next to \hype.cfg
  \input-1a\ ... \input-3c\    <- per-boot input-script folders, see table above
  \RUNBOOK-README.md, \hw-val-runbook-2026-08-25.md  <- reference copies

sdd2 (EADE-CA36, exFAT), referenced from every cfg via source_disk/media_disk
= 115E0735191800123920 (the DRIVE's serial -- hype's own resolver walks that
drive's partitions to find the path, no partition number needed):
  \iso\test.iso             <- Alpine standard 3.21.7
  \iso\windows.iso           <- Win11 24H2
  \iso\freebsd-install.iso   <- FreeBSD 15.0-RELEASE disc1
  \hype\disks\               <- scratch files land here on first boot of each
                                config; windows.img is the one hype4a.cfg
                                creates deliberately and hype4b.cfg keeps
```

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
