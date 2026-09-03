# AMD laptop boot queue — spare SATA + spare NVMe fitted (2026-09-03)

The AMD laptop is the only machine with two sanctioned write targets. `tools/hwstick/stage.sh:37`
authorizes exactly two serials:

```
AUTHORIZED = {"5ME3N005713803V2W", "2132E5BF4EAE"}  # AMD laptop spare NVMe + SATA SSD
```

**Before staging, confirm both serials on the machine** (`lsblk -o NAME,SERIAL,SIZE,MODEL`) and
update `stage.sh` if either differs. The `docs/hw-validation-runbook-2026-09-02.md` Boot AMD-2
section records the AHCI serial as a placeholder, so `2132E5BF4EAE` may name a drive that is no
longer fitted. The by-serial guard fails safe: an unmatched serial arms nothing, so a wrong serial
costs a wasted boot, never a wrong disk. Never the internal NVMe. Never the SABRENT (boot and log
medium).

## The blocker that orders everything below

`#799` — on this laptop the Alpine kernel never gets past `Booting Linux lts`. `LOOPPHASE` /
`HOUSECOST` measured 235 us per exit in vCPU-0 loop housekeeping (`house=77.9 s of 180 s`) and the
kernel never left its early RDTSC delay loops. **Every boot below except L0 needs a guest kernel
to reach userspace, so every one of them is behind #799.**

Two facts worth holding together: #713 was originally found *on this laptop*, with Alpine writing
to both spare disks, so the machine did run guests at some point. And the desktop counter-sample
from boot AMD-1 run 2 is 59 us per BSP loop iteration — a quarter of the laptop figure. So #799 is
this machine's problem, and L0 decides whether the rest of the queue is runnable today.

## L0 — #799, five minutes

The cheapest boot on the list and the gate for the other five. Stage the current tree with
`hype1a.cfg` (the `HOUSECOST` instrumentation landed in `70d6b0f`, so any build at or after it
carries it) and read one line:

```
fw-1 HOUSECOST vm0: s2= ... s75=
```

next to `LOOPPHASE`. The section with the mass is the answer. The kernel spins from about t=10 s,
so five minutes is enough; there is no reason to leave it longer.

| Passes when | Then |
| --- | --- |
| `HOUSECOST` names one dominant section | #799 has its root cause; fix, then run L1 |
| the Alpine kernel *does* reach userspace | #799 was config- or build-specific; go straight to L1 |

## L1 — #713 + #715 + #660, thirty minutes of writes

**This is the boot the spare SATA unblocks.** `docs/hw-validation-runbook-2026-09-02.md` had to
plan Boot AMD-2 without it: *"Without a sanctioned SATA scratch drive, drop the AHCI VM and run
NVMe + USB; #713's original symptom needs the AHCI+NVMe pair, so it is then recorded, not
reproduced."* With both spares fitted the pair is available and #713's symptom can be reproduced
rather than recorded.

Config: `tools/hwstick/hype.cfg` already has the shape — `vm0 phys-write-ahci` on the SATA SSD
via `[disk.pd] backing=physical`, `vm1 phys-write-nvme` on the spare NVMe, `vm.suite` as the
regression baseline. Each physical target needs the dashboard `confirm` by hand (#125): budget
five minutes of typing at the start.

| Ticket | Read | Passes when |
| --- | --- | --- |
| #715 | `DIAG: BLK WRITE ... vec=` on the NVMe VM | non-zero merged segments and the written range exact. Implemented in `5308498`; this is its only remaining bar |
| #660 | `nvme_lock_contended=`, `bsp_nvme_timeouts=` in the `KBDIRQ` line | **needs two writers on ONE NVMe controller.** `tools/hwstick/hype.cfg` has one, so as staged this run can only record zero, which says nothing. Add a second VM writing to the same NVMe before this boot, or #660 stays On Hold. Fix landed `d133cac` |
| #713 | `FBSPEED t=` against elapsed, `PREEMPT`, `LOOPPHASE` | reproduces the up-to-46 s dashboard stalls with the AHCI+NVMe pair. Closing still needs the scope decision the ticket asks for; reproducing it is what has been missing |

## L2 — #211 + #386 + #208, thirty minutes

The spare SATA partitioned as host filesystems, with virtual disks living on them.

| Ticket | Needs | Passes when |
| --- | --- | --- |
| #211 | spare SATA with a FAT32 and an exFAT partition | a raw-file virtual disk resolves and streams from each, and guest writes persist across a host reboot |
| #386 | the same drive with FAT32 + exFAT + ext2/3/4 + NTFS | sparse growth in place on each, then clean `fsck.vfat -n`, `fsck.exfat -n`, `e2fsck -fn`, `ntfsfix -n` |
| #208 | the spare NVMe as the backing | the guest's own `nvme` driver binds to the presented controller and does durable I/O — the host NVMe read/write path against a real SSD |

## L3 — #686 #687 #688 #689 + #653 + #754, forty-five minutes

The mixed-partition epic. **Read the ticket text before staging:** these are written for a
512 GB **USB-SATA** drive, so the spare SATA has to go in a USB enclosure, or the tickets need
re-scoping to an internal SATA first. One rebuild serves all three data-filesystem tickets if the
drive gets four partitions: 4 GiB FAT32 ESP plus three ~150 GB data partitions, one each for
exFAT, ext4 and NTFS. `hype3b.cfg` and `input-scripts/usbsata-triple-write.txt` are the starting
point. Rebuilding retires the layout every boot since 2026-08-25 used, so **this one is the
operator's call.**

#653's remaining bar is criterion 6 only, the bare-metal Intel+AMD pass, and it is blocked by
#692 landing the fs-agnostic battery first. #754 wants a **second, sacrificial** USB device pulled
mid-write and must be the last act of the run.

## L4 — #126 + #178

Install a guest straight to the spare NVMe, then boot it natively outside hype to prove the
install is not virtualization-dependent (#126), and run the full persistence cycle across a host
reboot (#178).

## L5 — the AMD-only virtualization legs, no spare disk needed

These need only the laptop being AMD, but they still need a guest to boot, so they sit behind
#799 with the rest.

| Ticket | Scope |
| --- | --- |
| #600 | AVIC on bare metal; on PASS flip `-DHYPE_ENABLE_AVIC=1` to the default (decision 58) |
| #621 | NSVM-5: L0 -> L1 -> L2, hype hosting hype, the closing slice of #212 |
| #212 | the #621 parent — hype inside hype on real hardware |
| #478 | SMP-22: more vCPUs than pCPUs across three VMs, two different guest OSes. The laptop's 3 usable cores make it the sharpest over-commit target available |
| #625 #629 | CSM legacy boot and Secure Boot enforcing — both want the AMD half of an Intel+AMD pair |
