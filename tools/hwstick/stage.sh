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
DST=""
FAT32=0
for a in "$@"; do
    case "$a" in
        --fat32) FAT32=1 ;;   # #597: stage the FAT32 on-medium self-test instead of the default run
        *) DST="$a" ;;
    esac
done
[ -n "$DST" ] && [ -d "$DST" ] || { echo "usage: $0 [--fat32] <mounted-stick-path>"; exit 2; }
[ -f build/hype.efi ] || { echo "build/hype.efi missing -- run 'make all'"; exit 2; }
for k in hello faulter ram1 cpumsr fwcfg intdeliver pausespin ps2 pflash pci; do
    [ -f "build/micro/$k.bin" ] || { echo "build/micro/$k.bin missing -- run 'make micro'"; exit 2; }
done

# 1. safety: a physical target is allowed ONLY for operator-authorized spare-disk serials, both as
# `target_disk = physical:<serial>` and as a `[disk.*] backing=physical / id_match=<serial>`. Any
# other serial aborts the stage -- the by-serial allowlist is the same one hype auto-confirms, so
# staging and the hypervisor agree on exactly which drives may be written. Comments stripped first.
python3 - <<'PY'
import re, sys, glob
AUTHORIZED = {"5ME3N005713803V2W", "2132E5BF4EAE"}  # AMD laptop spare NVMe + SATA SSD
bad = []
for f in sorted(glob.glob('tools/hwstick/*.cfg')):
    text = "".join(line.split(';', 1)[0] for line in open(f))
    serials = re.findall(r'target_disk\s*=\s*physical:\s*(\S+)', text)
    serials += re.findall(r'\bid_match\s*=\s*(\S+)', text)
    for s in serials:
        if s not in AUTHORIZED:
            bad.append((f, s))
if bad:
    sys.exit("REFUSING TO STAGE: unauthorized physical: target serial(s) %s "
             "(authorized: %s)" % (bad, sorted(AUTHORIZED)))
print("safety: physical targets limited to authorized spare-disk serials -- OK")
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
# The physical-write NVMe VM direct-boots a real Linux kernel + initramfs (alpine-virt). These are
# not build artefacts, so they live under disk-images/; a config that names them fails the
# kernel-present check below if they are absent, which is the intended loud failure.
for k in vmlinuz-virt initramfs-virt; do
    if [ -f "disk-images/545/$k" ]; then
        cp "disk-images/545/$k" "$DST/EFI/hype/micro/$k"
    fi
done
if [ "$FAT32" -eq 1 ]; then
    # #597: the active \hype.cfg is the FAT32 self-test config, and the marker file that arms the
    # on-medium battery is present. The probe VM takes no input, so \input is left empty (no stale
    # physical-write script is fed to it).
    cp tools/hwstick/hype-fat32.cfg "$DST/hype.cfg"
    cp tools/hwstick/hype-micro.cfg tools/hwstick/README.md "$DST/"
    printf '%s\n' "#597 marker -- its presence makes hype run the FAT32 write battery on this volume." \
        > "$DST/F32TEST.RUN"
    rm -rf "$DST/F32TEST" # start the battery from an empty dir so a stale file can't mask a failure
    echo "fat32: staged \\hype.cfg = hype-fat32.cfg and dropped the \\F32TEST.RUN marker"
