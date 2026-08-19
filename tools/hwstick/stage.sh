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

rm -f "$DST"/HYPE.LOG "$DST"/VM0.LOG "$DST"/VM1.LOG "$DST"/vars-vm*.bin
mkdir -p "$DST/EFI/BOOT" "$DST/EFI/hype/micro" "$DST/input" "$DST/input-micro"
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
echo
echo "NOT staged (put them there yourself if the primary config needs them):"
echo "  \\iso\\test.iso and \\iso\\vm1.iso -- the Alpine media for vm0 and vm1"
ls -la "$DST/iso" 2>/dev/null | tail -3 || echo "  (no \\iso directory on this stick yet)"
