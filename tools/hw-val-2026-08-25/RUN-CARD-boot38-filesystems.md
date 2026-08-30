# Boot 38 -- FAT32 boot beside a real data filesystem. Three tickets, and a drive rebuild.

> **This run needs a decision before it can be prepared.** #688 and #689 want a 512 GB
> USB-SATA drive laid out as a 4 GiB FAT32 ESP plus a large data partition -- ext4 for one,
> NTFS for the other. The current hw-val drive is exactly that hardware, laid out as FAT32 +
> exFAT, and re-partitioning it retires the layout every run since 2026-08-25 has used,
> including the log medium. Do not start this until that is agreed.

## The layout that spends one boot instead of two

Give the drive **three** partitions and run two VMs, one sourcing from each data filesystem:

- Partition 1 -- FAT32, ~4 GiB. The ESP: `\EFI\BOOT\BOOTX64.EFI`, hype's firmware images,
  `hype.cfg`, and hype's logs. Nothing else.
- Partition 2 -- ext4, ~230 GB. ISOs and guest disk images for vm0.
- Partition 3 -- NTFS, ~230 GB. The same for vm1.

Both tickets are about hype reading and writing a real data filesystem beside its own ESP, so
one boot with two VMs answers both. Keep the ISOs byte-identical between the two partitions,
or a difference in behaviour cannot be attributed to the filesystem.

## The three

| Ticket | What to read | Passes when |
| --- | --- | --- |
| **#688** | `host-ext: ... resolved` for an ISO and a disk image on partition 2 | both resolve and stream, and vm0 boots from the ISO. The log keeps landing on partition 1 throughout |
| **#689** | `host-ntfs: ... resolved` for the same on partition 3 | same, for vm1 |
| **#754** | a real USB storage device pulled **mid-write** | the in-flight I/O fails cleanly, the guest is told, the volume is not left half-written, and a re-plug does **not** silently resume a half-written cluster chain |

## #754 is the last act, and it needs its own device

It deliberately damages volume state, so nothing else can run after it.

> **Not the HYPEBOOT drive.** #754 needs a sanctioned scratch USB device that hype is
> actively WRITING to at the moment you pull it, identified by the serial hype itself reports
> from INQUIRY VPD 0x80 -- never by anything the host OS says, because USB-SATA bridges report
> the enclosure's serial rather than the drive's.

The failure it is chasing is real and not hypothetical: the SABRENT bridge on the hw-val drive
dropped off the bus under sustained write during staging on 2026-08-27 at 09:05, with no
operator involved, and left the exFAT volume dirty.

## #653 rides here once it exists

#653 is test infrastructure, not a run: exFAT has no on-medium write battery, where FAT32 has
`core/fat32_selftest.c` plus a host rig. Build that first and it runs on this boot for free,
against a real stick, which is the only place it means anything.
