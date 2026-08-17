#!/bin/bash
# Boot an arbitrary ISO as a hype guest under QEMU+KVM (nested SVM) and capture hype's serial.
#
#   tools/run-guest.sh <iso> <log-name> [seconds]
#
# Drops the freshly built hype.efi plus hype's vendored guest firmware onto a disk image with the
# ISO, and boots it. Used for the #166 FreeBSD work and for the Alpine regression check that any
# change to the shared interrupt-injection path needs.
#
# TWO DELIVERY MODES, because hype itself supports two and they have different limits. Both end
# up presenting the guest an ordinary ATAPI CD-ROM (`cd0: <HYPE VIRTUAL CD-ROM>`), streamed with
# no RAM copy -- the difference is only in how hype OBTAINS the ISO bytes. Selected by
# ISO_MODE=file|raw, defaulting to raw for ISOs FAT32 cannot hold:
#
#   file  (GLADDER-11 / #182)  GPT disk, partition 1 = FAT32 holding \iso\test.iso; hype resolves
#                             the file's extents and streams from the raw disk LBAs. Handles up to
#                             HYPE_FAT_MAX_EXTENTS extents (#327/#366); beyond that hype refuses,
#                             and says so by name.
#                             **Cannot carry an ISO >= 4 GiB: that is FAT32's max file size.**
#                             The GPT is not optional -- see build_esp_file().
#
#   raw   (GLADDER-10)        GPT disk, partition 1 = a small FAT ESP (hype.efi + firmware only),
#                             partition 2 = the raw ISO bytes, no filesystem in the path. hype's
#                             FAT-file scan finds no \iso\test.iso, falls through, GPT-locates
#                             partition 2 and verifies "CD001" at byte 32769. No size limit, and
#                             it does not copy a multi-GB file into a filesystem per run.
#
# Two traps this encodes, both of which cost a wasted run before being understood:
#
#  * The ESP must be an mtools-built FAT **image**, never QEMU's vvfat. vvfat SIGSEGVs QEMU
#    inside its own AHCI emulation while OVMF reads it (#288) -- which reads as a hype crash
#    and is not one.
#  * The HOST pflash varstore must be the pair of the HOST OVMF_CODE. Handing QEMU hype's own
#    vendored GUEST varstore (fw/OVMF_VARS.fd, for the 4MB build) against a 2MB host CODE
#    produces a 0-byte serial log, indistinguishable from hype faulting on entry.
set -e
cd "$(dirname "$0")/.."
ISO="$1"; NAME="${2:-guest}"; SECS="${3:-180}"
[ -n "$ISO" ] && [ -f "$ISO" ] || { echo "usage: $0 <iso> <log-name> [seconds]"; exit 1; }
[ -f build/hype.efi ] || { echo "build/hype.efi missing -- run make all"; exit 1; }

OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/ovmf/OVMF_CODE.fd}"
OVMF_VARS="${OVMF_VARS:-/usr/share/edk2/ovmf/OVMF_VARS.fd}"
OUT=disk-images/run-$NAME
LOG="$OUT.log"
ESP="$OUT.esp.img"

ISO_BYTES=$(stat -c%s "$ISO")
# 4 GiB - 1 is FAT32's maximum file size. At or above it the `file` mode cannot work at all, so
# default to `raw` rather than producing a truncated ISO and a mystery boot failure.
if [ -z "${ISO_MODE:-}" ]; then
    if [ "$ISO_BYTES" -ge 4294967295 ]; then ISO_MODE=raw; else ISO_MODE=file; fi
fi

