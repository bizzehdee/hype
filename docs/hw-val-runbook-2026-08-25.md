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

---

## Run 1 — AMD laptop (3 usable physical cores after BSP reservation)

### Boot 1a: one long-lived 2-vCPU Alpine session, three signals at once
Bring up a genuine 2-vCPU (non-SMT) Alpine guest and, in one continuous
session: let it idle to gather HLT-rate data, run the REP INSB/OUTSB string-IO
probe, then pin the reboot to the non-BSP vCPU.

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

### Boot 1b: `tests/micro/suite-603.cfg` — blocked on a sizing decision
**#603**'s AMD leg needs 4 physical cores (`vmexit`=2, `vmexitstorm`=1,
`hello`=1); this laptop has 3 usable after BSP reservation. Decide before this
boot: trim the suite to fit 3, or borrow a bigger AMD box. Not resolvable by
running harder.

### Boot 1c: exFAT on-medium self-test battery
**#653** — `tools/exfat-e2e` (the underlying battery, #692, is already
complete). Needs both vendors; this is the AMD half. Quick, self-contained —
tack onto the end of this session rather than scheduling separately.

---

## Run 2 — Intel laptop (i5-13420H) — the VMX/APICv cluster

Nine tickets share one root cause area (AP-vCPU interrupt/timer delivery
under VMX) or one physical setup. Reuse the **same already-boot-verified
2-vCPU (2 physical cores, non-SMT) Alpine disk image** across as many of these
as possible rather than re-imaging between them.

### Boot 2a: DEFAULT build, fresh boot, no restart
This is **#708's own open question**, asked first because #605 cannot proceed
without the answer: does a genuine 2-vCPU non-SMT Linux guest's AP ever get a
timer interrupt on the DEFAULT (non-APICv) build, on a fresh boot with no
restart involved at all? Every prior default-build test used either 1 vCPU
with an SMT-reported second logical CPU, or the microtest suite — never this
exact shape.
- Hangs the same way APICv did (BSP freezes ~253 deliveries, AP zero) -> a
  pre-existing general VMX AP-timer defect, broader than APICv (#708).
- Boots clean to login -> squarely an APICv-introduced regression, and this
  same clean boot is also the **#637** repro window (Alpine login shell dying
  between motd and first prompt) — check for that right here before moving on.

### Boot 2b: same image, same build, pin reboot to the non-BSP vCPU
**#525**'s VMX leg (SVM side already proven). Bar: reset served by the AP,
exactly once, clean restart to login. If 2a hung, this boot cannot proceed
meaningfully — resolve 2a's finding first.

### Boot 2c: APICv build (`-DHYPE_ENABLE_APICV=1`), same image, fresh boot
**#599**'s own bar (2-vCPU login with the flag on) and the direct follow-up to
**#708**/**#605**. Compare directly against 2a:
- Both hang the same way -> #708 closes as "pre-existing, not APICv-specific";
  #599/#605 can re-evaluate once the underlying defect is fixed.
- Only this one hangs -> APICv regression confirmed; worth checking the
  `lvt=0x30005` lead #708 already flagged (bits [17:16] = `0b11`, architecturally
  reserved for the classic LVT timer — possibly TSC-deadline mode, which Linux
  defaults to and which hype's timer model may not arm under APICv).
- Neither hangs -> #599 bar met; #605 can consider recommending the default
  flip.

### Boot 2d: `tests/micro/suite-603.cfg` (default build, this is Intel's first-ever run of it)
Three tickets from one boot:
- **#603** — Intel/VMX leg (this environment has never had VMX at all before now).
- **#604** — Intel leg of the NX/W^X hardening pass (SMP=8, four AP cores
  reaching `long-mode=yes` under NX, `tools/643` two-boot passes).
- **#712** — Intel leg of the REP INSB/OUTSB fix (the same `probe ok` line
  Run 1's AMD boot checks, this time on VMX).

### Boot 2e: FreeBSD image, instrumented timer-calibration sampling
**#577** — a different guest entirely, so its own boot. Per the ticket's own
next step: sample how many real-world ms elapse per `hype_guest_lapic_advance()`
call while the guest sits in its LAPIC calibration loop at `lapic_et_start`,
to confirm or refute the rate/cadence theory.

### Boot 2f: exFAT self-test battery (Intel half of #653)
Same as Run 1's tack-on, the other vendor. Quick, tack onto the end of this session.

---

## Run 3 — physical storage cluster (either laptop; vendor-agnostic)

### Boot 3a: two serialized USB sticks on one root hub
**#387** + **#388** together, exactly as #387's own comment already scopes
it: stage the standard stick as the boot medium, insert a second serialized
stick, set `media_disk = <second's serial>` on a VM, assert the same four log
lines #387's QEMU run already produced. #388 rides the same boot (its own bar
is the `physical:` target write landing only on the named stick).

### Boot 3b: one USB-SATA drive, ALL the partitions at once
**#688** + **#689** name the same drive class (a USB-SATA stick) with two
different data-partition filesystems. Nothing requires them on separate
sticks or separate boots — partition it once as ESP (FAT32) + an ext4 data
partition + an NTFS data partition, and check both tickets' bars from the
same cold boot instead of two.

### Boot 3c: physical NVMe + physical AHCI, concurrent two-VM write load
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

---

## Run 4 — Windows session (either laptop; needs `ahci-sata`, Windows' own `os_hint` default)

### Boot 4a: fresh Windows install
`SECS=300 tools/436/run-win.sh` at HEAD — #436 (the upstream install-media
blocker) is now closed, so this is the cheap first test of whether the path
is clear at all.
- Reaches OOBE -> pull **#442**'s own next-step diagnostics right here
  (`C:\Windows\Panther\UnattendGC`, `%ProgramData%\Microsoft\Diagnosis`), and
  re-test the `appxsvc` start timeout.
- Succeeds -> **archive the resulting disk image immediately.** This single
  artifact IS **#695**'s own deliverable (a reusable installed Windows guest
  image), which is the one thing blocking the next boot.

### Boot 4b: tri-OS concurrent run, using the image Boot 4a just produced
Three tickets collapse into this one boot, since #635/#636 both just read off
the same running session #634 establishes — no extra boot needed for them:
- **#634** — Windows + Linux + BSD concurrently, console switching.
- **#635** — isolation/pinning proof under that same tri-OS load.
- **#636** — dashboard stats cross-checked against config and guest-visible
  reality, observed during the same run.

If Boot 4a doesn't reach a stable install, 4b/4c/4d simply don't happen
tonight — there is nothing to test them against, per #634/#635/#636's own
stated blocker.

---

## Summary

| run | machine | boots | tickets closed (or advanced) |
|---|---|---|---|
| 1 | AMD laptop | 3 | #641, #712 (AMD), #653 (AMD half), #603 (AMD — blocked on a sizing decision), *(#698, not On Hold)* |
| 2 | Intel laptop (i5-13420H) | 6 | #708, #525, #599, #605, #637, #603 (Intel), #604 (Intel), #712 (Intel), #577, #653 (Intel half) |
| 3 | either | 3 | #387, #388, #688, #689, #715, #660, #713, #426 |
| 4 | either | up to 2 | #442, #695, #634, #635, #636 |

**14 physical boots** (14th being Run 4's conditional second boot) cover
**22 of the 27** On Hold tickets. The remaining 5 (#232, #399, #400, #709,
#692) are excluded above with a one-line reason each — none of them need a
boot tonight, they need a code fix, unbuilt driver work, a human decision, or
a board-status correction respectively.
