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

## Result -- 2026-09-11, build `8ab98e2-dirty`, Intel i5-13420H (logs in `logs/bootI5-boot1/`)

16 minutes (rtc 11:24:25 to `all guests down -- powering off`), `host off` honoured, log intact
at 23 MB. Admission fitted all four VMs; `run2c` `run2e` `run2n` autostarted, `run3d` was never
started. **No guest reached login.** All three stopped after GRUB's `Booting 'Linux lts'`; the
kernel ran (it programs its LAPIC timer from `rip=0x...091ffc` 210,000 times) and idled in
`sti; hlt` for the whole run.

```
vmx: apicv=ON (slot 0..7)  proc2=0x13eb                           <- VID, APIC-register virt, virtualize APIC accesses all granted
HLTSHADOW: bsp hlt exits with STI blocking=81518880 of those with RVI pending=81518880 | rvi wakes=81518880
GUESTPC vm0: exits=82819348 lastreason=0xc rip=0xffffffffa8c81c2e rflags=0x246 intrblk/entryinj=0x100000000
INTDIAG vm0/0: eventinj=1343336 ... IF=1 shadow=0x1
vmx apicv-state: gis=0x3030   (91 samples: 3x 0x0 at bring-up, then 0x3030 with 3x 0x30ec)
  virr: [1]=0x10000            <- vector 0x30 pending
  isr:  [1]=0x1001af           <- vectors 0x20 0x21 0x22 0x23 0x25 0x27 0x28 0x30 ALL in service
VEC 0x30 irq=0 chip="IO-APIC" action="timer"; VEC 0x20 acpi; 0x21 i8042; 0x22 rtc0
PITROUTE vm0: edges=45 pic_delivered=2 apic_delivered=42 apic_refused=3 | RTE[2]=0x30
APVCPU vm0/1: live=1 exits=245840067 last=0xc@0x3fb6b033 timer_irqs=0    <- APs: HLT storm too, IF=0
vm0 vCPU 1/2/3: SIPI received x10 each (the guest retried its AP bring-up ten times)
```

| Ticket | Result |
| --- | --- |
| #708 | **FAIL, and the wake is now measured.** The 447cbb0 wake fires on every HLT exit (W = M = 81.5 M) and the guest halts again at once: the CPU cannot deliver. `gis=0x3030`: RVI 0x30 pending, SVI 0x30 in service, so PPR = 0x30 and a vector of the same class is never deliverable. The page's VISR holds eight IO-APIC vectors (0x20..0x28, 0x30) that were delivered and never cleared. The LAPIC timer (0xec) outranks PPR and still runs, which is why the guest ticks but never sees IRQ0. Not a wake problem any more: an EOI-virtualization problem |
| #599 | `apicv=ON` on all 8 slots, `proc2=0x13eb`. Its bar (a 2-vCPU login with the flag on) is not met |
| #605 | FAIL: a guest that logs in without APICv (the same ISO on this machine in run 14) does not with it. Signature on #708 |
| #688 #689 | not met: `run2e` and `run2n` never logged in, no `EXT4-WRITE-DONE` / `NTFS-WRITE-DONE` |
| #388 #754 | not exercised: `run3d` not started, `RUN3D.LOG` 0 bytes, and the SanDisk was not registered (`media: registered` lists only the NVMe and the hw-val drive) |
| #788 | no Pico line |
| #808 | `TSCSKEW: apic=8 9 16 17 24 25 40 42 ap-bsp=+1050609..+1050632us` -- **every AP's TSC is 1.05 s ahead of the BSP** on this machine (the AMD laptop: -197 us). That is the run-14 wrapped gap. `KBDDRAIN pumps=226209 foreign=314303`, `gap_recent` steady 30.8-34.5 ms across 100+ samples, `kbddiag=264(max 673ms)` |

### Two things to fix before the next APICv boot

1. The `vmx apicv-wr:` bring-up probe printed 210,129 lines (one per TMICT write) and
   `apicv-sync` 69,693: 277 K of the log's 311 K lines, 23 MB. Rate-limit both.
2. The AP TSC offset: sync each AP to the BSP at bring-up (IA32_TSC_ADJUST), as Linux does.
   Timers hype arms in BSP TSC units fire 1.05 s off on the APs (run 14 `TMRLATE worst_late` 7 s
   here).

### What decides #708 next

Probe the reason-45 exit: vector, `gis` and VISR word before/after, so the log says whether the
guest's EOIs for 0x20..0x30 reach EOI virtualization at all. Count reason-45 exits per vector.
