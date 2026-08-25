# Combined hardware-validation runbook — clearing the On Hold column (2026-08-25)

27 tickets are On Hold as of this pass. Grouped below into the smallest number
of physical boot sessions that still discharges every ticket's own bar —
several tickets watch different signals from the exact same boot, and several
more explicitly say "rides the next hardware run alongside" another ticket
already. Safety rules (targets by serial only, standing exclusions) as
`docs/hw-val-211.md`.

**Not part of tonight's run** — hardware access would not help these:

| ticket | why excluded |
|---|---|
| #232 | blocked by **#727**, a hype code bug (`cdroms=` beyond the first slot is never attached) — needs a fix merged first, not a boot |
| #399, #400 | blocked on unbuilt HNET-D* NIC driver work — nothing to exercise yet |
| #709 | blocked on a **human decision** (link `fs_battery.c` into the shipped binary, or duplicate its logic firmware-side) — decide first, the run is cheap after |
| #692 | its own last comment says the ticket is complete — housekeeping only: move to Done, not a run |

## Build changes needed

Checked every fix's own commit against `main` before writing this: **every
landed fix referenced below (#641, #637, #712, #660, #715, #604's item 1,
#698's `TIMERSTALL` probe, #577's HPET fix) is already on `main`.** One
default build covers **12 of the 14 boots** below with configuration-only
differences (`hype.cfg`, which VM/guest image, which physical device by
serial). Only two boots are exceptions:

| boot | change | why unavoidable |
|---|---|---|
| **2c** | `-DHYPE_ENABLE_APICV=1` at compile time | #599/#605/#708's whole comparison IS "does the same guest behave differently with the flag on vs. off" — a compile-time-gated feature can't be A/B'd from one binary, by construction. This is not a build change to try to minimize away. |
| **2e** (possible) | new instrumentation for `hype_guest_lapic_advance()` call cadence, if needed | #577's existing diagnostic (RT-2c `KRIPHIST`) was already found "empty during the hang" per the ticket's own Aug 21 comment — the HPET-layer fix (2d2e40f) landed and is confirmed working, but this SECOND, deeper layer (LAPIC calibration itself) still needs real measurement the current build may not produce. **Decide before Run 2**: either confirm an existing diagnostic already gives enough resolution (then this boot is config-only too), or write the sampling instrumentation ahead of time and fold it into the SAME extra build as the APICv flag below — not a third build, and not a rebuild mid-session. |

**Practical shape for tonight: bring exactly two `hype.efi` builds, built once,
ahead of time** — the default build (used everywhere except 2c), and one
extra build carrying `-DHYPE_ENABLE_APICV=1` (and #577's sampling
instrumentation too, if that turns out to be needed, since it can ride the
same rebuild rather than forcing a separate one). Every other difference
between boots below is `hype.cfg` alone: `vcpus`, `os_hint`, `disks=`/`cdroms=`
by serial, and which guest image/ISO is attached.

---

## Run 1 — AMD dev machine (5950X, 16 cores) — not the AMD laptop

Moved off the AMD laptop (3 usable physical cores after BSP reservation) onto
this 5950X box, which removes Boot 1b's core-count blocker entirely (16 cores
is well past the 4 `suite-603.cfg` needs).

**Log-rotation workflow note:** run all three boots below back-to-back first,
then ingest all three logs together in one pass at the end — not
incrementally after each boot. Reviewing mid-sequence competes with log
rotation for the same window and risks losing the earlier boots' evidence
before it's read.

### Boot 1a: one long-lived 2-vCPU Alpine session, three signals at once — min 30 min [build: default]
Bring up a genuine 2-vCPU (non-SMT) Alpine guest and, in one continuous
session: let it idle to gather HLT-rate data, run the REP INSB/OUTSB string-IO
probe, then pin the reboot to the non-BSP vCPU. 30 min floor because #641's
own numeric thresholds were originally measured over a 26-minute idle window
— shorter risks an inconclusive HLT-rate/TMRLATE sample, not just a smaller one.

- **#641** — the outstanding numeric thresholds (HLT/s rate, TMRLATE under
  load) that the correctness fix (already landed) still needs confirming on
  real hardware.
- **#712** — Intel leg is separate (Run 2); the AMD leg's own bar
  ("`probe ok` — register IN/OUT and REP OUTSB/INSB both intercepted") was
  already exercised via `suite-603.cfg` at SMP=8 under this session's own
  KVM/SVM — worth one real bare-metal repeat here for a belt-and-suspenders
  confirmation, but not a new investigation.
