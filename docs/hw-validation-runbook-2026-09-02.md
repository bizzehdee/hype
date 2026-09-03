# Clearing the On Hold column with real hardware -- 2026-09-02

The board holds 22 tickets in On Hold. 15 of them need a real-hardware run and nothing else.
This runbook packs those 15 into **four boots**: one on the Intel box and three on the AMD
5950X. The other 7 On Hold tickets wait on a decision, a fix, or an artefact; they are listed
last so nobody schedules a boot for them.

Every boot below runs the **default build** of one tree. Stage once with
`tools/hw-val-2026-08-25/stage.sh`; the configs for every boot sit on the stick side by side,
and the operator copies the next one over `\hype.cfg` between boots. The microtest kernels and
the scratch images are staged with them. Log archive: `tools/hw-val-2026-08-25/logs/<boot>/`.

Ticket outcomes below use three words. **Closes**: the boot alone meets the ticket's bar.
**Leg**: the boot meets one of two required legs (SVM or VMX); the ticket closes when both
have run. **Records**: the boot produces the measurement the ticket asks for but cannot close
it.

| Boot | Machine | Config | Closes | Legs | Records |
| --- | --- | --- | --- | --- | --- |
| ~~Intel-A~~ DONE 2026-09-03 (five attempts) | i5-13420H | `hype2g.cfg` | #729 #795 #796 #797 #798 #698 #525 | #603 | |
| AMD-1 | 5950X | `hype1g.cfg` | #426 #775 #790 | #525 #603 | #641 #788 |
| AMD-2 | 5950X | `hype3g.cfg` (to write) | #388 #715 | | #660 #713 |
| AMD-3 | 5950X | drive rebuild, `hype3b.cfg` | #687 #688 #689 #653 #754 | | |

After Intel-A and AMD-1 both run, #525 and #603 close. Four boots close 14 tickets and finish
two more legs. #599/#605 need a fifth boot with a different binary and are a diagnostic until
#708 is fixed (below).

## Boot Intel-A -- staged, `RUN-CARD-2026-09-02-bootA-intel-vmx.md`

