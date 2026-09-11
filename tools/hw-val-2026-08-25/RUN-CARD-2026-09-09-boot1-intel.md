# Boot 1 (Intel i5-13420H) -- APICv build, ext4 + NTFS data legs, scratch-stick write and pull

Plan: `docs/hw-validation-queue-2026-09-09.md`, boot 1. Tickets: #599 #605 #708, #688 #689
(Intel legs), #388 #754, and a #788 sample.

**The Intel box's only NVMe is the user's BitLocker Windows install.** Nothing in this run names
a physical target except the 8 GB SanDisk scratch stick (hype serial `4C530201070308103214`).
Boot from the hw-val drive, nothing else.

Build: **APICv** (`-DHYPE_ENABLE_APICV=1`) is the active `\EFI\BOOT\BOOTX64.EFI`; default and
AVIC builds sit beside it under `\EFI\hype\`. Active config `\hype.cfg` = `hype2h.cfg`: four
VMs, `run3d` not autostarted. Scripts `\input\vm0..3.txt` = `input-2h/`.

## Plug in, in this order

1. The hw-val drive (boot medium). Four partitions: `HYPEBOOT` FAT32, exFAT, ext4, NTFS.
2. The 8 GB SanDisk scratch stick. **Its contents are destroyed by step 6.**
3. The Pico keyboard (the #788 sample) and the Keychron to type on.

## Before you boot

- Banner sha matches the staging output.
- `vmx: apicv=ON (slot 0) ... (HYPE_ENABLE_APICV set)`. If it prints `apicv=off`, the part did
  not grant the controls; note it and go on -- steps 3-6 do not depend on it.
- `media: registered host device N = usb serial='4C530201070308103214'` -- the scratch stick is
  seen. Without it step 6 cannot run; re-seat it and cold boot.
- Admission grants `run2c` two cores, `run2e` and `run2n` one each.

## The sequence

1. Boot, stay on the dashboard. Do not type into a guest.
2. `run2c` logs in and prints `fresh-boot-login`. `run2e` and `run2n` log in, write, and print
   `EXT4-WRITE-DONE` / `NTFS-WRITE-DONE`. Their disks are images on the ext4 and NTFS
   partitions, resolved through `media_disk = DB9876543214E`.
3. **From run2c's login, leave the machine 10 minutes idle.** That is the window where boot 2c
   froze. The Pico types throughout.
4. `flush` at the dashboard. Then `start run3d`.
5. `confirm` when run3d asks for the physical target. Its script writes a signature to the
   stick, reads it back, then prints `YANK-WINDOW-OPEN` and starts a 6000 MiB write.
6. **Pull the SanDisk** about a minute into the bulk write. Watch for `DIAG: GONE ... refused=`
   climbing and the guest's dd ending with an I/O error. hype must stay up. Wait 30 s, plug the
   stick back in: nothing may resume without an explicit `attach`.
7. `host off` first, then the power button if it parks. Bring back `HYPE.LOG` and every
   `RUN*.LOG`.

If run2c never reaches login inside 20 minutes, that is the #708 evidence, not a harness fault:
go on to step 4 anyway.

## On the bench afterwards

```
fsck.vfat -n /dev/<drive>1      # partition 1: clean, and only hype's own files on it
fsck.exfat -n /dev/<drive>2
e2fsck -fn /dev/<drive>3        # ext4: clean; hype/disks/ext4-scratch.img carries the signature
ntfsfix -n /dev/<drive>4        # NTFS: clean; hype/disks/ntfs-scratch.img carries the signature
fsck.vfat -n /dev/<stick>       # scratch stick: the write landed at LBA 512 + 1 MiB..; the
                                # boot drive's four volumes are untouched
```

Then archive the logs to `logs/boot1-intel/` and re-stage.

## What to read

| Ticket | Read | Passes when |
| --- | --- | --- |
| #708 | `SCRIPT vm0: PASS`; `HLTSHADOW: bsp hlt exits with STI blocking=N of those with RVI pending=M \| rvi wakes=W` | login reached, no `TIMERSTALL`. `W` > 0 with `M` ~ `W` says the wake fired for the case 2c died in |
| #599 | run2c login with `apicv=ON` | its bar: a 2-vCPU Linux login with the flag on |
| #605 | `VECSTAT` / `INTDIAG`, no `PANIC`, `WATCHDOG`, `TIMERSTALL` | a previously working guest boots to the same point with APICv on. On PASS flip the Intel default (decision 58); on FAIL the signature goes on #708 |
| #688 | `SCRIPT vm1: PASS`, the volume the resolver reports for `\hype\disks\ext4-scratch.img`, `e2fsck -fn` clean, `fsck.vfat -n` on partition 1 clean with only hype's files | Intel leg met. Deviation to record: image staged fully allocated (mkdisk cannot target a USB disk), written in place by the guest |
| #689 | `SCRIPT vm2: PASS`, same for `\iso\ntfs-test.iso` and `ntfs-scratch.img`, `ntfsfix -n` clean | Intel leg met, same deviation |
| #388 | `READBACK-MATCH` on run3d; host-side: the boot drive's four volumes unchanged | the write landed on the named stick and nowhere else |
| #754 | `DIAG: GONE ... refused=` climbing after the pull; hype alive to `host off`; no resume after re-plug; stick `fsck` | the bridge-under-yank half of #747 |
| #808 | `fw-1 TSCSKEW: apic=N ap-bsp=+Xus` per AP; `KBDDRAIN ... gap_recent= ... pumps=P foreign=F`; `BSPSTARVE ... kbddiag=` | run 14 on this machine printed a `gap_recent` that decoded to -1.2 s (two cores stamping one TSC). `TSCSKEW` in the seconds on any AP names a per-core TSC offset as the cause; tens of us on every AP says it was the AP pump, now turned away (`foreign` > 0). Either way `gap_recent` must now read in ms, and `kbddiag` well under the 173 ms run 14 measured here |
| #788 | `HIDTICK[...] typematic=T rebounce=R [#788]` on the Pico's line against the doubled-character count from `KBDCHARS` | `T` tracking the doubles = hype's auto-repeat fires on an 8 ms stream; `R` tracking them = a duplicate at the report level; both 0 = downstream of the report diff |

## Re-staging afterwards

`./tools/hw-val-2026-08-25/stage.sh --boot amd1` after archiving this boot's logs. Boot 2 of the
queue gets its own set once its config is written.
