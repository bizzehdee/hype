# HW-VAL queue 2026-09-09: clear On Hold in two boots

Goal: empty the On Hold column with the fewest real-hardware boots. Earlier run plans are
superseded by this one. Seventeen tickets are On Hold. Eleven of them can leave the column with
two boots plus one bench session; the rest are not hardware work at all.

## Triage of the seventeen

| Ticket | Waiting on | Disposition |
| --- | --- | --- |
| #795 | re-run of boot A after `f26b67c` | **Close now, no boot.** Boot A2 (2026-09-03, `b89e5d6`) recorded "CR0.CD fix held"; runbook row Intel-A lists #795 as met |
| #599 #605 (+#708 Doing) | APICv build on Intel with the `447cbb0` wake fix | Boot 1 |
| #688 #689 | a USB-SATA drive with FAT32 ESP + ext4 / NTFS data, booted on Intel AND AMD | Bench rebuild of the hw-val drive, then both boots discharge them in passing |
| #388 #754 | a scratch USB stick as `physical:<serial>` write target, then pulled mid-write | Boot 1, last act |
| #713 #715 #660 | AHCI + NVMe concurrent physical writes on the AMD laptop | Boot 2 |
| #788 | a fix, then a measured run | Diagnostic lands before boot 1; fix confirmed on boot 2 |
| #641 | an idle-yield change, then a measured run | Boot 2 only if the yield change lands first; otherwise stays |
| #653 | #709 (boot-time hook for the #692 battery) | Rides both boots if #709 lands first; otherwise stays |
| #634 #635 #636 | #695 (installed Windows image), QEMU only | Not hardware. Leave; a dev session owns them |
| #442 | an installed Windows guest at OOBE | Not hardware once #695 exists; the #436 QEMU rig serves it |

## Bench session (no boot): rebuild the hw-val drive once

The SABRENT hw-val drive is today `HYPEBOOT` (FAT32) + one exFAT data partition. #688 and #689
describe hype's own boot drive with a data partition beside the ESP. Rebuild the drive as:

1. 4 GiB FAT32 `HYPEBOOT` (ESP, hype.cfg, logs).
2. exFAT data partition (keep it: `stage.sh` locates the drive by its exFAT partition; update the
   UUID it expects at `tools/hw-val-2026-08-25/stage.sh:59`).
3. ext4 data partition with the Alpine ISO and an empty `hype/disks/` directory (#688).
4. NTFS data partition with a second copy of the ISO (#689).

Deviation to record on both tickets: three data partitions instead of one 508 GB partition. The
clauses under test (locator picks partition 1, cfg write-back and logs land only on partition 1,
ISO read from the data partition, `mkdisk` + guest write on it, clean `fsck` afterwards) do not
depend on the partition count. Every boot from this drive after the rebuild is a #688/#689 sample.

Also before boot 1, in the sandbox:

- `tools/hwstick/hype.cfg`: point vm0's ISO at the ext4 partition, add `vm2` as a second writer on
  the spare NVMe (other partition, different LBA range) with a second file-backed disk on the
  NTFS partition, drop `vm.suite` (the laptop budget is 3 vCPUs). #660 records zero without two
  writers on one NVMe controller.
- #788 diagnostic: count HID boot reports where an empty report sits between two identical
  non-empty reports inside one poll interval, and log the count on the `KBDCHARS` tick. The Pico
  feeds ~25k alphabet characters per 90 minutes, so any boot with the Pico attached is a sample.
- Optional, each turns the two boots into more closes: #709 (small; `fs_selftest_disk` key is
  already parsed, `0dddc7a`) unlocks #653 on both boots; the #641 idle yield (AP loop arms a
  host wake deadline and issues a real HLT instead of re-entering VMRUN) unlocks #641 on boot 2.

## Boot 1: Intel i5-13420H, APICv build, 45 minutes

Card: `tools/hw-val-2026-08-25/RUN-CARD-2026-09-03-bootIntelB.md` as written, plus three
additions. The internal NVMe is BitLocker Windows and is never named. Plug the Pico in.

