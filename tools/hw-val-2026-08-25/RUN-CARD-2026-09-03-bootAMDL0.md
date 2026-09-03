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
3. After five minutes, power off.
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
