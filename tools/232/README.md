# #232 -- the `hype-additions` companion ISO

Design decided in `plan.md` section 10, decision 70 (2026-08-25): the additions
payload is a **separate** disc from the OS installer, attached as a second
`cdroms` entry (`docs/hype-cfg-spec.md` §5.7 already allows several per VM --
no hype-side change needed, only content). This generalizes #228's proven
Alpine-only recipe (a remastered installer ISO) to #146's mixed-distro case and
to BSD/Windows, which #228 could not do by construction.

```
tools/232/
  linux/    -- Alpine (and, per #146, other distros later) offline install
  bsd/      -- FreeBSD unattended install via bsdinstall's installerconfig
  windows/  -- Windows unattended install via autounattend.xml
  build-additions-iso.sh  -- assembles all three trees into hype-additions.iso
```

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

## Open item, NOT resolved by this design (see plan.md decision 70)

**How does the primary boot medium learn to look at the second CD at all?**
FreeBSD's `installerconfig` and Windows' `autounattend.xml` both scan every
attached medium for their answer file by design -- `cdroms[0]` stays a
bone-stock, unmodified ISO for both of those. Alpine does not have an
equivalent "scan everything" behavior for a live/install boot; #228 worked
around this by remastering the boot ISO itself. A genuinely separate-ISO Linux
flow needs Alpine's own live-boot kernel parameters (`alpine_repo=`,
`apkovl=`) pointed at the second CD-ROM device -- standard, documented Alpine
functionality, not yet wired into a hype install workflow. Tracked as
follow-up, not implemented here.

## Status

Content builders and installer scripts below are written and internally
reviewed against each platform's own documented unattended-install mechanism,
**but not yet run in QEMU or on real hardware** -- unlike #228's Alpine recipe,
which was validated end to end before this ticket started. Treat the BSD and
Windows legs especially as a first draft to validate, not a proven recipe.
