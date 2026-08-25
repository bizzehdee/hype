# #232 -- the `hype-additions` companion ISO

Design decided in `plan.md` section 10, decision 70 (2026-08-25): the additions
payload is a **separate** disc from the OS installer, attached as a second
`cdroms` entry (`docs/hype-cfg-spec.md` §5.7 already allows several per VM --
no hype-side change needed, only content). This generalizes #228's proven
Alpine-only recipe (a remastered installer ISO) to #146's mixed-distro case and
to BSD/Windows, which #228 could not do by construction.

```
tools/232/
  linux/    -- Alpine (built) -- other distro families are follow-up, see below
  bsd/      -- FreeBSD (first draft) -- other BSDs are follow-up, see below
  windows/  -- Windows (first draft, targets one version range) -- see below
  build-additions-iso.sh    -- assembles all trees into hype-additions.iso
  linux/make-bridge-iso.sh  -- the tiny two-line bootstrap for cdroms[0]
```

**Real target matrix (corrected 2026-08-25, plan.md decision 70's own scope
correction) -- directory names above are NOT the full scope:**

- **Linux**, every major package-manager family, not just Alpine: apt
  (Debian/Ubuntu), dnf (Fedora/RHEL), pacman (Arch), apk (Alpine). Each needs
  its OWN offline-repo + unattended-answer tooling -- `linux/build-repo.sh`
  and `linux/install-linux.sh` are the **apk/Alpine leg only**, not a
  template the others can share code with beyond the overall design pattern.
- **Windows 7 through 11** (and Server equivalents) -- `windows/autounattend.xml`
  as it stands targets one version range (UEFI/GPT-default, 10/11-era OOBE
  component names); 7/8.x need their own variant.
- **BSD** beyond FreeBSD: OpenBSD (`install.conf`), NetBSD (`sysinst`'s
  response file), DragonFlyBSD (its own installer). `bsd/installerconfig` is
  the **FreeBSD leg only**.

**Built and QEMU-validated today: Alpine only.** Everything else above is
unbuilt -- tracked in #725 and likely to become per-family sub-issues once
the Alpine pattern is proven enough to generalize from, not silently assumed
covered by a directory being named `linux/` or `bsd/`.

## Content manifest, and why each platform needs what it needs

| platform | drivers needed? | unattended mechanism | why |
|---|---|---|---|
| Linux (Alpine) | **yes** -- initramfs feature list | apkovl `local.d` script + offline apk repo | Alpine's installer kernel is generic, but the *installed* system's `mkinitfs` only bakes in modules for the features it's told about. #228 shipped a list curated against hype's own virtio disk, which then failed to boot on real SATA hardware (#226) -- fixed by using Alpine's own stock `sys-install` feature list verbatim: `ata base cdrom ext4 keymap kms mmc nvme raid scsi usb virtio`. |
| BSD (FreeBSD) | **no** | `bsdinstall` `installerconfig` | `os_hint = bsd` already defaults to `virtio-blk` + virtio-net (`docs/hype-cfg-spec.md` §5.6), both inbox in GENERIC since long before any FreeBSD release hype targets. |
| Windows | **no** | `autounattend.xml` | `os_hint = windows` deliberately defaults to `ahci-sata` + `e1000` (§5.6's own reasoning: "a virtio system disk is invisible at Windows install"), both inbox on every Windows version hype targets. This is the opposite of the usual `virtio-win` problem -- hype's own bus choice for Windows already avoids it. |

Windows needs something neither other platform does: an explicit serial-console
boot-config step (`bcdedit /ems` + `/emssettings COM1 115200`). Without it,
Setup is silent on the serial line hype's whole diagnostic pipeline
(`\HYPEFULL.LOG`, input scripts) depends on -- Linux/BSD installers already
default their console to whatever the kernel command line says, so this is
Windows-specific.

## Boot-bridge (resolved 2026-08-25)

FreeBSD's `installerconfig` and Windows' `autounattend.xml` both scan every
attached medium for their answer file by design -- `cdroms[0]` stays a
bone-stock, unmodified ISO for both. Alpine's live boot has no equivalent
"scan everything" behavior, so `cdroms[0]` for the Linux leg is a stock
`alpine-standard` ISO remastered with ONLY `linux/bridge-boot.start` -- a
two-line apkovl that finds the separate additions medium and `exec`s its
`install-linux.sh`. Built via `linux/make-bridge-iso.sh`, using the same
xorriso technique #228 proved, but injecting nothing else -- no repo, no
seed. All the substantive content stays on `hype-additions.iso` alone.

## Status

**Alpine leg: built and QEMU-validated under hype** (`tools/232/run-232-linux.sh`).
Every other family in the target matrix above (Debian/Fedora/Arch, Windows
7-11, OpenBSD/NetBSD/DragonFlyBSD) is a first draft or entirely unbuilt --
`bsd/installerconfig` and `windows/autounattend.xml` are written and
internally reviewed against each mechanism's own documentation but **not yet
run in QEMU or on real hardware**.
