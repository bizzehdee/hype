# Boot AMD-L0 (AMD **laptop**, not the 5950X) -- five minutes for #799

This is the cheapest boot in the queue and it gates the other five. It answers one question:
**where does the AMD laptop's vCPU-0 loop spend its time?** Everything else planned for this
machine needs a guest kernel to reach userspace, and on this laptop it does not.

Build: default, no `EXTRA_CFLAGS`. Active config `\hype.cfg` = `hype1a.cfg` -- one 2-vCPU Alpine
live boot. This is deliberately the **same config and input script** as the 2026-09-03 laptop
attempt that measured 235 us per exit, so the two runs compare directly. Do not "improve" the
config for this boot; a different config makes the comparison worthless.

The laptop is **cold-boot only and has no serial port.** `HYPE.LOG` on the drive is the entire
record. There is nothing to watch on a terminal.

## Before you boot

- Banner reads `build 952e364-dirty` for run 2 (run 1 was `17785e9-dirty`). `-dirty` is the
  `edk2` submodule and untracked `tools/789/` and `tools/799/` files, none of which is compiled
  in.
- The active build must NOT carry the APICv marker -- verified at staging time
  (`HYPE_ENABLE_APICV set` absent, `HOUSECOST` present).
- Nothing else needs checking. No spare disks are touched by this config; `target_disk` is a file
  on the drive and exists only to satisfy admission's "needs a disk" check.

## The sequence

1. Cold-boot the laptop from the drive.
2. Stay on the dashboard. **Do not type.** The guest kernel spins from about t=10 s, so the
   evidence is complete long before five minutes are up.
3. After five minutes, **`host off` first, then power off if it parks.** Typing `host off` at the dashboard is the only path that reaches `usb_log_fatal_flush()` -- the last-gasp drain of the log ring. Pulling the power skips it and truncates the log mid-line, losing the last 2-25 KB, which is the newest and usually the most interesting part. If the firmware has no S5 path hype parks and says so; the flush has already happened by then, so holding the power button after that is safe.
4. Bring back `HYPE.LOG`.

Leaving it longer than five minutes costs machine time and adds nothing: the counters are
cumulative and the shape is set in the first minute.

## What to read

One line, printed next to `LOOPPHASE`:

```
fw-1 HOUSECOST vm0: s2= ... s75=
```

The section holding the mass is the answer. Read it alongside:

```
fw-1 LOOPPHASE: diag= persist= house= dispatch=
fw-1 DRAIN: iters=
```

`house` divided by `iters` is the per-iteration housekeeping cost. Use that denominator, not exit
counts, so the number is comparable with the reference figures below.

| Ticket | Read | Outcome |
| --- | --- | --- |
| #799 | `HOUSECOST vm0: s<N>=` names one dominant section | root cause identified; fix it, then run L1 |
| #799 | the Alpine kernel **does** reach userspace this time | #799 was build- or config-specific, not a property of this laptop; skip straight to L1 and say so on the ticket |

## Reference figures to compare against

| Run | Machine | Housekeeping |
| --- | --- | --- |
| 2026-09-03 laptop attempt (`70d6b0f`) | AMD laptop, 4 cores | `house=77.9 s of 180 s` = **235 us per exit**; kernel never left its early RDTSC delay loops |
| boot AMD-1 run 2 (`447cbb0`, 119 min) | 5950X, 16 cores | `house=92773ms`, `DRAIN: iters=1574688` = **59 us per BSP loop iteration** |
| boot AMD-1 run 1 (`70d6b0f`, 40 min) | 5950X, 16 cores | `house=36587ms`, `iters=1029075` = **36 us** |

So the desktop sits at 36-59 us and the laptop at 235 us. The gap, not the absolute number, is
what #799 is about.

## Result -- boot AMD-L0, 2026-09-03 (logs in `logs/bootAMDL0-1/`)