# raw mode: GPT, partition 1 = small FAT ESP (firmware only), partition 2 = the raw ISO.
build_esp_raw() {
    local esp_mb=128 iso_mb start_iso
    iso_mb=$(( ISO_BYTES / 1048576 + 1 ))
    rm -f "$ESP"
    fallocate -l "$(( 1 + esp_mb + iso_mb + 1 ))M" "$ESP" 2>/dev/null || \
        dd if=/dev/zero of="$ESP" bs=1048576 count=$(( 1 + esp_mb + iso_mb + 1 )) status=none conv=fsync
    # sfdisk + mtools only, so this needs no root -- same constraint tools/262/make-rig.sh works
    # under. 1MiB alignment: partition 1 at LBA 2048, partition 2 right after it.
    start_iso=$(( 2048 + esp_mb * 2048 ))
    sfdisk --label gpt -q "$ESP" >/dev/null <<SFDISK
2048,$(( esp_mb * 2048 )),U
$start_iso,$(( iso_mb * 2048 )),L
SFDISK
    # The ESP partition, built standalone then written into place: mtools addresses a whole image,
    # not a partition within one.
    local espfs="$OUT.espfs.img"
    rm -f "$espfs"
    fallocate -l "${esp_mb}M" "$espfs" 2>/dev/null || \
        dd if=/dev/zero of="$espfs" bs=1048576 count="$esp_mb" status=none
    mformat -i "$espfs" -F ::
    mmd -i "$espfs" ::/EFI ::/EFI/BOOT ::/EFI/hype
    mcopy -i "$espfs" build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
    mcopy -i "$espfs" fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
    dd if="$espfs" of="$ESP" bs=1048576 seek=1 conv=notrunc,fsync status=none
    rm -f "$espfs"
    # The ISO, raw, at partition 2's first LBA.
    dd if="$ISO" of="$ESP" bs=1048576 seek=$(( 1 + esp_mb )) conv=notrunc,fsync status=none
    sync "$ESP"

    # Verify: "CD001" must be readable at ISO offset 32769 from partition 2's start, which is the
    # exact check hype itself makes. Trusting dd here would turn a short write into a boot mystery.
    local magic
    magic=$(dd if="$ESP" bs=1 \
                skip=$(( (1 + esp_mb) * 1048576 + 32769 )) count=5 status=none 2>/dev/null)
    [ "$magic" = "CD001" ] || { echo "raw ESP verify FAILED: no CD001 at partition 2 + 32769"; return 1; }
    mdir -i "$ESP@@1M" ::/EFI/BOOT 2>/dev/null | grep -q BOOTX64 || \
        { echo "raw ESP verify FAILED: BOOTX64.EFI missing"; return 1; }
    return 0
}

build_esp_file() {
    # ISO + firmware + slack, rounded up to whole MB, plus 1MiB for the GPT + alignment gap.
    local mb
    mb=$(( $(stat -c%s "$ISO") / 1048576 + 96 ))
    rm -f "$ESP"
    # FULLY ALLOCATED, not `truncate`. tools/make-disk-image.sh explains the general reason;
    # the reason here is narrower and was learned the hard way: a sparse ESP built and handed
    # to QEMU in the same breath intermittently came up as `BdsDxe: ... Not Found` / no boot
    # option at all, on an image mtools itself could list correctly. Allocating and then
    # syncing removes the variable rather than leaving a retry to paper over it.
    fallocate -l "$(( mb + 1 ))M" "$ESP" 2>/dev/null || \
        dd if=/dev/zero of="$ESP" bs=1048576 count=$(( mb + 1 )) status=none conv=fsync
    #
    # The image MUST be GPT-partitioned, not a bare FAT filesystem. hype's streaming resolver
    # locates the volume with hype_gpt_find_partition() before handing it to core/fat.c, so a
    # bare `mformat -i "$ESP" ::` image has no partition for it to find. This mode used to build
    # exactly that, which meant it silently fell back to the pre-EBS RAM load every run while
    # this file's own header claimed both modes streamed. It never streamed once. A missing
    # `host-fat: resolved ...` line is the only symptom, and there is no log line for a FAILED
    # resolve -- so absence looked like "nothing to see" rather than "wrong mode".
    #
    # Size omitted so sfdisk fills the rest of the disk: an explicit mb*2048 overruns the GPT
    # backup header at the end and sfdisk refuses ("last usable GPT sector is ...").
    sfdisk --label gpt -q "$ESP" >/dev/null <<SFDISK
2048,,U
SFDISK
    # mtools addresses the partition via @@1M (1MiB == LBA 2048, the alignment above). Without
    # the offset it reports "init :: non DOS media".
    mformat -i "$ESP@@1M" -F ::
    mmd -i "$ESP@@1M" ::/EFI ::/EFI/BOOT ::/EFI/hype ::/iso
    mcopy -i "$ESP@@1M" build/hype.efi ::/EFI/BOOT/BOOTX64.EFI
    mcopy -i "$ESP@@1M" fw/OVMF_CODE.fd fw/OVMF_VARS.fd ::/EFI/hype/
    mcopy -i "$ESP@@1M" "$ISO" ::/iso/test.iso
    # HYPE_CFG=<path> drops a config at \hype.cfg, which is where hype looks (boot/main.c:14699).
    # Without one hype uses its built-in defaults, so the entire config path -- and everything it
    # decides, from guest RAM to display names -- goes untested here unless a run asks for it.
    [ -n "${HYPE_CFG:-}" ] && mcopy -i "$ESP@@1M" "$HYPE_CFG" ::/hype.cfg
    # HYPE_INPUT=<path> drops a section 6k input script at \input\vm0.txt, so a run can drive the
    # guest -- log in, run a workload -- instead of only watching it boot. #295 needs one: its own
    # first instruction is to re-measure the write-size histogram, and nothing writes to a disk
    # unless something in the guest is told to.
    if [ -n "${HYPE_INPUT:-}" ]; then
        mmd -i "$ESP@@1M" ::/input 2>/dev/null || true
        mcopy -i "$ESP@@1M" "$HYPE_INPUT" ::/input/vm0.txt
    fi
    # HYPE_DISK=<path> drops a raw disk image at \hype\disks\<basename>, which a hype.cfg
    # `[disk.*] backing = file` entry can then point at.
    # Several images may be given, space separated, for a multi-disk config (#329).
    if [ -n "${HYPE_DISK:-}" ]; then
        mmd -i "$ESP@@1M" ::/hype 2>/dev/null || true
        mmd -i "$ESP@@1M" ::/hype/disks 2>/dev/null || true
        for _d in $HYPE_DISK; do
            mcopy -i "$ESP@@1M" "$_d" "::/hype/disks/$(basename "$_d")"
        done
    fi
    sync "$ESP"

    # Verify what was produced rather than trusting the tools -- the same discipline
    # tools/make-disk-image.sh applies, for the same reason: a silent short write here looks
    # exactly like hype failing to boot.
    local want have
    want=$(stat -c%s "$ISO")
    have=$(mdir -i "$ESP@@1M" ::/iso 2>/dev/null | awk '/test *iso/ {print $3}')
    [ "$have" = "$want" ] || { echo "ESP verify FAILED: test.iso is $have bytes, wanted $want"; return 1; }
    mdir -i "$ESP@@1M" ::/EFI/BOOT 2>/dev/null | grep -q BOOTX64 || \
        { echo "ESP verify FAILED: BOOTX64.EFI missing"; return 1; }
    return 0
}

