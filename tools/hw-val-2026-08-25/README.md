# Staging layout for the 2026-08-25 on-hold clear-down

**Status: staged onto the real drive on 2026-08-25.** `/dev/sdd` (serial
`DB9876543214E` — see the serial note below, it is NOT what Linux reports)
is a two-partition drive: `sdd1` FAT32 (label
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

## Guest device names in the input-scripts -- also corrected after Boot 2a

The same class of mistake as the serial: the scripts named guest block devices
by assumption (`/dev/vdb` everywhere) instead of deriving them from how hype
actually presents each disk. Derived from `boot/main.c` and now recorded in
each script's own header:

| boot | cfg entry | front-end | guest device |
|---|---|---|---|
| 1c, 2f | `[disk.exfatscratch]`, no `bus`, `partition = 1` | virtio-blk (os_hint=linux default) | `/dev/vda` — the WHOLE device; a `partition =` exposure has no table inside it, so there is no `vda1` |
| 3b | `[disk.usbwhole]`, no `bus`, `partition = whole` | virtio-blk | `/dev/vda1`, `vda2`, `vda3` |
| 3c vm0 | `[disk.nvmescratch]`, `bus = nvme` | NVMe (PCI dev 5) | `/dev/nvme0n1` |
| 3c vm1 | `[disk.ahciscratch]`, `bus = ahci-sata` | ICH9 AHCI, plain-ATA sig | `/dev/sda` (the install ISO is a separate ATAPI HBA → `sr0`) |

Slot 0 in every case: an explicit `bus` on the VM's **first** attached disk
wins (`fw_1_target_bus`, #202), and `disks = <one entry>` makes it slot 0.
Each script now prints a `BLK-` line listing what actually appeared before
anything depends on the name, so a wrong guess shows up in the log instead of
producing a silent no-op `dd`.

**hype3b.cfg was restructured, not just renamed.** It attached three physical
`[disk.*]` entries; hype presents at most ONE physical-backed disk per VM —
`boot/main.c`'s extra-slot loop skips any slot past 0 whose backing is
physical ("only the FIRST attached disk may be physical … NOT presented"), so
ext4 and NTFS would have been silently absent and two thirds of #688/#689's
bar unproven. It now attaches the drive whole.

**`allow_overwrite = true` added to every physical entry** (1c, 2f, 3b, 3c):
#124's non-empty-partition-table guard refuses a real drive without it. That
is *on top of* the runtime dashboard confirm (#125) — expect to approve each
physical target by hand at boot; no input-script can do that step, so budget
for it in those boots' run times.

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
= DB9876543214E (the DRIVE's serial -- hype's own resolver walks that
drive's partitions to find the path, no partition number needed):
  \iso\test.iso             <- Alpine standard 3.21.7
  \iso\windows.iso           <- Win11 24H2
  \iso\freebsd-install.iso   <- FreeBSD 15.0-RELEASE disc1
  \hype\disks\               <- scratch files land here on first boot of each
                                config; windows.img is the one hype4a.cfg
                                creates deliberately and hype4b.cfg keeps
```

## The drive serial is the BRIDGE's, not the disk's -- this broke Boot 2a

First attempt at Boot 2a failed with `No bootable option or device was found`
in the guest. Cause: every cfg had `media_disk`/`source_disk` set to
`115E0735191800123920`, the serial `lsblk`/`udev` report for `/dev/sdd`.
hype does not see that number at all. It identifies a USB mass-storage device
by **SCSI INQUIRY VPD page 0x80**, and this is a USB-SATA bridge, so the
*enclosure* answers, not the disk behind it:

```
host-xhci: MSC identity serial='DB9876543214E' source=inquiry-vpd80 [#340]
host-media: vm[0] 'run2a': media_disk = '115E0735191800123920' is not present
            -- refusing to stream media from a different drive
m5-8: media_disk = '115E0735191800123920' is not present -- refusing to
      resolve \hype\disks\run2a-scratch.img from a different drive
```

Neither the ISO nor the scratch disk resolved, so the VM came up with no boot
device — hype behaved correctly and said so twice; the config was simply
wrong. All cfgs now use `DB9876543214E`.

**Take the serial from hype's own log, never from the host OS.** Boot once,
read `media: registered host device N = ...` and the `MSC identity serial=`
line, and use exactly that string. The same applies to every placeholder
below: if a drive is behind any USB bridge, `lsblk`'s serial is the wrong one.

## Real hardware serials still needed -- genuinely can't be filled in from here

| placeholder | file(s) | what it is |
|---|---|---|
| `<EXFAT-SCRATCH-SERIAL>` | `hype1c.cfg` | AMD 5950X's sanctioned exFAT scratch drive |
| `<EXFAT-SCRATCH-SERIAL-INTEL>` | `hype2f.cfg` | Intel box's sanctioned exFAT scratch drive |
| `<STICK-B-SERIAL>` | `hype3a.cfg` | a SECOND USB stick, separate from this one |
| `<USB-SATA-SERIAL>` | `hype3b.cfg` | #218's own USB-SATA drive |
| `<AHCI-SCRATCH-SERIAL>` | `hype3c.cfg` | a sanctioned AHCI/SATA scratch drive |

`5ME3N005713803V2W` in `hype3c.cfg` is real (#715's own hwstick) and
`DB9876543214E` (this drive's own serial) is real and already filled
in everywhere else. **Standing exclusions from `docs/hw-val-211.md` apply to
every placeholder above**: never the BitLocker NVMe, never a known-dying
drive, by serial only, never by index.

## Re-staged for Run 1 on 2026-08-26 — the default binary MOVED

Run 2 (boots 2a-2e) ran on `976e71a-dirty`. Run 1 is staged on a fresh
**`4b62c48-dirty`** build, because **#728 landed in between** and #728 is
exactly the defect that makes Boot 1a's own script untrustworthy on the older
binary: the whole-screen matcher could satisfy `expect localhost login:`
against the *previous* boot's banner and PASS a guest that never restarted
(that is how Boot 2b false-passed). Running 1a's reboot-pin on a pre-#728
binary risks a false PASS of precisely the class just fixed. #727 rode the
same rebuild.

The old binary is kept on the drive as `\EFI\hype\hype-976e71a.efi` so Run 2's
results stay reproducible. `hype-apicv.efi` is untouched and is STILL the old
tree — rebuild it before Boot 2c if 2c is re-run, or its A/B against the
default build compares two different trees as well as two different flags.

All Run 2 logs were archived to `logs/` (verified by hash, including two that
had not been captured: the failed first 2a attempt and the #387 regression
rerun) before the drive's logs were cleared. Stale `vars-*.bin` and
`HYPE.BOOTCOUNT` were cleared too, so 1a starts from a clean log rotation.

## Currently staged: the 2026-08-27 bugfix sweep, boot 1 (SUPERSEDES the section below)

Re-staged 2026-08-27 from commit `f61f43e`. `\hype.cfg` is `hype1a.cfg` again --
but **1535 bytes now, not 1269**: the #738 comment correction changed it, and the
byte count in the `cfg: loaded` line is the cheapest way to tell which version a
boot actually read.

What changed on the drive relative to the 2026-08-26 staging:

- `\EFI\BOOT\BOOTX64.EFI` + `\EFI\hype\hype-default.efi` = the new default
  build (`sha256 55b38969...`), carrying #732, #734, #737, #739, #729 and #640's
  AVIC counters.
- `\EFI\hype\hype-avic.efi` = the `-DHYPE_ENABLE_AVIC=1` build
  (`sha256 a9d33709...`), for boot 2 -- **both banners read `f61f43e-dirty`**, so
  the banner cannot distinguish them; use the SHA-256 or the `ENABLING` line.
  The stale `hype-apicv.efi` and `hype-976e71a.efi` were removed.
- `\hype\disks\run1a-scratch.img` -- **2 GiB, fully allocated, finally present.**
  This is #738: hype creates nothing, and its absence is why both 2026-08-26 boots
  ran with no SATA disk attached.
- All 18 configs refreshed (the #738/#740 comment corrections).
- `\RUN-CARD.md` and `\QUEUE-2026-08-27.md`: the per-ticket read-list for both
  boots. `docs/hw-validation-queue-2026-08-27.md` is the repo copy.
- Previous run's `\HYPE.LOG` and `\RUN1A.LOG` cleared, md5-verified identical to
  `logs/1a-desktop-5950x/` first.

**The drive dropped off the USB bus during this staging** (09:05:07: `USB
disconnect`, `Synchronize Cache(10) failed: hostbyte=DID_ERROR`, then a
re-enumeration as a new SuperSpeed device, and exFAT flagged the volume dirty).
Every staged file was then re-verified by re-reading it from the media after an
unmount/mount cycle -- SHA-256s, all 18 configs byte-for-byte, and the scratch
image's full-length cksum -- so the staged data is good. `fsck.exfat /dev/sdb2`
has NOT been run; it needs root. A bridge that drops a SuperSpeed link under
sustained write is worth remembering when reading I/O oddities out of any log
this drive produced.

Boot 1 also needs two PHYSICAL conditions that no staging can set: the keyboard
and mouse stay on the USB 2.0 hub (#734/#737), and the two SuperSpeed hubs on
controller 2 ports 7 and 8 stay connected with something behind at least one
(#739).

## Historical: Boot 1a alone (2026-08-26)

`\hype.cfg` is `hype1a.cfg` — the single Alpine VM — and `\input\vm0.txt` is
the reboot-pin script. No `\hype-state.txt`: absent means every VM starts, and
there is only one.

**Target host: a dedicated 4-core AMD box, not the 5950X.** 1a asks for two
whole physical cores and core selection excludes the BSP's core entirely (both
threads), so a 4-core host offers 3 usable and 2 fits. A dedicated machine also
gives #641's 30-minute idle window the quiet it actually needs.

**1b still needs the 5950X** — 2+1+1 = 4 physical cores, and a 4-core host has
only 3 after the BSP core goes. Re-stage `hype1b.cfg` as `\hype.cfg` for that
boot; it needs no input script.

`hype1ab.cfg` + `hype-state-1ab.txt` (both VMs on one host, micro VMs held off
by the #177 run-state record and started by hand) are kept here as a worked
alternative but are **not staged**. Verified against the real parsers when
written: the record resolves to `run1a`=RUNNING and three STOPPED, and the
config to 4 VMs with `run1a` at index 0, passing admission at 16 cores.

**The drive is `sdc` today, not `sdd`.** It was `sdd` on 2026-08-25 and the
letter moved when the machine's USB enumeration changed. Identify it by
`HYPEBOOT` + `EADE-CA36` and serial `115E0735191800123920` (Linux) /
`DB9876543214E` (what hype's INQUIRY sees) — never by letter. This is the same
rule the cfgs already follow for write targets, and the reason for it.

## The drive layout below says `sdd` — it is whatever letter it enumerated as

Every `sdd1`/`sdd2` in this file means "the HYPEBOOT partition" and "the
EADE-CA36 partition" of the drive with serial `DB9876543214E`. Kept as written
because that is what the Aug 25 session saw; do not read it as a stable name.

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