Build `17785e9-dirty`, `hype1a.cfg`, ~130 s of guest time. Symptom reproduced exactly: the guest
reached GRUB's `Booting \`Linux lts'` and emitted nothing after that.

**Section 5 is the answer, and it is `hype_input_runner_scan` over the whole reconstructed
screen.**

| Measurement | Value |
| --- | --- |
| `HOUSECOST vm0: s5=` | 47,544 ms |
| all housekeeping sections summed | 50,014 ms -- so **s5 is 95.1% of housekeeping** |
| `LOOPPHASE: house=` | 50,005 ms (agrees with the sum, so the accounting is closed) |
| wall time at that sample (`FBSPEED t=`) | ~123 s -- so **s5 is 37% of wall time** |
| `DRAIN: iters=` | 283,259 -> **176 us of housekeeping per loop iteration** |

Section 5 is the span between markers 5 and 6 in `boot/main.c` (`fw_1_loop_section(vm, 5u)` at
:19022, `6u` at :19072). It holds exactly two things: two `fw_1_drain_uart_console` calls and the
`in_script_armed` screen-snapshot block.

**The UART drain is ruled out.** `fw_1_drain_uart_console` (`boot/main.c:7893`) does no host port
I/O at all -- it drains a software queue -- and `UARTTX: COM1 written=4499` was constant from
t=~84 s onward, so the queue was empty for the rest of the run. An empty-queue call cannot cost
84 us.

**So it is the snapshot block**, which copies every cell of the grid (1920x1080, on the order of
16 KB) into `vm->diag.snap` and hands it to `hype_input_runner_scan` at 10 Hz. 123 s at 10 Hz is
1,230 scans, so **38.7 ms per scan.**

`hype_input_runner_scan` (`core/input_runner.c:201`) calls `text_contains` -- a naive
O(len x patlen) double loop (`:169`) -- once per fail-if (this script has two: `soft lockup`,
`STALE-SHELL`), once or twice more to settle the current expect's gate, and once to pre-measure
the next expect; then it feeds all ~16 KB through the per-byte matcher.

**This is a property of the code, not of this laptop.** Boot AMD-1 run 2 on the 5950X ran the
same build shape, the same 1920x1080 grid and the same 21-directive script, and recorded
`s5=10409ms`. Its script finished at 80,622 ms, and `hype_input_runner_scan` returns immediately
once `r->done` is set -- so it scanned for 80.6 s, i.e. 806 scans: **12.9 ms per scan.** 806 x
12.9 ms = 10.4 s, which is `s5` to three digits. The desktop pays the same cost; it just stops
paying after 80 seconds.

### Why that is fatal here, in one chain

1. The guest kernel sits in an early RDTSC delay loop with interrupts off:
   `GUESTPC vm0: lastreason=0x6e lastrip=0xffffffffafc81d7e ... rflags=0x2`, the same RIP in 16 of
   the last 30 samples. `0x6e` is SVM `VMEXIT_RDTSC`. `ioio` is frozen at 79,416 while `npf` and
   `vintr` keep climbing, so the guest is executing and silent.
2. hype intercepts RDTSC deliberately (#438, `boot/main.c:13210`), so **every iteration of that
   delay loop is a VMEXIT** -- 283,268 of them in 123 s.
3. Every one of those exits runs the host loop, which is spending 38.7 ms of every 100 ms in the
   screen scan: `BSPPROBE vm0: last=0x6e@0xffffffffafc81d7e IN-HOST for 79ms section=5`.
4. The script's first expect is `localhost login:`. It cannot match until the guest boots, and the
   guest cannot boot while the scan owns the loop. **The two wait on each other.**

The 5950X escapes only because its guest reaches a login prompt before the cost matters.

### Proposed fix

`core/vt_screen.c:3` already maintains `s->generation`, incremented by `touch()` on every screen
change, and the snapshot block does not consult it. Gating the snapshot and scan on
`vm->term.generation` having moved since the last scan makes a silent guest cost nothing: no new
bytes, no new generation, no scan. This run would have proceeded. Nothing about the matcher's
semantics changes -- rescanning an unchanged screen cannot produce a match a previous scan missed.

Worth doing on top: `text_contains` is called 4-5 times per scan over the full grid before the
per-byte pass, and the per-byte pass then covers the same bytes again.

### Second, independent finding: the core oscillates between ~3.46 GHz and ~400 MHz

`FBSPEED` is strictly bimodal across the run, alternating between adjacent one-second samples:

| | fb | ram | cli | chunk max |
| --- | --- | --- | --- | --- |
| fast state | 78 us | 54-58 us | 69 us | 1 us |
| slow state | 328-334 us | 492-498 us | 335-337 us | 5 us |
| 5950X, for scale | 18 us | 14-15 us | 18-19 us | 0-1 us |

`FBCLOCK` is bimodal in the same windows: `delivered/nominal` is either ~1650/1000
(aperf ~240,000, mperf ~146,000) or exactly 190/1000 (aperf ~135,000, mperf ~707,000-718,000).
The slow windows are 4.84x longer in mperf reference time, and `cli` is 4.86x slower in them --
the same ratio, so these are the same windows. 1.65x nominal is ~3.46 GHz; 0.19x is ~400 MHz.

**This is not the #795 CR0.CD class.** Host `cr0=0x80000011` in all 130 `FBCLOCK` samples: CD
(bit 30) and NW (bit 29) are clear, so the caches are on. `therm=0x0` says nothing here -- it
reads an Intel thermal MSR that does not exist on this part.

Fix section 5 first. It is 37% of the loop, it is hype's own code, and it is measurable on both
machines. Then re-run L0 and see how much of the 235 us/exit figure is left.


## Run 2 -- the same boot, after the section-5 fix (`952e364`)

Run 1 found the cause: `HOUSECOST` section 5 -- the input-script screen scan -- was 95% of loop
housekeeping and 37% of wall time, because the script's first `expect` could not match while the
guest was silent, and the scan was throttled to 10 Hz but never gated on the screen having
changed. `952e364` gates it on the `vt_screen` generation or the runner's pc having moved.

Sandbox before/after at the same 1920x1080 grid (`tools/799/run-799-idlescan.sh`), successive
`s5` samples: `0 1345 1982 2605 2827 2844 2861` ms before, still climbing, against
`0 983 1025 1025 1025 1026 1026` ms after, flat. Once the guest goes quiet the fix costs +43 ms
where the old code cost +1516 ms over the same 150 s.

**The run is exactly the same as run 1.** Same config, same input script, same five minutes,
same one line to read. Do not change anything: run 1 is the baseline and only the binary differs.

### What run 2 answers

| Read | Run 1 | Run 2 passes when |
| --- | --- | --- |
| `HOUSECOST vm0: s5=` | 47,544 ms, 95% of housekeeping | a small fraction of housekeeping, and it stops growing once the guest goes quiet |
| `LOOPPHASE: house=` / `DRAIN: iters=` | 50,005 ms / 283,259 = 176 us per iteration | far below 176 us; the residue is what is left of #799's 235 us/exit |
| the guest | GRUB `Booting \`Linux lts'`, then silence | **anything past it.** If the kernel now prints, the RDTSC-loop starvation was the scan, and the rest of the AMD-laptop queue unblocks |
| `FBSPEED ram=` / `FBCLOCK` | bimodal 54-58 us / 492-498 us and 1650 / 190 per 1000 | expected UNCHANGED -- this fix does not touch the clock oscillation. Record it, do not read it as a failure |

