#!/bin/sh
# #695: SUPERSEDED, kept for the record. Do not use.
#
# This injected tools/695/autounattend.xml into the Windows ISO with
# `xorriso -boot_image any replay`. It produced media the guest firmware would not boot at all
# ("No bootable option or device was found"), because the repack rewrote the El Torito UEFI
# entry with a load size of 0 where the original had 1:
#
#     original   El Torito boot img : 2  UEFI  y  none  0x0000  0x00  1  516
#     repacked   El Torito boot img : 2  UEFI  y  none  0x0000  0x00  0   60
#
# The verification here asserted only that the word "UEFI" still appeared in the report, which
# it did, so the check passed while the thing it existed to guarantee had broken.
#
# The answer file is delivered as a separate 16 MiB FAT volume instead ([disk.answer] in
# tools/695/hype.cfg), which touches the install media not at all.
echo "superseded: the answer file ships as [disk.answer], not injected into the ISO" >&2
exit 1
