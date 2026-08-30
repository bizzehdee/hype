Hardware validations owed as of 2026-08-30, ordered so each boot closes as many tickets as
it can. Everything below is built and QEMU-clean where QEMU can reach it; none of it closes
without a boot.

Twenty-six tickets sit in On Hold. **Six of them no hardware run can help** -- they are
waiting on an artefact or a decision, and are listed last so nobody schedules a boot for
them. The remaining twenty fall into six runs.

---

## Boot 36 -- AMD 5950X. The input set, and it is nine tickets

Build: default, no `EXTRA_CFLAGS`. Config `hype1a.cfg` (`vcpus = 2`). **Pico armed** -- press
BOOTSEL once and confirm `a0001` before walking away; boot 35 produced `reports=0` for the
whole run, which is what a disarmed board looks like, and it cost the run four tickets.
**Leave both spare USB drives plugged in** -- they are the condition #780 needs.

Run 90 minutes. No operator actions beyond arming the Pico and confirming the guest boots.

| Ticket | What to read | Passes when |
|---|---|---|
| #773 | the Pico's `abcdefghijklmnopqrstuvwxyz0123456789` lines echoed in `RUN1A.LOG` | every pass intact. The string is strictly increasing, so a dropped character leaves a gap and a doubled one a repeat -- no counting, no reference copy. Four passes at 8 ms/char and four at 30 ms; **both speeds clean** closes it, fast-only losses mean the unarmed window is real |
| #774 | a run of `a`s from the Pico's 12-second hold | present. hype synthesises typematic; the device does not |
| #777 | that same run of `a`s | **stops before the release**, at the 10-second bound |
| #775 | `HIDTICK ... revives= revive_fail=` | revives climbing with `revive_fail=0`, and `reports=` resuming after each. Boot 35 already showed this working four times on controller[1] |
| #780 | `media: registered host device` and `host-fat: vm0 resolved` | the boot medium keeps its own serial with the spare drives attached, and the guest boots. Confirmed once in boot 34; a second clean run closes it |
| #641 | `APVCPU vm0/N: exits=` and `PERF: hlt_wait=` | recorded, not passed -- this one is a measurement refresh. It was 328.9M exits in 84 minutes on boot 31 |
| #426 | the run completing at all | the standing HW-VAL gate: NVMe + xHCI ring math on the shared facility, no regression |
| #388 | `m5-8: target_disk` resolving a `physical:<serial>` USB disk | **requires a config change first** -- see below |

**#388 needs a config edit before this boot.** It wants `target_disk = physical:<serial>` to
resolve against a USB disk's identity, and no current config asks for that. Point it at the
SanDisk Cruzer Blade (`4C530201070308103214`), never the SABRENT -- that one is the boot and
log medium. If the edit is not made, drop #388 to boot 37 and this run still closes eight.

---

## Boot 37 -- AMD 5950X. Physical AHCI + NVMe under concurrent write

Config: the `tools/hwstick/hype.cfg` shape -- vm0 phys-write-ahci and vm1 phys-write-nvme
running at the same time. **Target the spare drive by serial**, never the internal NVMe.

| Ticket | What to read | Passes when |
|---|---|---|
| #713 | `FBSPEED t=` against real elapsed, `PREEMPT`, `LOOPPHASE` | no multi-second dashboard stalls. The original was up to 46 s with a 6-7 s recurring baseline |
| #660 | `nvme_lock_contended=`, `bsp_nvme_timeouts=` | the lock is exercised (contention non-zero under concurrency) with no timeouts and no wrong bytes |
| #715 | `DIAG: BLK WRITE ... vec=` | vectored writes actually taken on the NVMe path, not falling back to single-segment |

Three tickets, one config, one boot. This is the only run that reproduces the symptom at all:
a sandbox's virtual disks are memory-speed and cannot.

---

## Boot 38 -- AMD 5950X. The storage write battery, on a re-partitioned drive

**This run needs a drive rebuild and that is the user's call.** #688 and #689 want a 512 GB
USB-SATA drive as 4 GiB FAT32 ESP plus a large data partition -- ext4 for one, NTFS for the
other. The current hw-val drive is exactly that hardware, laid out as FAT32 + exFAT, and
re-partitioning it retires the layout every run since 2026-08-25 has used.