If the guest still stops at `Booting \`Linux lts'`, section 5 was a large amplifier but not the
whole cause, and the next suspect is the bimodal clock rather than anything in hype's loop.


## Result -- run 2, 2026-09-03, build `a835bc6-dirty` (logs in `logs/bootAMDL0-2/`)

**The fix works on its target and does not fix the boot.** Both halves matter.

### Section 5 is gone

| | run 1 (`17785e9`) | run 2 (`a835bc6`) | |
| --- | --- | --- | --- |
| `HOUSECOST s5=` | 47,544 ms | **377 ms** | 126x less |
| `LOOPPHASE house=` | 50,005 ms | **4,876 ms** | |
| `DRAIN iters=` | 283,259 | 444,140 | |
| housekeeping per iteration | **176 us** | **11 us** | 16x less |

So this ticket's headline measurement -- 235 us per exit in vCPU-0 loop housekeeping -- is
answered. It is now 11 us.

### The guest is still stuck, and the exit rate barely moved

`Booting \`Linux lts'` and nothing after, exactly as run 1. `GUESTPC vm0: lastreason=0x6e
lastrip=0xffffffff9ac81d7e ... rflags=0x2` -- still SVM `VMEXIT_RDTSC` at a single RIP with
interrupts off, `ioio` frozen at 83,881, `hlt=0`.

| | run 1 | run 2 |
| --- | --- | --- |
| wall time | 123 s | 220 s |
| vCPU 0 exits | 275,225 | 444,140 |
| **per exit, total** | **447 us** | **496 us** |
| of which housekeeping | 176 us | 11 us |
| of which `s75` | 252 us | 460 us |

**Removing 165 us per iteration of real work did not reduce the per-exit cost.** It moved into
`s75`. So `s75` is not work, it is a wait -- something in that span is elastic and soaks up
whatever slack appears in front of it. That is the next thing to find, and section 5 was an
amplifier sitting inside it, not the limit.

### The comparison that localises it

vCPU 0 and vCPU 1 both have a dedicated physical core --
`AP[vm0 vCPU 0]-SMOKETEST: apic_id=2`, and `SMP: vm0 granted 2 whole physical core(s)`. They run
**different loops**: vCPU 0 runs the full `fw-1` loop, vCPU 1 the lighter AP vCPU loop. In the
same run:

| vCPU | loop | exits | per exit |
| --- | --- | --- | --- |
| vm0/0 | `fw-1` | 444,140 | **496 us** |
| vm0/1 | AP vCPU | 37,195,936 | **5.4 us** |

**92x, on the same machine, both on dedicated cores.** The cost is a property of the `fw-1` loop.

`s75` is also the one section that has never been split: the sub-markers 761 and 768 exist
(`boot/main.c:19288`, `:19308`) but are **not in `g_housecost_codes`**, so `fw_1_loop_section`
finds no slot and silently drops their time -- 11 s of this run's 220 s is unaccounted for that
reason (sections sum to 209,153 ms of 220,505 ms).

### Do not misread the BSP's own numbers

`BSPCOST` sums to ~220 s of a 220 s run (render 26%, idle 24%, input 15%, kbddiag 14%, fbprobe
8%). That is the **BSP's** console loop on APIC 0, and vCPU 0 is on APIC 2, so it is not what
starves the guest. Recorded to stop the next reader chasing it.

### The clock oscillation is unchanged, as predicted

33% of samples in the slow state in run 2 against 34% in run 1 -- `FBCLOCK` 74/221 and 44/130 at
exactly 190/1000, `FBSPEED ram=` identically split. Host `cr0=0x80000011` throughout. This fix
does not touch it and did not.

### Standing lead worth acting on independently

`arch/x86_64/svm/vmcb.h:185` says it plainly: *"RDTSC is normally allowed to run directly, but
nested KVM can expose a stopped L2 counter. Intercept it so hype can return the advancing L1 TSC
plus the guest's configured offset (#438)."* The intercept is in the unconditional baseline set
(`vmcb.c:59` and `:165`), so **bare metal pays a dev-rig workaround.** This guest is doing nothing
but RDTSC. hype has no "am I running under a hypervisor" probe today (`hype_cpu_detect_vmm_kind`
distinguishes Intel from AMD, not nesting), so this needs CPUID leaf 1 ECX bit 31 and a
conditional intercept.