boot_once() {
    cp -f "$OVMF_VARS" "$OUT.vars.fd"   # fresh, so a previous run's BootOrder cannot decide
    rm -f "$LOG"
    # SMP and EXTRA_QEMU_ARGS are overridable so one harness can also build host topologies hype
    # must cope with but this machine does not have natively. #360's bug -- APs started by
    # hardcoded APIC ID 1 and 2 -- only shows on a host whose APIC IDs are NOT densely packed
    # from zero, which is every Intel hybrid part and no AMD one. QEMU can synthesize that:
    #   SMP='1,sockets=2,cores=4,threads=1,maxcpus=8' \
    #   EXTRA_QEMU_ARGS='-device host-x86_64-cpu,apic-id=1,socket-id=0,core-id=1,thread-id=0
    #                    -device host-x86_64-cpu,apic-id=5,socket-id=1,core-id=1,thread-id=0' \
    #   tools/run-guest.sh ...
    # gives APIC IDs 0, 1, 5 -- a host where ID 2 does not answer, reproducing the Intel failure
    # on an AMD box. Word-split deliberately, so EXTRA_QEMU_ARGS carries several arguments.
    #
    # VGA defaults to std, i.e. hype gets a real GOP framebuffer.
    #
    # It used to default to none, which is how this harness ran for its whole life -- and with no
    # GOP, fw_1_render_console() returns on its first line, so hype's ENTIRE display path never
    # executed under QEMU. #370 lived in that blind spot: a diagnostic MSR read that #GP-panicked
    # the BSP, reachable only when a framebuffer exists, so every regression run here was immune to
    # it while every real machine was not. A default that silently skips a subsystem is not a
    # neutral choice.
    #
    # VGA=none is still available for the A/B that isolates the display path from everything else
    # (that comparison is what identified #370), but it must be asked for, not assumed.
    # SENDKEYS presses real keys at chosen times, e.g.
    #   SENDKEYS='60:ctrl_r-alt_r-right,75:ctrl_r-alt_r-d' tools/run-guest.sh ...
    # Injected at the i8042, which is the device hype's own host-input path owns, so this exercises
    # the operator controls (leader chords, terminal switching, dashboard typing) end to end. #363
    # needs it: no amount of log reading can show that the keyboard still works -- something has to
    # press a key. QMP is only wired up when keys are actually requested.
    local qmp_args=()
    if [ -n "${SENDKEYS:-}" ]; then
        rm -f "$OUT.qmp"
        qmp_args=(-qmp "unix:$OUT.qmp,server=on,wait=off")
    fi
    qemu-system-x86_64 \
      -machine q35 -m 8192 -nodefaults \
      -accel kvm -cpu host -smp "${SMP:-4}" ${EXTRA_QEMU_ARGS:-} \
      -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
      -drive if=pflash,format=raw,file="$OUT.vars.fd" \
      -drive format=raw,file="$ESP" \
      "${qmp_args[@]}" \
      -serial "file:$LOG" -display none -vga "${VGA:-std}" 2>"$OUT.stderr" &
    local qpid=$!
    local keypid=
    if [ -n "${SENDKEYS:-}" ]; then
        python3 tools/qmp-sendkeys.py "$OUT.qmp" "$SENDKEYS" >"$OUT.keys" 2>&1 &
        keypid=$!
    fi
    # Bounded by wall clock: these guests never exit on their own, and a hung run must still
    # leave a log behind to read.
    local i
    for i in $(seq "$SECS"); do kill -0 $qpid 2>/dev/null || break; sleep 1; done
    kill -9 $qpid 2>/dev/null || true
    wait $qpid 2>/dev/null || true
    if [ -n "$keypid" ]; then
        kill -9 "$keypid" 2>/dev/null || true
        wait "$keypid" 2>/dev/null || true
        [ -s "$OUT.keys" ] && cat "$OUT.keys"
    fi
}