1. Boot the APICv build. Confirm `vmx: apicv=ON`.
2. vm0 boots the Alpine ISO from the ext4 partition, logs in, sits idle 10 minutes (the 2c
   freeze window). Read: `SCRIPT vm0: PASS`, `HLTSHADOW`, no `TIMERSTALL` -> #708, #599, #605.
3. At the dashboard: `mkdisk` a virtual disk on the NTFS partition, attach it to vm0, `dd` 64 MiB
   into it from the guest. Repeat on the ext4 partition -> #688 and #689 Intel legs.
4. Last act, scratch stick (a third USB device, identified by the serial hype prints from INQUIRY
   VPD 0x80): `target_disk = physical:<scratch-serial>` on a second VM, `confirm` at the
   dashboard, `dd` to it, verify the marker -> #388. While the write still runs, pull the stick:
   `DIAG: GONE ... refused=` climbs, hype stays up, guest reports the error; re-plug and confirm
   nothing resumes without `attach` -> #754.
5. `host off`. Bring back `HYPE.LOG`, `RUN2C.LOG`. On the bench: `fsck.vfat -n` partition 1,
   `e2fsck -fn` ext4, `ntfsfix -n` NTFS, `fsck` the scratch stick.

Tickets closed on PASS: #599 #605 #708 #388 #754. Half of #688 #689. #788 first diagnostic
sample. On PASS of step 2, flip the Intel APICv default in the follow-up commit (decision 58).

## Boot 2: AMD laptop, concurrent physical writes, 45 minutes

Both spare serials confirmed with `lsblk -o NAME,SERIAL,SIZE,MODEL` before staging. The
internal NVMe and the hw-val drive are never targets. Plug the Pico in. Type the three
`confirm`s in the first minute, while input is known live (#808 is still open on this machine).

1. vm0 `phys-write-ahci` on the SATA SSD, booting its ISO from the ext4 partition; vm1 and vm2
   both writing the spare NVMe; vm2 also `dd`s into a file-backed disk on the NTFS partition.
2. Read `DIAG: BLK WRITE ... vec=` on vm1/vm2: non-zero merged segments, written range exact
   -> #715.
3. Read `nvme_lock_contended=` and `bsp_nvme_timeouts=` in the `KBDIRQ` line: contended > 0,
   no corruption on the post-run re-read -> #660.
4. Read `FBSPEED t=` against the log clock: the 28-46 s stalls of the original run either
   reproduce (record them and take the scope decision the ticket asks for) or are gone after
   #715/#796/#799 (close) -> #713.
5. #788: the `KBDCHARS` double rate against the diagnostic counter; if a fix landed after boot 1,
   the rate must read 0 in 10,000+ characters.
6. #641 (only if the yield landed): `APVCPU vm0/1 exits=` under 1000/s idle and `TMRLATE
   worst_late` under 100 ms.
7. `host off`. Bench: `fsck.vfat -n`, `e2fsck -fn`, `ntfsfix -n` -> #688 and #689 AMD legs.

Tickets closed on PASS: #713 #715 #660 #688 #689, plus #788 and #641 when their fixes landed.

## After the two boots

Still On Hold, none of them hardware: #634 #635 #636 #442 (all behind #695), #653 (behind #709),
and #641 / #788 if their code changes were not ready. Everything else is out of the column.

## Fallbacks

- Boot 1 hangs before login: that is the #708 evidence. Steps 3 and 4 still run from the
  dashboard with a second VM, so #688/#689/#388/#754 do not depend on the APICv result.
- Boot 2 input dies before the confirms are typed (#808): power off, re-boot, type the confirms
  immediately. Nothing else in the run needs typing.
- Boot 2 hits #803 (xHCI BOT cc=4) reading the ISO from the hw-val drive: the run stands, but
  #688/#689's "ISO read correctly" clause is then Intel-only and the AMD leg re-runs with the
  ISO on the ESP.