## Result -- run 3, 2026-09-03, build `af1595a-dirty` (logs in `logs/bootAMDL0-3/`)

**The first boot on this machine whose guest reached userspace.** `#802` removed the RDTSC
intercept that had been holding it in an early delay loop, and `#801`'s section split made the
next cost visible instead of a guess.

### The guest booted

`RUN1A.LOG`, first boot: GRUB, then the kernel, then

```
ttyS0| l/reboot/cpu)
ttyS0| CPU-PINNED-1
ttyS0| localhost:~#
ttyS0| clear
ttyS0| localhost:~# reboot
```

and `HYPE.LOG` confirms the restart chain fired from the non-BSP vCPU:

```
fw-1: vm0 vCPU 1 guest reset via ACPI reset register (0xCF9) -> restart [#94 #525]
fw-1: vm0 restarted (M8-4): pristine firmware restored, RAM zeroed, vcpu reset
```

`GUESTPC vm0: lastreason=0x78 lastrip=0xffffffffb6c81c2e ... rflags=0x246` -- HLT with interrupts
enabled, and **zero `lastreason=0x6e`** anywhere in the run. The RDTSC delay loop that defined
runs 1 and 2 is gone.

### #799's symptom is resolved; the remaining hang is #803, and only on the restart

There is no `SCRIPT vm0: PASS`. The script armed with 21 directives, drove the first boot to
login, pinned the reboot and issued it -- and the second `expect localhost login:` never matched,
because the guest stalled after the restart.

That is **#803**, not #799. Its symptom is written in its own title: *first boot reaches login,
but a guest restart always stalls at `Booting \`Linux lts'`*. Same string as #799, different
cause, different leg. Evidence in this run:

```
host-xhci: #377 rejected incomplete transfer (slot=1 ep=4 trb=0x141b39000 cc=4 residue=31 len=31)
host-xhci: #377 rejected incomplete transfer (slot=1 ep=3 trb=0x141b38000 cc=4 residue=512 len=512)
```

Five of them, `residue == len` every time, so zero bytes moved. #803's own lead: nothing resets
the ATAPI device model or the xHCI controller when a guest restarts.

**Do not read a future run's missing `SCRIPT PASS` as a regression while #803 is open.** Any run
of this card reboots the guest and will stall there.

### Where the loop's time went -- #804, found here

`#801`'s split landed in this build and immediately named its successor:

```
HOUSECOST vm0: s2=2710ms s21=0ms s22=35ms s23=83ms s5=654ms s6=331ms s7=115ms s72=207ms
               s73=313ms s74=42ms s75=216ms s761=0ms s768=45ms s764=7ms s769=117ms
               s781=0ms s782=23ms s795=11643ms s796=291ms s797=1ms s798=10095ms
               s79=83006ms s80=6141ms s81=3918ms
LOOPPHASE: diag=429ms persist=99ms house=4377ms dispatch=16511ms
DRAIN: iters=427804
```

| | value | per iteration |
| --- | --- | --- |
| `house` (all housekeeping) | 4,377 ms | **10 us** |
| `s79` (pre-entry prologue) | 83,006 ms | **194 us** |
| wall (`FBSPEED t=`) | 144,812 ms | -- |

**`s79` alone is 57% of wall time**, and `s75` -- the section that looked like a 460 us elastic
wait in run 2 -- is now 216 ms. That confirms run 2's reading was an accounting artefact, not a
wait: `s75` had been charged the guest's own run time.

`s79` is the #436 Windows PE-base scan running on every kernel-mode exit, which is
**#804**, fixed after this run in `7d621a2` (7.3x on the nested rig: `s79` 13,528 ms -> 1,843 ms).
This run is its hardware baseline.

Housekeeping is now 10 us per iteration, against #799's original 235 us per exit.

### Unchanged and still unexplained

The clock oscillation: 49 of 144 `FBCLOCK` samples at exactly 190/1000 -- 34%, the same
proportion as runs 1 (34%) and 2 (33%).

### How it ended

On the power button: zero `fw-1 HOST:` lines, log truncated mid-line. This run predates
`f114aae`, which is what that commit exists to stop -- see the standing rule in
`docs/hw-validation-runbook-2026-09-02.md`.


## Run 4 as planned -- superseded by the actual run 4 recorded below. Kept for the read list.

## Run 4 -- confirm #804 on hardware, close #799, and exercise the flush

Same config and script as every run of this card. Five minutes is enough: run 3 reached login and
issued its reboot inside the first ~2.5 minutes of wall time.

### The sequence

1. Cold-boot from the drive. Stay on the dashboard, do not type.
2. Let the script drive the first boot to `localhost:~#` and issue its reboot. **The restart will
   stall at `Booting \`Linux lts'` -- that is #803 and it is expected.** Do not wait for a second
   login and do not record a missing `SCRIPT vm0: PASS` as a regression.
3. At about five minutes, type **`host off`** at the dashboard. Not the power button.
4. Bring back `HYPE.LOG` and `RUN1A.LOG`.

### What to read