- **(not On Hold, but rides this exact boot)** **#698** — the AP-vCPU
  LAPIC-timer-stuck-in-service freeze after a non-BSP 0xCF9 reboot. Every
  QEMU/nested-SVM attempt this session failed to reproduce it; this is the
  first real chance. The `TIMERSTALL` diagnostic (commit `d2bada3`, emitted at
  WARN so it survives any `log_level`) is already in the build — just watch
  the log for a `fw-1 TIMERSTALL` line after the reboot completes.

### Boot 1b: `tests/micro/suite-603.cfg` — min 10 min [build: default]
**#603**'s AMD leg needs 4 physical cores (`vmexit`=2, `vmexitstorm`=1,
`hello`=1) — no longer a blocker on the 5950X (16 cores). Run as originally
scoped (`SMP=8` or higher, well inside budget), no trimming needed. 10 min
floor: `tools/micro/run-micro.sh`'s own default is `SECS=90` per microtest
plus a 150-180s outer buffer, and this suite runs three VMs (`hello`,
`vmexit`, `vmexitstorm`) — real hardware boot overhead across all three
comfortably fits inside 10 min, but budget the full window rather than
cutting it the moment `hello` passes.

### Boot 1c: exFAT on-medium self-test battery — min 10 min [build: default]
**#653** — `tools/exfat-e2e` (the underlying battery, #692, is already
complete). Needs both vendors; this is the AMD half. Quick, self-contained —
tack onto the end of this session rather than scheduling separately. 10 min
floor matches the bounded on-medium batteries elsewhere in this repo
(`fat32_selftest`'s own shape) — a pass/fail battery, not an open-ended session.

### After all three boots: ingest logs together
Pull `\HYPEFULL.LOG` and each boot's per-VM logs from all three boots in one
pass. Cross-check in this order: #698's `TIMERSTALL` line (1a), the
`suite-603.cfg` verdict lines for `hello`/`vmexit`/`vmexitstorm` (1b), then
the exFAT battery's own pass/fail summary (1c) — same order the boots ran in,
so a truncated or rotated-out early section is easy to notice against what
should be there.

---

## Run 2 — Intel laptop (i5-13420H) — the VMX/APICv cluster

Nine tickets share one root cause area (AP-vCPU interrupt/timer delivery
under VMX) or one physical setup. Reuse the **same live Alpine ISO** across as
many of these as possible rather than re-imaging between them.

> **Correction (2026-08-25, after Boot 2a's first attempt).** "2-vCPU non-SMT"
> is not something a config can ask for. `vcpus` counts PHYSICAL CORES and the
> guest is given `vcpus x threads_per_core` logical CPUs — the spec calls SMT
> "a bonus it was never promised", and `isolation_group` allocates whole cores
> rather than disabling SMT. So `vcpus = 2` on this Intel box produced **4**
> logical CPUs (log: `granted 2 whole physical core(s) -> 4 logical CPU(s),
> 2 thread(s)/core (SMT bonus)`, APIC IDs 8/9/16/17 — two sibling pairs).
>
> That is still informative for #708 — there IS a genuine second physical
> core's AP in play — but it is not the clean read the "non-SMT" wording
> promised: if it hangs, an SMT sibling cannot be ruled out as the confound.
> To get the clean shape without a build change, either disable HT in the host
> BIOS for 2a/2b/2c (at the cost of comparability with Run 1's AMD boots), or
> set `host_cpu_budget` to this CPU's E-cores, which have no SMT, and confirm
> from the log that logical CPUs == cores granted.

### Boot 2a: DEFAULT build, fresh boot, no restart — min 20 min [build: default]
This is **#708's own open question**, asked first because #605 cannot proceed
without the answer: does a genuine 2-vCPU non-SMT Linux guest's AP ever get a
timer interrupt on the DEFAULT (non-APICv) build, on a fresh boot with no
restart involved at all? Every prior default-build test used either 1 vCPU
with an SMT-reported second logical CPU, or the microtest suite — never this
exact shape. 20 min floor: #708's own APICv run stayed hung long enough for
exit counts to climb from 156M to 205M+ with zero deliveries the whole time —
a short window can't distinguish "genuinely stuck" from "just slow," and the
whole point of this boot is telling those apart.
- Hangs the same way APICv did (BSP freezes ~253 deliveries, AP zero) -> a
  pre-existing general VMX AP-timer defect, broader than APICv (#708).
- Boots clean to login -> squarely an APICv-introduced regression, and this
  same clean boot is also the **#637** repro window (Alpine login shell dying
  between motd and first prompt) — check for that right here before moving on.

> **RESULT (2026-08-25): Boot 2a PASSED.** Logs archived at
> `tools/hw-val-2026-08-25/logs/2a/`.
> ```
> fw-1 SCRIPT vm0: PASS pass (8 directive(s), 56523ms)
>   at line 13: fresh-boot-login
> ```
> Guest reached `localhost login:` at t=155s and idled to ~900s with **zero
> `TIMERSTALL`** lines; `apicv=off` throughout, 2 cores -> 4 logical CPUs.
> Per this section's own decision tree that is the "boots clean to login"
> branch: **#708 is an APICv-introduced regression, not a general VMX AP-timer
> defect** — Boot 2c is now the confirming test, not an open question.
> **#637 did not reproduce**: motd, prompt and the echoed command all came
> through intact. The `#94 RTCRATE delivered=0/5s programmed=1024Hz` lines are
> a non-finding — `regB=0x02` has the PIE bit clear, so the guest never asked
> for periodic interrupts.
>
> Two harness facts this boot settled, both of which would have cost a full
> 30-minute timeout on 1a/2b (see `input-scripts/reboot-pin.txt`'s header):
> kernel messages never reach the serial console on this live-ISO shape (the
> log jumps straight from GRUB to the agetty banner), and `vcpus` is physical
> cores, so no `Brought up N CPUs` directive can work here on either count.

### Boot 2b: same image, same build, pin reboot to the non-BSP vCPU — min 20 min [build: default]
**#525**'s VMX leg (SVM side already proven). Bar: reset served by the AP,
exactly once, clean restart to login. If 2a hung, this boot cannot proceed
meaningfully — resolve 2a's finding first. 20 min floor: the SVM leg's own
bare-metal run took ~12 min end to end (login, pin, reboot, second login);
budget extra on the VMX leg's first-ever real-hardware attempt rather than
cutting it at the SVM timing.

> **RESULT (2026-08-25): Boot 2b FAILED — a real finding, logged on #525.**
> Logs at `tools/hw-val-2026-08-25/logs/2b/`. The guest reached Linux's
> `Requesting system reboot` with `/sys/kernel/reboot/cpu = 1` confirmed
> (`CPU-PINNED-1`) and wedged there. **`0xCF9` never appears in the log** — the
> reset register was never written.
> ```
> ATAPIOPS vm0: ... cmds=2297   at t=481s, 492s, 497s -- frozen, identical
> APLOOP vm0: iter=439319 -> 443019, reason=0xc, grip=0xffffffffb2c81c2e (const)
> ```
> ATAPI idle rules out Alpine live-mode's modloop teardown blocking on the
> streamed ISO (the `ERROR: modloop failed to stop` on screen is the cosmetic
> warning, not the cause). The vCPU is spinning on HLT (VMX exit reason 12) at
> a fixed kernel RIP, which places the hang in `machine_shutdown()` /
> `smp_send_stop()`: the reboot work was handed to the AP and the handoff never
> completed. Same AP interrupt-delivery family as #698, one layer earlier.
>
> **The first attempt at this boot reported a false PASS** 124ms after `reboot`
> was sent, on a guest that had not restarted — hype's screen-scan matcher
> (#302) satisfying `expect localhost login:` from the previous boot's banner
> still on screen. Filed as **#728**. `reboot-pin.txt` is now hardened against
> it (screen cleared before reboot; tmpfs marker armed as a `fail-if` so a
> stale shell fails loudly rather than passing). Any prior PASS from a
> restart-class script anywhere in this repo is suspect for the same reason.

### Boot 2c: APICv build (`-DHYPE_ENABLE_APICV=1`), same image, fresh boot — min 20 min [build: APICv extra build]
**#599**'s own bar (2-vCPU login with the flag on) and the direct follow-up to
**#708**/**#605**. Compare directly against 2a — same 20 min floor and the
same reasoning: this is exactly the boot #708 already ran once and found
hung, so anything shorter than that prior run's own observation window risks
declaring "fixed" on a guest that was simply still climbing toward the same wall.
- Both hang the same way -> #708 closes as "pre-existing, not APICv-specific";
  #599/#605 can re-evaluate once the underlying defect is fixed.
- Only this one hangs -> APICv regression confirmed; worth checking the
  `lvt=0x30005` lead #708 already flagged (bits [17:16] = `0b11`, architecturally
  reserved for the classic LVT timer — possibly TSC-deadline mode, which Linux
  defaults to and which hype's timer model may not arm under APICv).
- Neither hangs -> #599 bar met; #605 can consider recommending the default
  flip.

> **RESULT (2026-08-25): Boot 2c HUNG — "only this one hangs" branch, so the
> APICv regression is confirmed.** Logs at `tools/hw-val-2026-08-25/logs/2c/`.
> Same config and same tree as 2a (`976e71a-dirty` both), only the build flag
> differing; `apicv=ON` verified in the log before trusting the result.
>
> | | 2a (default) | 2c (APICv) |
> |---|---|---|
> | GRUB → kernel | t=118s | t=172s |
> | login prompt | **t=155s** | **never** |
> | `TIMERSTALL` | 0 over ~900s | 1, at t=216s |
>
> ```
> TIMERSTALL vm0/0 [#698]: latched after 5.0s stuck -- eoi_count=273
>     pending_valid=0 pending_vector=0x0 rflags_if=1 vintr_armed=0x0
> vmx apicv-state: gis=0x30ec  virr: [1]=0x10000 [7]=0x1000  isr: [1]=0x10000
>     -- frozen identically at t=264s, t=336s, t=406s
> ```
> RVI=0xec / SVI=0x30; vector 0x30 stuck in-service and never EOI'd (last
> reason-45 exit t=191s); the timer vector 0xec pending in vIRR. **hype's own
> model disagrees with the page** — it reports nothing pending while the page
> holds 0xec — and the vCPU sits in a HLT loop at a fixed RIP with the guest
> polling TMCCT (`apicv-acc: qual=0x390`). Detail and suggested direction on
> #599; #708 updated.
>
> **This section's `lvt=0x30005` lead does not hold up.** Bit 16 is the LVT mask
> bit and bit 17 is periodic mode, so that is a *masked periodic* LVT, not
> TSC-deadline (bit 18). It is the AP's (`timer_irqs=0`, `init=0xFFFFFFFF`),
> which never finished configuring its timer — a consequence of the BSP stall.
> The BSP's LVT is healthy throughout: `lvt=0x200ec(periodic) init=207887` with
> `cur` still cycling.
>
> Per this section's own decision tree, the outcome is "**only this one hangs**":
> #599's bar is not met, and #605 must not recommend flipping the default.
>
> **Before 2d: copy `hype-default.efi` back over `EFI\BOOT\BOOTX64.EFI`.**

### Boot 2d: `tests/micro/suite-603.cfg` (default build, this is Intel's first-ever run of it) — min 10 min [build: default]
Same floor and reasoning as Boot 1b. Three tickets from one boot:
- **#603** — Intel/VMX leg (this environment has never had VMX at all before now).
- **#604** — Intel leg of the NX/W^X hardening pass (SMP=8, four AP cores
  reaching `long-mode=yes` under NX, `tools/643` two-boot passes).
- **#712** — Intel leg of the REP INSB/OUTSB fix (the same `probe ok` line
  Run 1's AMD boot checks, this time on VMX).

> **RESULT (2026-08-25): Boot 2d — #604 and #712 PASS, #603 one probe short.**
> Logs at `tools/hw-val-2026-08-25/logs/2d/`.
>
> - **#712 PASS** — `probe ok -- register IN/OUT and REP OUTSB/INSB (string
>   I/O) were all intercepted and serviced`.
> - **#604 PASS** — `EFER.NXE enabled on the BSP`, `NX applied to every host
>   page except hype's own image`, and **7** AP smoketests across 3 VMs all
>   `long-mode=yes ap_vmm_ok=1` (the ticket asked for four).
> - **#603 — one failure**, everything else green:
>   ```
>   MSR round-trip (MTRR var0 base) wrote 0x123456000, read back 0x0
>   PROBE FAIL -- a modelled MSR did not round-trip
>   ```
>   Filed as **#729**. `arch/x86_64/vmx/vmcs_hw.c:1925` already says the
>   pvclock/MTRR/PAT special-casing is omitted on VMX and that "a full guest OS
>   on VMX would need those ported too" — deferred work that was never
>   ticketed. Isolation results from the same boot are all good:
>   `WATCHDOG vm1: faulted: unhandled-exit storm at one RIP (reason=0x30
>   rip=0x100055f, 4096 repeats) -- forcing THIS vm off; others keep running`,
>   the paired healthy VM still reached `MICRO PASS: hello`, and vmexit's
>   deliberate triple-fault took only itself down. INIT/SIPI, GLADDER-1 absorb
>   and the seven SVM-instruction #UD probes all passed.
>
> **This changes Run 4's machine choice — see that section.**

### Boot 2e: FreeBSD image, instrumented timer-calibration sampling — min 15 min [build: default, or the extra build if new sampling instrumentation is needed]
**#577** — a different guest entirely, so its own boot. Per the ticket's own
next step: sample how many real-world ms elapse per `hype_guest_lapic_advance()`
call while the guest sits in its LAPIC calibration loop at `lapic_et_start`,
to confirm or refute the rate/cadence theory. 15 min floor: the ticket's own
title says this guest HANGS at calibration, so the boot needs enough sustained
samples post-hang to characterize the cadence, not just confirm the hang exists.

### Boot 2f: exFAT self-test battery (Intel half of #653) — min 10 min [build: default]
Same as Run 1's tack-on, the other vendor. Quick, tack onto the end of this session.

---

## Run 3 — physical storage cluster (either laptop; vendor-agnostic)

### Boot 3a: two serialized USB sticks on one root hub — min 15 min [build: default]
**#387** + **#388** together, exactly as #387's own comment already scopes
it: stage the standard stick as the boot medium, insert a second serialized
stick, set `media_disk = <second's serial>` on a VM, assert the same four log
lines #387's QEMU run already produced. #388 rides the same boot (its own bar
is the `physical:` target write landing only on the named stick). 15 min
floor: `tools/387/run-387.sh`'s own QEMU default is 300s (5 min) for the
whole scripted install+assert sequence; real USB hardware enumeration and a
real installer boot both run slower than QEMU's emulated xHCI, so budget 3x.

### Boot 3b: one USB-SATA drive, ALL the partitions at once — min 25 min [build: default]
**#688** + **#689** name the same drive class (a USB-SATA stick) with two
different data-partition filesystems. Nothing requires them on separate
sticks or separate boots — partition it once as ESP (FAT32) + an ext4 data
partition + an NTFS data partition, and check both tickets' bars from the
same cold boot instead of two. 25 min floor: each format's own HW-VAL pass
(write past EOF, verify gaps, fsck clean — see `docs/hw-val-386.md`'s
per-format shape) takes real minutes on a physical USB-SATA bridge; doing
both formats' passes in one boot means budgeting for both in sequence, not
just the slower of the two.

### Boot 3c: physical NVMe + physical AHCI, concurrent two-VM write load — min 45 min [build: default]
Four tickets, one extended session:
- **#715** — NVMe vectored-write path, on the actual hwstick this was found
  on (serial `5ME3N005713803V2W`).
- **#660** — criterion 2, the live two-VM concurrent re-read-every-range
  verification (criteria 1+3 already fixed and committed; this is the last
  piece).
- **#713** — the dashboard/BSP stall repro (up to 46s) during concurrent
  physical AHCI+NVMe writes — needs real disks because the sandbox's virtual
  backends are memory-speed and cannot reproduce the symptom at all.
- **#426** — the outstanding real-hardware HW-VAL gate for the AHCI/NVMe/xHCI
  ring migration. QEMU-proven already; this concurrent AHCI+NVMe session is
  exactly the kind of load that gate wants, so let it ride rather than
  scheduling a separate pass.

45 min floor: `tools/295`/`tools/715`'s own per-attempt QEMU timeouts are 420s
(7 min) each for a SINGLE-target, single-VM write check. This boot needs BOTH
targets under CONCURRENT two-VM load long enough to (a) let #713's stall
pattern actually recur — its own ticket describes a "~6-7s baseline" that
"persists after" an initial 46s stall, meaning it needs to be observed
repeating, not just once — and (b) give #660's criterion 2 enough transferred
ranges to make "verified against every range" a real claim rather than a
lucky sample.

---

## Run 4 — Windows session (either laptop; needs `ahci-sata`, Windows' own `os_hint` default)

> **Run this on the AMD 5950X, not the Intel box (added 2026-08-25).** Boot 2d
> found that VMX does not model the MTRR MSRs (**#729**): writes drop, reads
> return 0, while MTRRcap still advertises variable MTRRs. Per #436's own
> notes, OVMF's MtrrLib is invoked by **Windows winload's SetMemoryAttributes**
> — so a Windows install on Intel is expected to fail the same way AMD did
> before #436. Linux and BSD never call MtrrLib, which is why Boot 2a's Alpine
> guest booted clean on the same build. Until #729 lands, "either laptop" in
> the heading above does not hold for Windows.

### Boot 4a: fresh Windows install — min 45 min [build: default]
`SECS=300 tools/436/run-win.sh` at HEAD — #436 (the upstream install-media
blocker) is now closed, so this is the cheap first test of whether the path
is clear at all. 45 min floor: `tools/436`'s own `SECS=240` (4 min) default is
sized for one phase of a scripted QEMU install; a full Windows install to
OOBE on real hardware (real disk speed, no virtual-disk shortcuts) routinely
runs 30-45+ min even on a healthy machine, before OOBE/CloudExperienceHost
even starts.
- Reaches OOBE -> pull **#442**'s own next-step diagnostics right here
  (`C:\Windows\Panther\UnattendGC`, `%ProgramData%\Microsoft\Diagnosis`), and
  re-test the `appxsvc` start timeout.
- Succeeds -> **archive the resulting disk image immediately.** This single
  artifact IS **#695**'s own deliverable (a reusable installed Windows guest
  image), which is the one thing blocking the next boot.