**First attempt (2026-09-03, build 1daa028) closed #729 and #603's VMX leg; vm0 stalled on the
CR0.CD fault fixed in f26b67c. Second attempt (build f26b67c) proved that fix and found the BSP
starved by the PS/2 poll, fixed in 92c23a0 (#796). Third attempt (build 92c23a0) met #796,
served the 0xCF9 reset from vCPU 1 on VMX, and hung the second boot on a stale VM-entry
interrupt, fixed in 478f6f8 (#797). Fourth attempt (build 478f6f8) confirmed #797's fix and found the
real cause of the hung second boot: the restart handed VMX the NPT table as its EPT root (all-UC),
fixed in da3e93d (#798). Fifth attempt (build
da3e93d) PASSED: second boot to login, AP timers climbing. Intel-A is done.**

One Alpine live guest (2 vCPUs, reboot pinned to CPU 1 by `input-2b/vm0.txt`) plus the three
#603 microtests. Seven physical cores with the BSP. If admission refuses a VM, fall back to
`hype2b.cfg` (Alpine only) then `hype2d.cfg` (microtests only): two boots, no re-staging.

| Ticket | Read | Passes when |
| --- | --- | --- |
| #525 (VMX leg) | `vm0 vCPU 1 guest reset via ACPI reset register (0xCF9)` once, then `SCRIPT vm0: PASS ... reboot-pin-nonbsp` | the restart was driven from vCPU 1 and the guest came back to a fresh login |
| #698 | `TMRLATE vm0/1: deliveries=` after `restarted (M8-4)` | climbing every sample for 10 minutes. `INTDIAG vm0/1` never sits at `pending=1 ... shadow=0x1` |
| #729 | `micro/vmexit: MSR round-trip (MTRR var0 base) wrote 0x123456000, read back` | `0x123456000` |
| #603 (VMX leg) | `micro/hello` PASS after `vmexit`'s triple fault and `vmexitstorm`'s watchdog force-off; no `PROBE FAIL` | |

Duration: 25 minutes. Operator actions: none after power-on.

## Boot AMD-1 -- the input run with the SMP legs folded in, `hype1g.cfg`

`hype1a.cfg`'s guest plus `hype1b.cfg`'s three microtests. `\input\vm0.txt` = `input-1a/vm0.txt`
(reboot-pin). Build: the first tree carrying 1ea2793 (the rate-bounded reset) and 61921d9.

1. Boot, stay on the dashboard. The microtests finish themselves in the first minute.
2. The script logs in, pins the reboot to CPU 1, reboots, logs in again and prints
   `reboot-pin-nonbsp`. Do not type until it has.
3. Then arm the Pico: **BOOTSEL once**, confirm `a0001` in the guest. Keep the Logitech mouse receiver and the
   Keychron attached. Leave the two spare USB drives plugged in (#780's condition, kept).
4. Leave it 90 minutes from the second login. Type on the Keychron now and then. The Logitech (046d:c547) is a mouse; its receiver exposes a keyboard HID interface, which is why hype counts `keyboards=3`. The two real keyboards are the Keychron (3434:0da4) and the Pico (cafe:4b44).
5. Power off. Bring back `HYPE.LOG` and `RUN1A.LOG`.

| Ticket | Read | Passes when |
| --- | --- | --- |
| #525 (SVM leg) | as Intel-A | recorded 2026-09-03 (bootAMD1-1): 0xCF9 from vCPU 1, restart, second login, `reboot-pin-nonbsp` |
| #698 (SVM, hardware) | `TMRLATE vm0/1` after the restart | recorded 2026-09-03 (bootAMD1-1): deliveries 7452 -> 48395 after the restart |
| #603 (SVM leg) | as Intel-A | ~~as Intel-A~~ **PASSED 2026-09-03, run bootAMD1-1** (`hello` PASS after `vmexit`'s triple fault and `vmexitstorm`'s watchdog force-off); #603 closed |
| #426 | the run reaching 90 minutes with input live at the end (bootAMD1-1 gave 40 minutes, input live, 0 stalls; the 90-minute form is still owed) | no `left dead`, no `REFUSED`, keyboards typing at power-off. Boot 42 reached 71 minutes; the rate bound (#792) removes the cap that would have ended it |
| #775 | `CTRLSILENCE` / `XHCIRESET` | every stall is followed by `reset #N done ... keyboards=3`. The fault is #781's controller death, not a lost completion; close as answered by #781-#785 |
| #790 | `cmdring timeouts=` and `REVIVE` | `timeouts=` equals the number of `CTRLSILENCE` No-Ops and nothing else; 0 `REVIVE`. Met in boots 40-42 already; this run is the 90-minute form the ticket asked for |
| #641 | `APVCPU vm0/N: exits=`, `PERF: hlt_wait=` | recorded. The fix is a design decision, not a run |
| #788 | doubled characters per 1,000 in `KBDCHARS` | recorded. Boot 42 was 2.7; bootAMD1-1 (40 min) 2.6; no fix has landed |

Duration: 100 minutes.

## Boot AMD-2 -- physical write targets, `hype3g.cfg` (write it before staging)

Merge `hype3c.cfg` (NVMe `5ME3N005713803V2W`, AHCI `<AHCI-SCRATCH-SERIAL>`) and `hype3d.cfg`
(USB SanDisk `4C530201070308103214`) into one config with three VMs writing at once. **The AHCI
serial is a placeholder.** Without a sanctioned SATA scratch drive, drop the AHCI VM and run
NVMe + USB; #713's original symptom needs the AHCI+NVMe pair, so it is then recorded, not
reproduced. Never the internal NVMe, never the SABRENT (the boot and log medium). Each physical
target needs the dashboard confirm by hand (#125): budget five minutes of typing at the start.

| Ticket | Read | Passes when |
| --- | --- | --- |
| #388 | `m5-8: target_disk` resolving `physical:4C530201070308103214`, the confirm prompt, the guest's marker | the marker is on the SanDisk and nowhere else; naming the boot medium is refused (`hype3e.cfg` is that refusal test, one extra boot if wanted) |
| #715 | `DIAG: BLK WRITE ... vec=` on the NVMe VM | non-zero merged segments, written range exact |
| #660 | `nvme_lock_contended=`, `bsp_nvme_timeouts=` | recorded. Contention needs two writers on one NVMe controller; this config has one. Non-zero closes it, zero says nothing |
| #713 | `FBSPEED t=` against elapsed, `PREEMPT`, `LOOPPHASE` | recorded. Closing needs the scope decision the ticket asks for |

Duration: 30 minutes of writes.

## Boot AMD-3 -- the storage battery on a rebuilt drive, `hype3b.cfg`

**This boot needs the drive rebuilt and that is the user's call.** #687/#688/#689 want a
512 GB USB-SATA drive as a 4 GiB FAT32 ESP plus a data partition in exFAT, ext4 and NTFS
respectively. Rebuilding the hw-val drive retires the layout every boot since 2026-08-25 used.
One rebuild serves all three if the drive gets **four** partitions (4 GiB FAT32 + three ~150 GB
data partitions, one per filesystem) and the config runs three VMs, one sourcing from each.
`hype3b.cfg` and `input-scripts/usbsata-triple-write.txt` are the starting point.

| Ticket | Read | Passes when |
| --- | --- | --- |
| #687 #688 #689 | `host-fat`/`host-ext`/`host-ntfs ... resolved` for an ISO and a vdisk on each data partition; logs on partition 1 only | all three resolve and stream, the guest boots from each ISO, and `fsck.vfat -n`, `fsck.exfat -n`, `e2fsck -fn`, `ntfsfix -n` are clean afterwards |
| #653 | the fs-agnostic write battery's PASS lines (#692 landed it) | PASS on every filesystem. The Intel half of its bar rides the next Intel boot |
| #754 | a **second, sacrificial** USB device pulled mid-write, then re-plugged | `DIAG: GONE ... refused=` climbs, hype stays up, nothing resumes without `attach`, the boot medium is untouched. **Last act of the run** |

Duration: 60 minutes plus the rebuild.

## Boot Intel-B -- APICv, `hype2c.cfg` (staged 2026-09-03)

`-DHYPE_ENABLE_APICV=1` build active, `hype2c.cfg`, `input-2c` (login only). #599 and #605 ride
it. #708's candidate fix is in the tree: the BSP HLT wake reads RVI when APICv is live (boot 2c's
dump had `pending_valid=0` with `gis=0x30ec`, the vector in the page and not in the software
IRR), and `HLTSHADOW` counts the deadlock case. Card:
`tools/hw-val-2026-08-25/RUN-CARD-2026-09-03-bootIntelB.md`. If the guest reaches login the
boot is a #605 gate; if it hangs it is the #708 diagnostic and #605 is untested, not failed.
Stage with `stage.sh --boot intelb`; `stage.sh --boot amd1` puts the AMD staging back.

## Decisions this runbook needs

1. Rebuild the hw-val drive with four partitions for AMD-3 (retires the current layout).
2. Name a sanctioned SATA scratch drive by serial for AMD-2, or accept NVMe + USB only.
3. Provide the sacrificial USB device for #754.
4. #641: move to Doing for a fix, or leave On Hold. Three hardware runs already reproduce it.
5. ~~#790~~ closed 2026-09-03 on boots 40-42 (operator's call); #775 closed the same day as answered by #781-#785.

## On Hold, no boot helps

| Ticket | Waits on |
| --- | --- |
| #442 #634 #635 #636 | #695, an installed Windows guest image |
| #599 #605 | #708, the APICv hang |
| #641 | a fix (decision 4) |
| #713 | a scope decision; AMD-2 records it |
| #788 | a fix; AMD-1 records it |
| #660 | a two-writer NVMe configuration that does not exist yet; AMD-2 records it |