| Ticket | Read | Passes when |
| --- | --- | --- |
| **#804** | `HOUSECOST vm0: s79=` against `DRAIN: iters=` | far below run 3's **194 us per iteration** (83,006 ms, 57% of wall). This is #804's stated bar -- the nested A/B was 7.3x, and hardware is the number that counts |
| **#799** | `LOOPPHASE house=` / `DRAIN iters=`, and the guest reaching `localhost:~#` | housekeeping stays at run 3's ~10 us and the guest boots. Both halves then hold on the fixed build and **#799 closes** -- its original figure was 235 us per exit |
| **#806** | `flush:` / `fw-1 HOST:` lines after `host off` | a **non-zero** byte count. This is the flush's first non-zero exercise anywhere: #807 means no QEMU rig can show it, because `tools/338`'s log sink has not mounted since #638 |
| **#803** | `host-xhci: #377 rejected incomplete transfer ... cc=4 residue=` | recorded, not fixed. Count them and note whether `bot_recover()` restored the datapath |
| clock | `FBCLOCK ... 190/1000` share | expected unchanged at ~33-34%, as in all three prior runs. Still unexplained, still nobody's ticket |

If `s79` has collapsed and the guest still reaches login, this machine's queue
(`docs/hw-validation-amd-laptop-2026-09-03.md`) moves to **L1** -- #713/#715/#660, the physical
AHCI+NVMe write pair. Read the #660 caveat there first: contention needs two writers on ONE NVMe
controller and `tools/hwstick/hype.cfg` has one, so as staged that run can only record zero.


## Result -- run 4, 2026-09-04, build `315bbdc-dirty` (logs in `logs/bootAMDL0-4-315bbdc/`)

Found already done on the drive when re-staging for it. `315bbdc` is the last of the three #803
commits (`99e5ba9`, `72b02ab`, `315bbdc`).

**#804 is confirmed on hardware, decisively. The run says nothing about #803, because it was
stopped before the guest reached login.**

### #804 -- the bar is met

| | run 3 (`af1595a`) | run 4 (`315bbdc`) | |
| --- | --- | --- | --- |
| `HOUSECOST s79=` | 83,006 ms | **86 ms** | |
| `DRAIN iters=` | 427,804 | 149,620 | |
| **s79 per iteration** | **194 us** | **0.57 us** | **340x** |
| s79 share of wall | 57% | 0.26% | |

The nested A/B predicted 7.3x and warned that nested understates it. Hardware gives 340x per
iteration. `s79` is no longer in the top ten sections; the loop's time now sits where it should,
in `s795` (NPF, 12,672 ms) and `s798` (HLT, 9,313 ms) -- both guest-serving.

### #799 -- housekeeping holds at ~10 us

`LOOPPHASE: house=1431ms` over `iters=149620` = **9.6 us per iteration**, matching run 3's 10 us.
Against the 235 us per exit this ticket was filed on. Both runs agree on the fixed build, and
run 3 already showed the guest reaching `localhost:~#`, so both halves of #799 are answered.

### This run was cut short -- do NOT read it as a #803 regression

There is no login, no `0xCF9` restart and no `SCRIPT vm0: PASS`, and the guest's last state is
`Booting \`Linux lts'`. That looks like run 1 and run 2's hang, and it is not. Every measure says
the run simply ended early, at about half of run 3:

| | run 3 | run 4 |
| --- | --- | --- |
| `HYPE.LOG` | 378,383 B | 179,827 B |
| `RUN1A.LOG` last stamp | 265,780 | 150,244 |
| `DRAIN iters=` | 427,804 | 149,620 |
| wall (`FBSPEED t=`) | 144,812 ms | 33,342 ms |

Run 3 reached GRUB at `RUN1A` stamp ~83,400 and login at ~265,600. Run 4 ends at 150,244 -- short
of where run 3's login appeared. And the guest was healthy when the log stops, not stuck:
`GUESTPC vm0: lastreason=0x78 ... rflags=0x246` (HLT with interrupts enabled) and
`EXHIST ... hlt=1411 npf=31262 ioio=76014` -- an idling, progressing guest, not the frozen
`ioio` and `0x6e` signature of runs 1 and 2.

### #803 -- still 6 rejected transfers, still unproven

```
host-xhci: #377 rejected incomplete transfer (slot=1 ep=4 trb=0x141b3a000 cc=4 residue=31 len=31)
host-xhci: #377 rejected incomplete transfer (slot=1 ep=3 trb=0x141b39000 cc=4 residue=4096 len=4096)
```

Six of them, `residue == len` again, so the failures themselves have not gone away on the #803
build. Whether the bounding work fixed the *restart* stall is untested: this run never restarted.

### How it ended

Power button again -- zero `fw-1 HOST:` lines. `315bbdc` predates `f114aae`, so the operator did
not yet have the `host off` instruction. Run 5 has it.

## Run 5 -- the full form: #803's restart leg, and the flush

Same config. **Long enough to reach login and get through the reboot** -- run 3 needed about
2.5 minutes of wall time to reach login, so give it ten and do not stop early. Run 4's whole
lesson is that a short run reads as a hang.

1. Cold-boot. Stay on the dashboard, do not type.
2. Wait for `localhost:~#`, then for the script's reboot, then for the **second** login.
3. Type **`host off`**. Not the power button.
4. Bring back `HYPE.LOG` and `RUN1A.LOG`.

