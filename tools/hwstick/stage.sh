#!/bin/bash
# Stage this directory onto a mounted hype hardware-validation stick.
#
#   tools/hwstick/stage.sh /run/media/$USER/HYPEHW
#
# Refuses rather than guesses, in three places, because the target machine's only internal NVMe is
# the operator's BitLocker Windows install and a bad stick costs a cold-boot cycle:
#
#   1. any `target_disk = physical:` in either config aborts the stage;
#   2. every `kernel =` path named by a config must exist on the stick afterwards;
#   3. the staged BOOTX64.EFI must md5-match build/hype.efi.
#
# It also CLEARS the previous run's logs and varstores. A stale HYPE.LOG is worse than no log: it is
# indistinguishable from a run that produced nothing.
set -eu
cd "$(dirname "$0")/../.."
DST="${1:-}"
[ -n "$DST" ] && [ -d "$DST" ] || { echo "usage: $0 <mounted-stick-path>"; exit 2; }
[ -f build/hype.efi ] || { echo "build/hype.efi missing -- run 'make all'"; exit 2; }
for k in hello faulter ram1 cpumsr fwcfg intdeliver pausespin ps2 pflash pci; do
    [ -f "build/micro/$k.bin" ] || { echo "build/micro/$k.bin missing -- run 'make micro'"; exit 2; }
done

# 1. safety: no physical target, comments stripped first (';' begins a comment -- spec section 3)
python3 - "$@" <<'PY'
import re, sys, glob, os
bad = []
for f in sorted(glob.glob(os.path.join(os.path.dirname(__file__) if False else 'tools/hwstick', '*.cfg'))):
    for n, line in enumerate(open(f), 1):
        if re.search(r'target_disk\s*=\s*physical:', line.split(';', 1)[0]):
            bad.append((f, n))
if bad:
    sys.exit("REFUSING TO STAGE: physical: target at %s" % bad)
print("safety: no physical: target in any config -- OK")
PY