### Boot 4b: tri-OS concurrent run, using the image Boot 4a just produced — min 30 min [build: default]
Three tickets collapse into this one boot, since #635/#636 both just read off
the same running session #634 establishes — no extra boot needed for them.
30 min floor: #635's isolation/pinning proof and #636's dashboard-stats
cross-check both need the tri-OS load to be genuinely SUSTAINED, not just
momentarily concurrent, or there's nothing meaningful to observe for either.
- **#634** — Windows + Linux + BSD concurrently, console switching.
- **#635** — isolation/pinning proof under that same tri-OS load.
- **#636** — dashboard stats cross-checked against config and guest-visible
  reality, observed during the same run.

If Boot 4a doesn't reach a stable install, 4b/4c/4d simply don't happen
tonight — there is nothing to test them against, per #634/#635/#636's own
stated blocker.

---

## Summary

| run | machine | boots | min run time | tickets closed (or advanced) |
|---|---|---|---|---|
| 1 | AMD dev machine (5950X) | 3 | 30 + 10 + 10 = **50 min** | #641, #712 (AMD), #653 (AMD half), #603 (AMD), *(#698, not On Hold)* |
| 2 | Intel laptop (i5-13420H) | 6 | 20 + 20 + 20 + 10 + 15 + 10 = **95 min** | #708, #525, #599, #605, #637, #603 (Intel), #604 (Intel), #712 (Intel), #577, #653 (Intel half) |
| 3 | either | 3 | 15 + 25 + 45 = **85 min** | #387, #388, #688, #689, #715, #660, #713, #426 |
| 4 | either | up to 2 | 45 + 30 = **75 min** | #442, #695, #634, #635, #636 |

**14 physical boots** (14th being Run 4's conditional second boot),
**~305 min (~5.1 hours) of minimum boot time** across all four runs, cover
**22 of the 27** On Hold tickets. That's a floor, not a schedule — it excludes
setup/teardown between boots, log ingestion (Run 1's own back-to-back note),
and any retry a failed or ambiguous boot needs. The remaining 5 (#232, #399,
#400, #709, #692) are excluded above with a one-line reason each — none of
them need a boot tonight, they need a code fix, unbuilt driver work, a human
decision, or a board-status correction respectively.

**Builds needed tonight: 1, possibly 2 — not 14.** 13 of 14 boots run the
same default `hype.efi` (current `main`, every referenced fix already
landed); only Boot 2c's `-DHYPE_ENABLE_APICV=1` comparison genuinely can't be
done from that same binary. See "Build changes needed" above for the one
open question (Boot 2e) that could make it 2 extra builds instead of 1 —
resolve that before Run 2 starts, not mid-session.