| Ticket | Read | Passes when |
| --- | --- | --- |
| **#803** | a second `localhost login:` after `vm0 restarted (M8-4)`, and the `cc=4` count | the restart reaches login. That is the ticket's own symptom and the only thing that closes it |
| **#803** | `SCRIPT vm0: PASS pass (21 directive(s))` | the script completes -- it has never done so on this machine |
| **#806** | the `host off` result line | a **non-zero** byte count; #807 means no QEMU rig can show it |
| clock | `FBCLOCK ... 190/1000` share | expected unchanged at ~33-34%, as in runs 1-3 |


## Result -- run 5, 2026-09-04, build `a61fe0f-dirty` (logs in `logs/bootAMDL0-5/`)

Operator report: **first boot fully successful, second boot showed IO errors on syslinux, the log
was 90 KB behind, and then hype was fully frozen -- nothing could be typed.**

Three separate findings. Only the first is #803.

### 1. #803's first half works, the restart leg does not

The script drove the first boot to login and the restart chain fired from the non-BSP vCPU:

```
fw-1: vm0 vCPU 1 guest reset via ACPI reset register (0xCF9) -> restart [#94 #525]
fw-1: vm0 restarted (M8-4): pristine firmware restored, RAM zeroed, vcpu reset
```

The second boot then reached GRUB and stopped at `Booting \`Linux lts'` (`RUN1A.LOG` stamp
223,545) and the guest printed nothing further. There is no `SCRIPT vm0: PASS`.

**New detail, and it changes the suspect list.** At the end the guest is
`lastreason=0x78 ... rflags=0x246` -- HLT with interrupts *enabled*, waiting for an interrupt
that never arrives. That is not runs 1 and 2's signature (RDTSC spin with IF=0). And there are
**no `cc=4` rejections after stamp 148,272**, well before the restart at 204,288 -- so the six
BOT failures in this run all belong to the *first* boot, which succeeded anyway. The restart
stall is not accompanied by fresh transfer errors.

### 2. hype was NOT frozen. The host keyboard path was.

Everything says the loop was alive to the last line: `DRAIN: iters=308514` still climbing,
`GUESTPC` sampling, `FBSPEED`/`FBCLOCK` alternating normally, and `ps2reads` climbing
158,444 -> 243,916 across the final samples. hype was polling the i8042 thousands of times a
second.

What stopped was byte delivery:

```
KBDIRQ: isr_entries=107 (+0 since last) eois=107 | polled=112 chords=13 ps2polled=5
        ps2reads=243916 max=12us | bsp_usb_timeouts=9 ...
KBDPOLL: p64 22675ms ago val=0x05 rip=0xffffffff968db490 | p60 22675ms ago val=0xfa
```

`polled` frozen at 112 and `isr_entries` frozen at 107 while `ps2reads` climbs by ~17,000 per
sample. Port 0x64's value last *changed* 22.7 seconds before the log ends, to **0x05 -- OBF set**
-- with port 0x60 reading **0xFA**, a PS/2 ACK rather than a scancode.

It happened twice. `p64_ago` grew 20.7 s -> 40.7 s in the first window, recovered (`val=0x04`,
`ago=134ms`, then `ago=11ms`), then froze again for the final 22.7 s:

| `RUN1A`/log stamp | `p64_ago` | `p64` | `p60` |
| --- | --- | --- | --- |
| 150,990 -> 197,866 | 20,676 -> 40,678 ms | 0x05 | 0xfa |
| 206,574 | 134 ms | **0x04** | 0xfa |
| 225,744 | 11 ms | **0x04** | 0xfa |
| 242,964 -> 265,135 | 2,667 -> 22,675 ms | 0x05 | 0xfa |

So the operator could not type `flush` or `host off`, which is why this run ended on the power
button like every other. **This is what made the machine look frozen, and it is a different bug
from #803.** Raised separately.

Note for anyone reading the absences: `host-hid: no USB boot keyboard on any controller (PS/2
host keyboard only)`. There were no USB keyboards this run, which is why the log has **zero**
`HIDTICK`, `CTRLSILENCE`, `XHCIRESET` and `REVIVE` lines. Their absence is not a fix.

### 3. The log lost ~4.2 KB mid-run and ~90 KB at the end -- but it is NOT out of order