# Clear the previous run's evidence and state. The glob was `vars-vm*.bin`, which missed
# `vars-intdeliver.bin` -- #441 names a varstore after the VM's SECTION, not vmN, so a stale one
# survived a stage that claimed to clear varstores. Per-VM logs are named after the VM too, so the
# same mistake applied to them.
rm -f "$DST"/HYPE.LOG "$DST"/*.LOG "$DST"/vars-*.bin
# Stale input scripts are worse than none: a leftover \input\vm8.txt from a different config looks
# like a script that should be driving something. Replace the directories wholesale.
rm -rf "$DST"/input "$DST"/input-micro
# \iso and \hype\disks are created empty so the operator's copy/truncate commands land
# somewhere. Their CONTENTS are checked below and never invented.
mkdir -p "$DST/EFI/BOOT" "$DST/EFI/hype/micro" "$DST/input" "$DST/input-micro" \
         "$DST/iso" "$DST/hype/disks"
cp build/hype.efi "$DST/EFI/BOOT/BOOTX64.EFI"
cp fw/OVMF_CODE.fd fw/OVMF_VARS.fd "$DST/EFI/hype/"
cp build/micro/*.bin "$DST/EFI/hype/micro/"
cp tools/hwstick/hype.cfg tools/hwstick/hype-micro.cfg tools/hwstick/README.md "$DST/"
cp tools/hwstick/input/*.txt "$DST/input/"
cp tools/hwstick/input-micro/*.txt "$DST/input-micro/"
sync

# 3. the binary that will actually run
a=$(md5sum build/hype.efi | cut -d' ' -f1)
b=$(md5sum "$DST/EFI/BOOT/BOOTX64.EFI" | cut -d' ' -f1)
[ "$a" = "$b" ] || { echo "STAGE FAILED: BOOTX64.EFI md5 differs from build/hype.efi"; exit 1; }
echo "staged: $(LC_ALL=C strings build/hype.efi | grep -oE 'hype: build [0-9a-f]+(-dirty)?' | head -1)"

# 2. every kernel a config names must be there
python3 - "$DST" <<'PY'
import re, sys, os, glob
dst = sys.argv[1]
for cfg in sorted(glob.glob('tools/hwstick/*.cfg')):
    for m in re.findall(r'^kernel = (.+)$', open(cfg).read(), re.M):
        p = os.path.join(dst, m.strip().replace('\\', '/').lstrip('/'))
        if not os.path.exists(p):
            sys.exit("STAGE FAILED: %s names %s, which is not on the stick" % (cfg, m.strip()))
    print(os.path.basename(cfg), "-- every kernel present")
PY
# 4. every install_media and target_disk a config names, checked -- and the severity depends on the
# VM's own `boot` mode, which the first two versions of this check both got wrong.
#
#   install_media missing, boot = installer  -> FATAL. No boot media at all, so that guest cannot
#                                               start and the ticket riding on it produces nothing.
#   target_disk missing, boot = disk         -> FATAL. #120's VM has no media to fall back to: the
#                                               disk IS its boot device. hype refuses to substitute
#                                               a scratch (correctly), so the guest simply does not
#                                               run. This case did not exist when the check was
#                                               written and would have been reported as a warning.
#   target_disk missing, boot = installer    -> a WARNING. hype says "refusing to substitute a
#                                               scratch disk" and the guest still boots its media
#                                               and runs normally -- verified: a rehearsal with no
#                                               vdisks reached "Mounting boot media: ok" on both
#                                               guests. Only the ability to INSTALL is lost, which
#                                               most of these runs do not exercise.
#
# Nothing is created. A disk's size is the operator's call (it has to hold a real install, and this
# stick is FAT32, where nothing is sparse and no file may reach 4 GiB), and guessing it is exactly
# the kind of substitution the rest of this file refuses to make. The one exception is the #120
# disk, which is not a blank image but a specific bootable artefact -- so the message names the
# script that builds AND control-boots it rather than a `truncate` that would produce something
# unbootable.
python3 - "$DST" <<'PY'
import re, sys, os, glob

dst = sys.argv[1]

def sections(text):
    """[(section-name, {key: value})] -- per-section, because severity depends on `boot`."""
    out, cur, keys = [], None, {}
    for line in text.splitlines():
        line = line.split(';')[0].strip()
        if not line:
            continue
        if line.startswith('[') and line.endswith(']'):
            if cur is not None:
                out.append((cur, keys))
            cur, keys = line[1:-1], {}
        elif '=' in line and cur is not None:
            k, v = line.split('=', 1)
            keys[k.strip()] = v.strip()
    if cur is not None:
        out.append((cur, keys))
    return out

fatal, warn = [], []
for cfg in sorted(glob.glob('tools/hwstick/*.cfg')):
    for name, keys in sections(open(cfg).read()):
        boot = keys.get('boot', '')
        for key in ('install_media', 'target_disk'):
            val = keys.get(key)
            if val is None:
                continue
            if key == 'target_disk':
                if not val.startswith('file:'):
                    continue      # a physical: target is a real drive, not a file to stage
                val = val[len('file:'):]
            rel = val.replace('\\', '/').lstrip('/')
            if os.path.exists(os.path.join(dst, rel)):
                continue
            row = (os.path.basename(cfg), name, key, val)
            if key == 'install_media' or boot == 'disk':
                fatal.append(row)
            else:
                warn.append(row)

if warn:
    print("WARNING -- no target disk, so these VMs cannot INSTALL (they still boot their media):")
    for cfg, name, key, path in warn:
        print("  %-16s %-10s %-14s %s" % (cfg, name, key, path))
    print("  create with e.g.  truncate -s 2G %s/hype/disks/vm0.img" % dst)
    print("  (size is yours to choose; this stick is FAT32, so nothing is sparse and 4 GiB is the")
    print("   per-file ceiling)")
if fatal:
    print("STAGE FAILED -- these VMs have no boot device at all and cannot start:")
    for cfg, name, key, path in fatal:
        why = "boot = disk, so this disk IS the boot device" if key == 'target_disk' \
              else "boot = installer with no media"
        print("  %-16s %-10s %-14s %s   (%s)" % (cfg, name, key, path, why))
    print()
    print("  installer media:  cp <alpine.iso> %s/iso/<name>.iso" % dst)
    print("  a #120 boot disk: tools/make-guest-disk-from-iso.sh <hybrid.iso> \\")
    print("                        %s/hype/disks/<name>.img" % dst)
    print("                    -- it control-boots the image in bare QEMU first and refuses to")
    print("                       produce one that does not reach a login prompt, because a")
    print("                       `truncate`d blank here would give the guest nothing to boot.")
    sys.exit(1)
if not fatal and not warn:
    print("every install_media and target_disk named by a config is present")
PY
