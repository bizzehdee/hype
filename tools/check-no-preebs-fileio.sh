#!/bin/sh
# #452 (plan.md section 10 decision 37): Phase 0 does no file I/O.
#
# After #450 (config), #451 (firmware images) and #452 (input scripts, the ISO head, the
# previous-boot log), nothing in hype's pre-ExitBootServices path reads or writes a file through
# UEFI's Simple File System. Everything hype reads off its own volume goes through its own
# drivers, so booting from USB, SATA or NVMe is not a distinction hype's code makes.
#
# That property is one careless call away from being false again, and the symptom would be
# invisible on the machines where it matters: a firmware-mountable ESP hides it completely, and it
# only shows up on the host whose boot volume firmware cannot mount. So assert it at build time.
set -e
target=${1:-boot/main.c}
hits=$(grep -n 'hype_file_locate_root\|hype_file_get_size\|hype_file_read_range\|hype_file_read_into\|hype_file_write_new\|hype_file_delete' "$target" | grep -v '^\s*[0-9]*: *\*' || true)
if [ -n "$hits" ]; then
    echo "check-no-preebs-fileio: FAILED -- $target calls UEFI Simple File System:" >&2
    echo "$hits" >&2
    echo "" >&2
    echo "Phase 0 must do no file I/O (decision 37). Read through fw_1_boot_volume() +" >&2
    echo "hype_fs_lookup/hype_fs_read_at in Phase 1 instead." >&2
    exit 1
fi
echo "check-no-preebs-fileio: OK -- no UEFI Simple File System calls in $target (#452)"