**Correction to the first version of this section**, which claimed the file's last 26,799 bytes
were older content written after the newest record. That was wrong and the error was mine: I
treated the produced-stream stamp 267,153 as a FILE offset and read from there, which lands
mid-file on content that is exactly where it belongs. The file ends cleanly on `[0000267153]` at
file offset 293,836, and `tools/log-fsck/run-log-order.sh` (added for #809) reports `backward=0`
on this log and on all six others in `logs/`.

What is real is loss, and the log names it itself:

```
[0000218009] usb-log: BEHIND -- logbuf has 218009 bytes, file has 205575 (#338)
[0000222322] fw-1 FBSPEED: ...
```

80 bytes apart in the file, 4,313 apart in the produced stream: **~4.2 KB of records absent**,
immediately after the sink's own BEHIND warning. Every AMD-laptop run has exactly one such gap
and both 5950X runs have none -- despite the desktop falling behind 87 and 254 times against the
laptop's 2-4. So falling behind is not the cause.

The flush's own account of itself never showed the operator's 90 KB:

```
USBFLUSH: slices=85 drained=465794B(all sinks) total=4561ms max=1507067us
          produced=263273B behind=2810B peak=55005B | bursts=24 caught=22 stalled=2
```

`behind=2810B` at the last sample, but `max=1507067us` -- **one flush slice took 1.5 seconds** --
and `stalled=2` of 24 bursts. Every earlier run on this machine had `stalled=0`. So the sink was
struggling, the operator saw it 90 KB behind on the dashboard, and the last ~90 KB of the run --
including whatever the second boot's syslinux IO errors printed -- never reached the medium.

Raised separately. This is the first run where the log sink has demonstrably lost data, which is
also why the `flush` verb (#806) could not be exercised: there was no way to type it.

### Unchanged

`FBCLOCK` still bimodal, `cr0=0x80000011` throughout.


## Result -- run 6, 2026-09-04, build `d51ceaf-dirty` (logs in `logs/bootAMDL0-6/`)

The `KBDDRAIN` probe worked and produced clean data. **It does not answer #808, because nobody
typed during the window the log covers.** Recorded so the next run is set up to answer it.

### What the probe says

```
KBDDRAIN: calls=106828 | empty=106828 floating=0 data_ff=0 aux=0 | host st=0x14 data=0x00
          nocrl=0 | last push never
```

Every one of 106,828 drain calls took the `empty` exit -- OBF clear, nothing waiting. Zero
`floating`, zero `data_ff`, zero `aux`, and **`nocrl=0`, so the sticky `g_kbd_no_controller`
latch never fired.** `st=0x14` is bits 2 (SYS) and 4, with bit 0 (OBF) clear.

### Why that answers nothing yet

```
KBDIRQ: isr_entries=0 (+0 since last) eois=0 | consumed=0 chords=0 poll_pushed=0
        poll_reads=106828
```

**`consumed=0`**: hype received not one scancode from any source in the whole logged window. No
USB keyboard either (`host-hid: no USB boot keyboard on any controller`). So `empty=100%` is
simply the correct reading for a keyboard nobody was using -- it is what a healthy idle drain
looks like, not a fault.

The operator *did* type later (they ran `flush`, which answered), but the log stops at stamp
162,245 and the sink never recovered after that, so the samples covering the typing are in the
lost tail. `isr_entries=0` throughout is worth keeping: IRQ1 does not fire on this laptop, which
is #218's whole premise, so input here depends entirely on the poll.

**Run 7 must have the operator typing early and repeatedly**, so `KBDDRAIN` brackets both a
working keystroke and a lost one. Waiting until something looks wrong is too late -- by then the
sink is gone and the evidence with it.

### #809's mechanism is identified, and hype already reports it

```
usb-log: FLUSH FAILED -- retrying each interval; \HYPE.LOG is INCOMPLETE until a retry succeeds.
         hype itself is unaffected; this is the USB block path (xHCI/MSC).
usb-log: flush RECOVERED after 6 failed interval(s) -- \HYPE.LOG is growing again
         (a gap may precede this line).
```

That is the ~4 KB gap, named by hype itself, with "a gap may precede this line" written into the
message. It is a **reported** condition, not a silent corruption -- so #809's remaining hole in
bootAMDL0-3 is very likely this same failure, and the question becomes why the USB block path
fails at all rather than whether the sink loses data.

Supporting counters: `bsp_usb_timeouts=7`, `USBFLUSH ... max=162427us` (a 162 ms slice against a
10 ms budget), `stalled=2` of 9 bursts, `behind=1905B` at the last sample.

Then it failed again and did not recover: the operator saw the log 100 KB behind and typing
`flush` reported **`0 byte(s) written`** -- correct, and the honest answer a bare "done" would
have hidden. #806's verb did its job on its first real use, just not with the number anyone wanted.

## Run 7 -- the same boot, with the operator typing from the start

1. Cold-boot. **Start typing on the built-in keyboard within the first 30 seconds** and keep
   going every 10-20 s for the whole run, whether or not anything appears. `KBDDRAIN` prints
   every ~10 s and needs samples from before, during and after the moment input dies.
2. Let the script drive the first boot to login and issue its reboot; the restart stalling is
   #803 and expected.
3. If input dies, **note the wall-clock moment** -- that is the one thing the log cannot
   reconstruct.
4. Type `flush` while it still works, then `host off`.

| Read | Answers |
| --- | --- |
| `KBDDRAIN empty=` vs `floating=`/`data_ff=`/`aux=`, across a sample where `consumed=` stops rising | #808: below hype, or hype discarding |
| `nocrl=` | #808: the sticky-latch suspect, live or dead |
| `usb-log: FLUSH FAILED` / `RECOVERED after N failed interval(s)` | #809: how often, and whether a gap follows |
| the `flush` byte count while input still works | #806: its first non-zero exercise |


## Result -- run 7, 2026-09-04, build `d51ceaf-dirty` (logs in `logs/bootAMDL0-7/`)

Operator typed from early and kept typing. **#808 now has a mechanism, and it is not the drain.**
Input never died this run, which is itself the finding.

### The measurement

`KBDDRAIN` and `KBDIRQ` paired across the run:

| `KBDDRAIN` | `isr_entries` | consumed | `ps2polled` |
| --- | --- | --- | --- |
| `calls=97185 empty=97185 floating=0 data_ff=0 aux=0 nocrl=0 st=0x14 data=0xe0` | rising | 146 | -- |
| `calls=217277 empty=217277 floating=0 data_ff=0 aux=0 nocrl=0 st=0x14 data=0xe0` | 509 (+31) | 374 | 27 |
| `calls=330553 empty=330553 floating=0 data_ff=0 aux=0 nocrl=0 st=0x14 data=0x9c` | **578 (+41)** | **612** | **34** |

Consumed rose monotonically 0 -> 612 for the whole run, so input worked start to finish.
`floating`, `data_ff` and `aux` stayed at **zero** throughout and `nocrl=0` -- hype never
discarded a byte and the sticky latch never fired. `data=` cycles through real Set-1 codes
(0x1c Enter, 0x1e A, 0x1f S, 0x20 D, 0x9c/0x9e breaks), so the drain does reach port 0x60.

### What it means

**IRQ1 fires on this laptop, and it carries essentially all the input: 578 ISR entries against
`ps2polled=34`.** The polled fallback contributed 5% of 612 scancodes.

That inverts #218's premise. The poll was added because "on the operator's laptop IRQ1 never
fires -- a full real-hardware run measured isr_entries=0 eois=0". On this machine, in this run, it
fires 578 times and the poll is a trickle.

And it re-reads run 5 exactly:

| run | `isr_entries` (last) | `ps2polled` | input |
| --- | --- | --- | --- |
| bootAMDL0-5 | **107, (+0 since last)** | **5** | died twice, dead at the end |
| bootAMDL0-7 | 578, (+41 since last) | 34 | alive throughout |

In run 5 the ISR **stopped** -- frozen at 107 with `+0` per sample -- and the poll delivered 5
bytes in the entire run. **So the polled fallback does not work as a fallback.** It cannot carry
the load when IRQ1 stops, which is the one job it exists for.

Why it cannot is the open question, and the drain has now answered for itself: it takes the
`empty` exit on ~100% of calls, discards nothing, and is not latched off. It sees OBF clear
because in the healthy case the ISR got there first -- but in run 5 the ISR was not running, and
`ps2polled` still stayed at 5.

**So #808's next probe belongs on IRQ1 delivery, not on the drain.** The question is why the
interrupt stops, and separately why an unconsumed byte does not then sit in the output buffer for
the 4 kHz poll to find.

### Also this run

- **No input death**, so the fault is intermittent. Runs 5 and 7 differ in outcome with the same
  config and build family.
- The restart chain fired again (`vm0 restarted (M8-4)`) and, as in every run since #803 was
  filed, the second boot did not reach a second login. No `SCRIPT vm0: PASS`.
- **#809: two more flush failures**, both self-reported and both recovered --
  `flush RECOVERED after 1 failed interval(s)` and `after 4 failed interval(s)`, each carrying
  "a gap may precede this line". `bsp_usb_timeouts` climbed to 16.
- **New: the USB buffer pool hit its ceiling.** `usb_waiters_max=1 usb_held=64/64` on the final
  `FBINFLIGHT` line, against `0/64` for the rest of the run. Worth carrying to #809 -- an
  exhausted pool is a plausible reason a flush interval fails.
- Ended on the power button again, so the tail is short by whatever was outstanding.

## Run 8 -- the starvation probe (`f0eba66`)

Run 7 settled what the drain is *not*, so the second #808 probe measures whether the input path
is being **starved** rather than broken. The i8042 buffers one byte, so any window with neither
an ISR nor a drain loses every keystroke after the first.

`KBDDRAIN` gains three fields:

- **`gap_max=`** -- longest interval between two consecutive drain calls. Tens of microseconds is
  the #796 4 kHz gate working; hundreds of milliseconds is the answer.
- **`isr_empty=`** -- interrupts that fired with nothing waiting.
- **`pic_imr=` / `irq1=`** -- the master PIC mask. `MASKED` would be a different fault wanting the
  opposite fix.

**QEMU already shows `gap_max=128932us`** on an idle sandbox with nobody typing -- 129 ms, or 516x
the 250 us gate interval, with `irq1=unmasked`. If the hardware number is anything like that, the
poll cannot be a fallback for anything. Treat that as a hypothesis the run tests, not a finding:
it came out of a check that was only meant to validate a printf.

### The sequence

Same as run 7 -- **type from the first 30 seconds and keep tapping every 10-20 s**, watch the
echo, and note the wall-clock moment if it stops. Type `flush` while input still works, then
`host off`.

| Read | Answers |
| --- | --- |
| `gap_max=` | whether the input path is blind for long enough to lose keystrokes |
| `irq1=` | whether IRQ1 is masked (a different fault) or starved |
| `isr_empty=` vs `isr_entries` rising | whether interrupts fire and find the byte already gone |
| `consumed=` flatlining while you tap | the death moment, against `gap_max` at that sample |
| `usb-log: FLUSH FAILED` / `usb_held=` | #809's pool-exhaustion lead |
| the `flush` byte count while input works | #806's first non-zero exercise |

Input death is **intermittent** -- runs 5 and 7 had the same config and opposite outcomes -- so a
run that keeps input is not evidence of a fix, and `gap_max` is worth reading either way.
