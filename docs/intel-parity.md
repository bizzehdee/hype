# Intel (VT-x/VMX) → AMD parity: what is left

Status as of 2026-07-29, `8a476c5`.

> ## UNPARKED 2026-08-02 — hardware access restored
> The nested-VMX box is back, now at **`192.168.0.150`** (log in as `darren@`; `usbuntu`
> is the hostname, not the login). Re-verified reproducing on 2026-08-02: Intel/VMX
> selected, Linux 6.12.81 boots, the xstate.c:332 WARNING fires, guest still executing.
>
> `~/hype-bisect` on the box was empty, so the harness lives in `~/vmxrun` instead:
> `scp` hype.efi to `~/vmxrun/esp/EFI/BOOT/BOOTX64.EFI`, the guest firmware pair to
> `~/vmxrun/esp/EFI/hype/`, and the earlycon ISO to `~/vmxrun/esp/iso/test.iso`, then
> `RUNSECS=180 ./run.sh` (log: `/tmp/vmx.log`). **`run.sh` shipped with `-m 8192`, which
> does not fit this 7 GB box** — it is now 4096.
>
> Everything below still applies: the failures are only observable by running the guest.
> Do not delete the diagnostics added for this; they are what made the last five fixes
> findable.

## The bar (where AMD is)

Two Alpine guests on two dedicated cores, **both reaching a login prompt**, with
**isolated filesystems**, and the self-test battery green — all in **one boot**,
on **real AMD hardware**. That is the target Intel has to match.

## Where Intel actually is

The guest boots firmware -> GRUB -> the Linux kernel, and is **still running** when the
capture window closes. Last observed:

```
x86/fpu: Enabled xstate features 0x207, context size is 840 bytes
[    0.420422] ... kernel still executing
LAPIC timer IRQs=254, PIT IRQ0 IRQs=28      <- interrupts ARE being delivered
```

It has not reached userspace/login. The one open guest-side complaint is a
**WARNING** (not an oops) at `arch/x86/kernel/fpu/xstate.c:332`, which the kernel
runs past.

### Fixed today, in order found
| Commit | Root cause |
|---|---|
| `354e151` | Dumps fabricated guest state on VMX (SVM-only struct read) |
| `281186a` | CR-write probe + physical insn-byte fallback when `cr3=0` |
| `84e14b5` | Host must OWN CR4.VMXE / CR0.NE -- CR4 `#GP` 85 -> 0 |
| `3bb58ad` | STI window consumes the exit's interrupt -- storm 13.8M -> 0 |
| `ceae540` | acknowledge-interrupt-on-exit + `hype_isr_dispatch_vector()` |
| `f9bac5e` | **LOAD_IA32_EFER never requested** -- guest ran on the HOST's EFER |
| `96ff51b` | Guest FS/GS base never applied AND segments left "unusable" -- `#GP` 96 -> 0 |
| `9b760b6` | Guest introspection used an SVM-only CR3, blind on VMX |
| `c227ff5` | VM-entry/exit MSR areas (KERNEL_GS_BASE, SYSCALL MSRs) |
| `237fb60` | pvclock never armed; `IA32_PAT` discarded (guest saw PAT=0 = all-UC) |
| `825f283` | **INVPCID `#UD`** -- the "enable INVPCID" control was never set |
| `8a476c5` | **XSETBV** unhandled; CR4.OSXSAVE per-context; CPUID 0xD vs guest XCR0 |

### The pattern worth internalising
Every one of these is the same shape: **a feature SVM gets for free that VMX gates or
requires the hypervisor to emulate.** `vmload`/`vmsave` hands SVM the FS/GS bases and
SYSCALL MSRs; SVM has no INVPCID gate and leaves XSETBV unintercepted; the VMCB carries
PAT and EFER without extra controls. Anything in that category is a candidate for the
next fault. Check the VMX secondary controls and the unconditional-exit list against
what CPUID advertises.

### How they were found (this is the transferable part)
Five feature guesses in a row failed. What worked was making the guest report its own
fault:

- **`earlycon=uart8250,io,0x3f8,115200n8` — NOT `earlyprintk`.** earlyprintk is
  initialised in `setup_arch()`, i.e. *after* these faults, so it can never show them.
  earlycon is honoured earlier and produced full oopses with call traces.
