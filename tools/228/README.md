# #228 — unattended guest install to a physical target

Builds a seeded Alpine ISO that installs itself, with no operator input, onto
whatever disk hype presents to the guest as `/dev/vda`. Used to prove hype's
install-to-physical-disk path end to end.

```sh
./build-offline-repo.sh     # offline apk repo (needs podman)
./make-install-iso.sh       # remaster alpine-standard with repo + apkovl
./run-bare-install.sh       # bare QEMU: prove the ISO before involving hype
./run-boot-installed.sh     # bare QEMU: boot the result, no CD, fresh vars
```

Artifacts land in `$HYPE_228_BUILD` (default `disk-images/hype-228-build`).
Do **not** put them in a scratchpad — an earlier set was lost to cleanup.

## Validated (bare QEMU, 2026-07-30)

| step | result |
|---|---|
| unattended install | `setup-alpine rc=0`, "Installation finished. No error reported." |
| on-disk layout | GPT: `vda1` ESP vfat, `vda2` swap, `vda3` ext4 root |
| bootloader | `grub-install --removable` → `EFI/boot/bootx64.efi` |
| boot from disk, no CD, fresh vars | GRUB 2.12 → kernel 6.12.98-0-lts → `EXT4-fs (vda3): mounted` → `hype-guest login:` |
| re-wipe guard | live ISO re-booted with the installed disk attached → refused, disk byte-identical |

## Things that cost real debugging time

**The modloop is the whole ballgame.** Under an apkovl boot the `modloop`
service does not run, so `/lib/modules` is empty and nothing is loadable. Worse,
`setup-alpine` re-enters the `default` runlevel partway through and *stops*
modloop again (look for `Call to flock failed` next to `Unmounting /.modloop`).
`autoinstall.start` therefore starts modloop explicitly, preloads every module it
needs, and keeps a watchdog that re-mounts it whenever it disappears. Symptoms
when this is wrong, neither of which names the real cause:

- `mount: mounting /dev/vda3 on /mnt failed: No such device` — no ext4 module.
- `mount: mounting /dev/vda1 on /mnt/boot/efi failed: Invalid argument` — no NLS
  codepage module for the bare `mount -t vfat` that setup-disk issues.

**`modprobe a b c` does not load three modules.** Arguments after the first are
module *parameters*; the kernel says `vfat: unknown parameter 'ext4' ignored` and
you get a silent absence later. One `modprobe` per module.

**apk reads only `<repo>/<arch>/APKINDEX.tar.gz`.** Every package, including
`arch = noarch` ones, must be present *and indexed* in `x86_64/`, exactly as
Alpine's own mirrors do it. A noarch package filed only under `noarch/` reports
`<pkg> (no such package)`, which then prompts and hangs the unattended run.

**Resolve the dep closure by installing, not by fetching.** `apk fetch
--recursive` misses `install_if` dependencies. Install into a throwaway root,
then fetch the resulting set by name.

**`kbd-bkeymaps` is not in the closure** but `setup-keymap` needs it. Add it
explicitly or the first step prompts.

**Every answerfile key must be set.** Any unset option prompts, and a prompt is
an unattended hang. `setup-alpine` is wrapped in `timeout` so that becomes a
diagnosable failure with a complete log and a clean poweroff instead.

**setup-disk unmounts the target when it finishes**, so `/mnt` is empty
afterwards. The post-install step re-mounts the root and ESP by *discovering*
them with `blkid` (busybox `blkid` has no `-o value -s TYPE`; parse its default
output) rather than hardcoding partition numbers.

**Use `grub-install --removable`.** The guest OVMF has no NVRAM boot entry, so
only `EFI/BOOT/BOOTX64.EFI` is tried; a hand-copied `grubx64.efi` lands you in a
bare `grub>` shell because the prefix is wrong.

## Safety

The installer wipes its target. Four independent guards, in the order they fire:

1. Refuses unless the root filesystem is a live one (`tmpfs`/`overlay`/`squashfs`).
2. Refuses if `/etc/hype-228-installed` is present.
3. Deletes itself from the target after a successful install, and leaves that marker.
4. Refuses if the **target disk** already carries `/etc/hype-228-installed`.

Guard 4 is the one that matters under hype, and the reason the first three are
not enough: hype keeps the seeded ISO attached, so after a successful install the
*live ISO* can boot again — a fresh tmpfs root that sails past guards 1 and 2 and
would re-wipe the disk on every boot. Verified: byte-identical disk after such a
boot. Override with a file named `HYPE228_FORCE` on the boot medium.