build_esp() {
    if [ "$ISO_MODE" = raw ]; then build_esp_raw; else build_esp_file; fi
}

build_esp || exit 1
echo "delivery mode: $ISO_MODE ($(( ISO_BYTES / 1048576 )) MB ISO)"
echo "booting $(basename "$ISO") for ${SECS}s -> $LOG"
#
# #371: a boot with no hype banner is INVALID, not failed. Retry it, and never return it as a result.
#
# The firmware fails to reach hype in 5-15% of boots on this rig: OVMF selects the boot device,
# issues the first READ DMA for LBA 0, and QEMU never completes it. Measured cause -- a 2,560-byte
# hello-world reproduces it, two independent OVMF builds fail at identical rates, the host disk is
# completely idle at the time, and it never happens on virtio. Nothing to do with hype, which is
# never entered: the log holds only OVMF's mode-setting escape sequences, 113 bytes.
#
# That log reads exactly like a guest wedge -- "hype silent, zero exits, no output" -- and has
# already produced two wrong diagnoses (#344 scored these as wedges; #365 read four of them as a
# regression in a binary that had booted minutes earlier). At this rate a 10-run batch expects 1-2,
# so filtering them is not a rare-event nicety.
#
# The ESP is deliberately NOT rebuilt between attempts, unlike the previous version of this retry.
# #371 proved the image is fine -- six of eight boots loaded BOOTX64.EFI from the very same file --
# so rebuilding only spends ~30s recopying a 266 MB ISO and muddies what is being retried.
#
BOOT_ATTEMPTS="${BOOT_ATTEMPTS:-3}"
attempt=1
while :; do
    boot_once
    grep -aq "^hype: build" "$LOG" && break
    if [ "$attempt" -ge "$BOOT_ATTEMPTS" ]; then
        echo "HARNESS FAILURE: firmware never launched hype in $BOOT_ATTEMPTS attempts (no banner)."
        echo "  This is #371 (QEMU/OVMF loses the first AHCI DMA completion), NOT a hype result."
        echo "  Do not score this run. Log: $LOG ($(wc -c < "$LOG") bytes)"
        exit 2
    fi
    echo "WARNING: no hype banner (#371, ~5-15% of boots) -- retrying, attempt $((attempt + 1)) of $BOOT_ATTEMPTS"
    attempt=$((attempt + 1))
done
echo "done: $(wc -c < "$LOG") bytes in $LOG"

#
# #343: score the GUEST's own health, not just "did the installer appear".
#
# Every FreeBSD run until now was graded on reaching bsdinstall -- which is true both when the guest
# is healthy AND when it panics seconds later, because the panic happens after that point. So the
# project's most-used regression gate was blind to a guest kernel fault, and the next real regression
# would have been indistinguishable from #343's intermittent one. Grep for the fault signatures too.
#
if grep -aqE 'panic:|Fatal trap|Kernel panic|BUG: unable to handle' "$LOG"; then
    echo "WARNING: the GUEST reported a kernel fault -- this run is NOT a clean pass (see #343):"
    grep -aoE 'panic: [^|]{0,60}|Fatal trap [0-9]+[^|]{0,40}|Kernel panic[^|]{0,50}' "$LOG" \
        | sort -u | head -3 | sed 's/^/    /'
fi
# (The bannerless case is handled above and exits 2 before reaching here, so there is deliberately
# no second check: a warning that can never print is worse than none, because it reads as coverage.)