- **`nokaslr`** pins the kernel base so a RIP is directly readable.
- ISO kept at `~/Downloads/alpine-hype-earlycon.iso`; rebuild recipe is
  `xorriso -indev X -outdev Y -boot_image any replay -map cfg /boot/grub/grub.cfg -commit`
  (the `replay` is what preserves El Torito/EFI bootability).
- `test.iso` in the bisect dir is the STANDARD image; swap deliberately and `mv` it back
  (it has ~47 hard links, so never overwrite in place).

## A. #236 / #251 — finish the live guest on Intel  [BLOCKED: needs the box]

Everything below is observation-driven. Do not attempt any of it without hardware; the
last five fixes came from reading guest oopses, not from reasoning about VMX.

### Resume here (exact steps)
1. `cp ~/Downloads/alpine-hype-earlycon.iso /mnt/data/hype-bisect/test.iso`
   (first `mv` the existing `test.iso` aside — do NOT overwrite, it has ~47 hard links).
2. `intel-probe.sh <label> "-DHYPE_RUN_SELFTEST_GUESTS=1 -DHYPE_VMX_SMOKE_TEST=1 \
    -DHYPE_FW_1_GUEST_RAM_MB=1024 -DHYPE_RUN_TWO_VMS=0"`
   Single VM matters: two VMs share one VMCS (#245) and inject a spurious
   VM-instruction-error 7 that contaminates every reading.
3. Read the log for the next kernel oops / hype panic and fix that. Repeat.
4. `mv` the original `test.iso` back when done.

- [~] **A1. The xstate.c:332 WARNING — ROOT CAUSE CONFIRMED 2026-08-02 (#252).** hype's
  own CPUID ring shows leaf 0xD returning DIFFERENT values on two consecutive passes:
  sub-leaf 0 EBX goes 0x240 -> 0xa88 and sub-leaf 1 EBX goes 0x240 -> 0x348, while the
  static per-component sub-leaves (2, 9) are identical both times. Those two are exactly
  the XCR0-dependent fields in leaf 0xD.

  Cause: `cpuid_emulate.c` passes ALL of leaf 0xD through from the host, but sub-leaf 0
  EBX ("size of the XSAVE area for states enabled in XCR0") and sub-leaf 1 EBX are read
  under whatever XCR0 is loaded at that instant. On VMX hype swaps XCR0 only after the
  guest's first XSETBV, so the two passes straddle the transition. The guest asks the
  same question twice and gets two answers -- CPUID stops being a pure function of
  (leaf, sub-leaf, guest XCR0). The WARNING's R12/R14=0x348 and R15=0xa88 are those two
  readings.

  Fix shape: derive both XCR0-dependent fields from the GUEST's XCR0 (VMX already tracks
  `g_vmx_guest_xcr0`; architectural reset value is 1 before the first XSETBV) by summing
  per-component sizes for the enabled bits. Leave the rest of leaf 0xD passthrough.
  CHECK SVM TOO: the leaf 0xD code is shared and SVM does not intercept XSETBV at all,
  so host/guest XCR0 can diverge there as well. Validate on both vendors.
- [ ] **A2. Whatever the guest hits next**, following the resume loop above. It was still
  running at 0.420s when the capture ended, so the next fault is unobserved -- there may
  be none at all until userspace.
- [ ] **A3. Reach an Alpine login prompt** (the M4-6d3 bar, and AMD's).
- [ ] **A4. The 14 remaining raw `hype_svm_*` calls** in the FW-1 region (#249). One was
  already a real bug (`9b760b6`: the guest page walk). The rest are mostly SVM-only
  diagnostics that want gating, not porting.

## B. #245 — VMX singletons (blocks two concurrent Intel guests)

One static vCPU ctx + one VMCS + one global EPT. Two Intel guests would collide.
Needs a pooled ctx/VMCS (the AMD analogue was #237's 2-slot VMCB pool, which
silently clamped and gave two cores one VMCB), **per-VM EPT roots**, and a
**VPID** per vCPU so TLB entries don't cross VMs.

## C. Two-VM Intel isolation run

Match AMD's proof: two Alpines, both logged in, filesystems provably isolated,
battery green, one boot. Gated on A and B.

## D. Real hardware (bare metal, not nested)

VMX-3, plus the **Intel halves** of #13 (M0-5), #43 (M3-6), #173 (M8-10).

> **Disk safety — non-negotiable.** Both of the Intel box's disks are off limits:
> `/dev/nvme0n1` is the user's BitLocker Windows install, and `5YDCT00331OA0CD2L`
> (`/dev/sda`) is the live Ubuntu root in an NVMe enclosure — excluded from
> native-HW testing too. The **only** authorised physical-write target anywhere
> is the AMD laptop's AHCI/SATA `2132E5BF4EAE`. Ship **no `hype.cfg`** on the
> validation stick; capture results via RT-3 (`\hype-diag-prev.txt`) + the frozen
> GOP screen, since the box is cold-boot-only/serial-less for real runs.

## E. Hygiene that has already cost time twice

- [ ] **E1.** The M4-6d5/M4-6d6 region (~240 lines: idle probe, TASKWALK with
  hard-coded Alpine 6.12.81 `task_struct` offsets, GRUB-spin window dump,
  CWATCH) is **completely ungated** and runs on every exit. `HYPE_FW1_DEBUG`
  exists (`core/fw1_debug.h`, default 0) and gates only 4 blocks. Note this is
  *not* a blocker any more — post-re-land those blocks go through the vendor
  shims, so it is perf/clarity, not Intel-blocking.
- [ ] **E2.** Two dump sites (undecodable-MMIO `hype_fatal`, any-exception dump)
  still print SVM debug state that is now deterministically **zero** on VMX —
  mark those `n/a` (#250). The `GUESTEXCP` insn-bytes half is DONE (`281186a`:
  physical read when `cr3=0`), and it paid for itself immediately by naming
  `MOV CR4, EAX` and then `MOV CR0, EAX`. This matters right now: the A2d panic
  is one of these two sites.
- [ ] **E3.** `probe.sh`'s progress check is fooled by two VMs interleaving on
  one serial port — it shredded `fw-1 EXHIST:` and reported `exits=0` where the
  real value was 1. Verdicts were right, the number wasn't.
- [ ] **E4.** The SONY validation stick still carries the `c6c055c` build.

## Work that does NOT need the Intel box

Available now, in rough value order:

- **#245 (pooled VMX ctx/VMCS, per-VM EPT roots, VPID).** The code can be written and
  AMD-regression-tested locally; only the two-guest *validation* needs Intel. Doing this
  first also removes the error-7 contamination from every future two-VM Intel reading.
- **#250 tail:** nothing outstanding -- all four `vmm_get_debug_state()` sites are
  guarded. Ticket can be closed on review.
- **#236 hygiene:** gate the M4-6d5/M4-6d6 diagnostic region behind `HYPE_FW1_DEBUG`.
  NOTE: this grew today (CRPROBE, EPTDUMP, MSRTRACE, CPUIDRING). Gate them, do not delete
  them -- they are the reason the last five root causes were findable, and the Intel work
  is not finished.
- **SONY validation stick** still carries `c6c055c`; nothing to validate on metal until
  Intel returns, so low priority.

## Lessons that cost real time here — worth not repeating

1. **A shim that returns "unavailable" without zeroing its out-param is a trap.**
   It cost hours twice: once as a misleading `#GP` dump, once as a garbage-CR3
   guest walk. Zero on the failure path *and* check the return.
2. **`vmm_get_debug_state` called itself** instead of
   `hype_svm_vcpu_get_debug_state` — infinite recursion, so every AMD guest that
   reached the idle probe never ran. It masqueraded as layout/codegen
   sensitivity: an emitted-but-never-*called* copy of the same body passed, and
   the 4120-byte frame vs 16384-byte stack refuted the size theory, because
   unbounded recursion overflows any stack. Swept the other 63 `vmm_*` shims;
   it was the only one.
3. **`patch --forward` onto a tree that already contains earlier hunks silently
   *skips* the overlapping later ones** — a skip is not a `FAILED`, so the build
   succeeds and the probe passes on an incomplete tree. That produced a bogus
   K=68 PASS. `slice.sh` now treats `Skipping patch`/`ignored` as fatal; always
   reconstruct the intended file independently and diff it before committing.
4. **`probe.sh` used to call a dead guest PASS** (it only checked for panics).
   It now fails when the last `EXHIST total < 1000`.