else
    cp tools/hwstick/hype.cfg tools/hwstick/hype-micro.cfg tools/hwstick/README.md "$DST/"
    cp tools/hwstick/input/*.txt "$DST/input/"
    rm -f "$DST/F32TEST.RUN" # a normal run must not accidentally trigger the battery
fi
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

# 5. A RUN CARD, GENERATED FROM WHAT IS ACTUALLY ON THE STICK.
#
# The stick used to carry a hand-written RUN-PLAN.md. By the next stage it named a build that was no
# longer there, a vm1 role that had changed and a suite verdict that had been fixed -- and it sat
# next to a freshly-copied README saying different things. That is the same drift #482/#576 are
# about, one directory up: two hand-maintained copies of one fact.
#
# So this is derived, every time, from the binary's OWN stamp and the config as staged. It cannot
# describe a stick other than this one. README.md stays the durable prose; this is the card you read
# standing at the machine.
python3 - "$DST" <<'PY'
import glob, os, re, subprocess, sys

dst = sys.argv[1]
efi = os.path.join(dst, 'EFI/BOOT/BOOTX64.EFI')
stamp = '(unreadable)'
try:
    out = subprocess.run(['strings', efi], capture_output=True, text=True).stdout
    m = re.search(r'hype: build [0-9a-f]+(?:-dirty)?', out)
    if m:
        stamp = m.group(0)
except Exception:
    pass

def sections(text):
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

cfg_path = os.path.join(dst, 'hype.cfg')
vms = [(n[3:], k) for n, k in sections(open(cfg_path).read()) if n.startswith('vm.')]
cores = sum(int(k.get('vcpus', '1')) for _, k in vms)
ram = sum(int(k.get('mem_mb', '0')) for _, k in vms)

rows = []
for name, k in vms:
    src = k.get('install_media') or k.get('target_disk', '').replace('file:', '') or k.get('kernel', '')
    rel = src.replace('\\', '/').lstrip('/')
    full = os.path.join(dst, rel)
    size = ('%d MiB' % (os.path.getsize(full) // 1048576)) if os.path.exists(full) else 'MISSING'
    rows.append('| `%s` | %s | %s | %s MB | `%s` | %s |'
                % (name, k.get('label', ''), k.get('vcpus', '1'), k.get('mem_mb', '?'),
                   k.get('boot', '?'), size))

card = """# Run card — %s

GENERATED by `tools/hwstick/stage.sh` from this stick's own binary and config. If it disagrees with
anything, believe the stick. `README.md` beside it is the durable prose.

**Build line: `make clean && make all && make micro` — default flags, no `EXTRA_CFLAGS`.**
#527's instruction to build `-DHYPE_SMP_STARTABLE_VCPUS=2` is stale: that knob has raised only the
NO-CONFIG default since #192, and this stick ships a config. Do not look for `fw-1: vm0 vcpus 2`
either — hype does not emit that line for a one-core config.

## What is on this stick

| VM | label | cores | RAM | boot | media/disk |
|---|---|---|---|---|---|
%s

**%d cores** (the AMD laptop's whole budget: 4 physical, one reserved for the BSP) and **%d MB** of
guest RAM.

## Gate the run on these two, before reading anything else

```
%s
fw-1 SMP: vm0 granted 1 whole physical core(s) -> 2 logical CPU(s)
```

A log with no `hype: build` banner is #371 — roughly one boot in four never reaches hype. That is
neither a pass nor a failure. Re-run it.

## Then, per ticket

```sh
LC_ALL=C grep -a "hype: build" HYPE.LOG                      # the binary that ran
LC_ALL=C grep -aE "WILL NOT RUN|adm: REFUSED" HYPE.LOG       # anything admission dropped
LC_ALL=C grep -aE "live=|VMCSRELOAD" HYPE.LOG | tail -20     # 527  both APs live=1, steals=0
LC_ALL=C grep -ac "soft lockup" VM0.LOG VM1.LOG              # 526  AMD baseline was zero
LC_ALL=C grep -a "vector=13" HYPE.LOG                        # 461  watch-only
LC_ALL=C grep -a "boot=disk" HYPE.LOG                        # 120  vm1 booted its own disk
LC_ALL=C grep -ac "0xCF9" HYPE.LOG                           # 525  must be 1, and must name a vCPU
LC_ALL=C grep -ac "vm1 restarted (M8-4)" HYPE.LOG            # 525  must be 1
LC_ALL=C grep -aE "VM1-REBOOTED-up|VM1-RENPROC" VM1.LOG      # 525  the guest came BACK
LC_ALL=C grep -a "PITROUTE" HYPE.LOG | tail -2               # 557  edges/coalesced/pic_delivered
LC_ALL=C grep -aoE "MICRO (PASS|FAIL): [a-z0-9]+" HYPE.LOG   # the suite
LC_ALL=C grep -a "MICRO SUITE:" HYPE.LOG                     # absent = the sweep was truncated
```

The logs are on the stick because this machine has no serial port, and they contain invalid UTF-8 —
`LC_ALL=C grep -a` or you will silently match nothing.

## Expected

- **all 8 microtests PASS.** Any FAIL is new: #556 is closed and #580 is fixed, so the old "pflash
  FAILS, intdeliver may FAIL" advice no longer holds.
- `intdeliver` PASSing says nothing about #557, which is a bare-metal ticket. Read its `PITROUTE`
  line: served shows `pic_delivered` climbing with `coalesced` accounting for the rest; starved
  shows `pic_delivered=0` **with `coalesced=0`**, and `mIMR`/`mISR` say which.
- **Copy `HYPE.LOG`, `VM0.LOG` and `VM1.LOG` off the stick before the next boot.** Staging clears
  them, and a stale log is worse than none.
""" % (stamp, "\n".join(rows), cores, ram, stamp)

open(os.path.join(dst, 'RUN-CARD.md'), 'w').write(card)
print("run card written: RUN-CARD.md (%s, %d VM(s), %d core(s))" % (stamp, len(vms), cores))
# A hand-written plan from an earlier build is the drift this card replaces.
stale = os.path.join(dst, 'RUN-PLAN.md')
if os.path.exists(stale):
    os.remove(stale)
    print("removed the stale hand-written RUN-PLAN.md (superseded by the generated card)")
PY
