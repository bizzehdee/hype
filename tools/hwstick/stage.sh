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
# 4. every install_media and target_disk a config names must be there too.
#
# stage.sh checked `kernel =` paths and not these, which is the wrong half: a missing kernel
# is a config hype refuses loudly, but a missing install_media or target_disk costs a whole
# cold-boot cycle to discover. hype does NOT invent a disk -- "refusing to substitute a scratch
# disk" is correct behaviour, because silently inventing one is how an install lands somewhere
# nobody chose -- so an absent file means that VM simply does not run, and the ticket riding on
# it produces no evidence at all.
#
# These are CHECKED, not created. Their size is the operator's call (it has to hold a real
# install, and this stick is FAT32, where nothing is sparse), and guessing it is exactly the
# kind of substitution the rest of this file refuses to make.
python3 - "$DST" <<'PY'
import re, sys, os, glob
dst = sys.argv[1]
missing = []
for cfg in sorted(glob.glob('tools/hwstick/*.cfg')):
    text = open(cfg).read()
    for key, pat in (('install_media', r'^install_media = (.+)$'),
                     ('target_disk', r'^target_disk = file:(.+)$')):
        for m in re.findall(pat, text, re.M):
            rel = m.strip().replace('\\', '/').lstrip('/')
            if not os.path.exists(os.path.join(dst, rel)):
                missing.append((os.path.basename(cfg), key, m.strip()))
if missing:
    print("STAGE INCOMPLETE -- these VMs will NOT run:")
    for cfg, key, path in missing:
        print("  %-16s %-14s %s" % (cfg, key, path))
    print()
    print("Put them on the stick before booting. The ISOs are the Alpine install media;")
    print("the target disks must exist because hype refuses to substitute a scratch disk.")
    print("  cp <alpine.iso> %s/iso/test.iso" % dst)
    print("  cp <alpine.iso> %s/iso/vm1.iso" % dst)
    print("  truncate -s 2G %s/hype/disks/vm0.img   # size is yours to choose; FAT32 caps at 4G" % dst)
    print("  truncate -s 2G %s/hype/disks/vm1.img" % dst)
    sys.exit(1)
print("every install_media and target_disk named by a config is present")
PY
