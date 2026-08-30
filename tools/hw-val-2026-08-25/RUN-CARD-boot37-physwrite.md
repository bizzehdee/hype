# Boot 37 -- physical AHCI + NVMe under concurrent write. Four tickets.

Queued behind boot 36. This is the only run that can reproduce these at all: a sandbox's
virtual disks are memory-speed, so the stall symptom has no way to appear there.

## Config

The `tools/hwstick/hype.cfg` shape -- vm0 doing phys-write-ahci and vm1 doing phys-write-nvme
**at the same time**, which is the condition #713 was found under.

> **Targets are named BY SERIAL, and only the sanctioned spare.** Never the internal NVMe --
> that is the user's BitLocker Windows install. `docs/hw-val-211.md`'s exclusions apply to
> every write target on this run.

hype gates every `physical:` write behind a typed confirmation on the dashboard
(`core/phys_confirm.c`), so nothing is written until you type the token it prints. Read the
drive it names before you type it: confirming erases that drive.

## The four

| Ticket | What to read | Passes when |
| --- | --- | --- |
| **#713** | `FBSPEED t=` against real elapsed time, plus `PREEMPT` and `LOOPPHASE` | no multi-second dashboard stalls. The original was up to **46 seconds**, with a 6-7 s recurring baseline that persisted after |
| **#660** | `nvme_lock_contended=`, `bsp_nvme_timeouts=` | the lock is actually exercised -- contention non-zero under concurrency -- with no timeouts and no wrong bytes delivered |
| **#715** | `DIAG: BLK WRITE ... vec=` | vectored writes taken on the NVMe path rather than falling back to single-segment. `5308498` armed it; this is the first run that proves it fires |
| **#388** | `m5-8: target_disk` resolving a `physical:<serial>` USB disk | it resolves and the confirm prompt names the right drive. Point it at the **SanDisk Cruzer Blade** (`4C530201070308103214`), never the SABRENT -- that is the boot and log medium |

**#388 moved here from boot 36 deliberately.** It needs a config change, and boot 36's
configuration is the one that has finally been debugged into working; changing vm0's target
disk would alter the conditions #780 and the guest-boot check depend on. This run is already
about physical write targets, so it is where #388 belongs.

## Watch for

`USBWAIT` and `input ticks skipped for the USB lock`. Boot 34 measured 155 ms of lock wait
across 210 seconds with zero skipped ticks, on a run doing far less I/O than this one. If
concurrent physical writes starve the input tick, that is #713's mechanism and it will show
here rather than in a counter nobody read.