Two ways to spend one boot instead of two: give the drive **three** partitions (4 GiB FAT32 +
~230 GB ext4 + ~230 GB NTFS) and run two VMs, one sourcing from each.

| Ticket | What to read | Passes when |
|---|---|---|
| #688 | `host-ext: ... resolved` for an ISO and a disk image on the ext4 partition | both resolve and stream; the guest boots from the ISO |
| #689 | `host-ntfs: ... resolved` for the same on the NTFS partition | same |
| #653 | the exFAT write battery's own PASS lines | **needs the battery to exist first** -- #653 is test infrastructure, not a run. Build it, then it rides here |
| #754 | a real USB storage device pulled MID-WRITE | the in-flight I/O fails cleanly, the guest is told, the volume is not left half-written, and a re-plug does not silently resume. **Last act of the run** -- it deliberately damages volume state |

**#754 must not use the HYPEBOOT drive.** It needs a sanctioned scratch USB device that hype
is actively writing to, identified by the serial hype itself reports from INQUIRY VPD 0x80.

---

## Boot 39 -- AMD 5950X. VM-exit coverage

| Ticket | What to read | Passes when |
|---|---|---|
| #603 | the microtest's coverage table vs its counters | every intercepted exit reason reached and correct on the SVM leg |

One ticket, and a short boot. `suite-603.cfg` needs four physical cores after BSP reservation,
which the 5950X has and the earlier 3-core sandbox did not -- that was the admission failure,
not a hype bug.

---

## Boot 40 -- Intel i5-13420H. APICv

Paired A/B from one tree: the same config twice, once with `-DHYPE_ENABLE_APICV=1`.

| Ticket | What to read | Passes when |
|---|---|---|
| #599 | the APICv build reaching a login prompt | it boots at all. Boot 2c hung in kernel init against a clean Boot 2a from the identical tree -- that hang is the open work, and this run is the retest after it is fixed |
| #605 | the same, plus `VECSTAT` delivery counts | PASS flips the Intel default to ON, per decision 58 |

**#605 cannot pass before #599's hang is fixed.** If #599 is still open, this boot is a #599
diagnostic run, not a #605 gate.

---

## Boot 41 -- Intel i5-13420H. VMX correctness

| Ticket | What to read | Passes when |
|---|---|---|
| #525 | a guest reboot driven from a non-BSP vCPU | the 0xCF9 path works from vCPU 1. The VMX side of `fw_1_shared_port_io()` is compile-tested only; this is its first execution |
| #729 | guest MTRR/PAT/pvclock MSR reads | modelled values, not zeros and not dropped writes. `3f59e4c` landed one model shared by both backends; SVM is proven, VMX is not |

Both are Intel-only and both ride one Linux guest boot.

---

## Not schedulable: six tickets no boot can close

| Ticket | Waiting on |
|---|---|
| #442 | an installed Windows guest booted to OOBE |
| #634 | a pre-installed Windows guest disk image (#695) -- only install ISOs exist |
| #635 | #634 |
| #636 | #634 |
| #400 | its first consumer: the HNET-D* NIC driver. The facility is built and unit tested; "validate live drain" has nothing to drain |
| #709 | **a decision, not a run.** #709 as filed wires the boot hook to call `hype_fs_battery_run()` directly, but `core/fs_battery.c` was split out of the shipped binary at the user's request. Link it back in behind the marker gate, or duplicate just the write/append logic as a small firmware-owned function? |

#695 is the lever: it unblocks three of these at once, and it is a QEMU task, not a boot.

---

## What this comes to

| Run | Machine | Tickets |
|---|---|---|
| Boot 36 | 5950X | 8, or 9 with the #388 config edit |
| Boot 37 | 5950X | 3 |
| Boot 38 | 5950X | 3, plus #653 once its battery exists |
| Boot 39 | 5950X | 1 |
| Boot 40 | Intel | 2, gated on #599 |
| Boot 41 | Intel | 2 |

Twenty tickets across six boots, four of them on the AMD desktop. Boot 36 is the one to do
first: it is the largest, it needs no rebuild and no re-partitioning, and it is the run that
has now failed three times for reasons that are all fixed.
