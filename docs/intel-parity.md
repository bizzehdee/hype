# Intel (VT-x/VMX) → AMD parity: what is left

Status as of 2026-07-28, `329f435` + uncommitted diagnostic fixes.

## The bar (where AMD is)

Two Alpine guests on two dedicated cores, **both reaching a login prompt**, with
**isolated filesystems**, and the self-test battery green — all in **one boot**,
on **real AMD hardware**. That is the target Intel has to match.

## Where Intel actually is

Verified on the nested-VMX box (`192.168.0.144`, i5-13420H, `kvm_intel nested=Y`):

- Boots, enables VMX on the BSP **and** on an AP (`ap_vmm_ok=1` — #242's fix
  confirmed on Intel).
- All **17 microtests** run (M2–M4-5 battery).
- The **live FW-1 guest now enters** — the `kind != SVM` bail-out is gone, the
  device adapters are ported, and an EPT is built. Log grew 312 → 584 lines when
  the final re-land hunks landed.
- It does **not progress**. Two symptoms, from `run-intel-full68.log`:
  - a repeating **injected `#GP`** — `staged_eventinj=0xb0d` decodes as type 3
    (hardware exception), vector 0x0D, error-code-valid;
  - **nothing ever armed**: `pit_irq0=0 lapic_irq=0 ahci_irq=0`, LAPIC
    `lvt=0x10000(masked)`, `ever_armed=0x0`, PIC `mIMR=0xff sIMR=0xff`.
- **Never run on bare Intel metal** — nested only.

## A. #236 (VMX-4) — make the live guest work on Intel

- [x] **A1. Get a truthful `#GP` dump.** Was unusable: `xd` came from
  `vmm_get_debug_state()`, which is SVM-only and returned 0 **without writing
  the struct**, so on Intel the dump printed uninitialised stack —
  `rip=0x0 cs=0x0 rsp=0x0 insn=00 00 ...` — and fed a garbage CR3 into a guest
  page-table walk. It reads as "guest executing at address 0" and is pure noise.
  Fixed: neutral `info.guest_rip` + `vmm_get_cr3()`, CS/RSP dropped rather than
  faked, the shim now zeroes the struct on VMX, and the two `detail = .exitinfo2`
  sites keep their `info.qualification` fallback. **Needs an Intel re-run.**
- [ ] **A2. Diagnose the `#GP` storm** with real RIP/CR3 data.
- [ ] **A3. Timer + interrupt delivery on VMX.** Nothing is ever armed, so this
  is likely the bigger half: `VM_ENTRY_INTR_INFO` injection,
  `GUEST_INTERRUPTIBILITY_STATE`, `GUEST_ACTIVITY_STATE`, and the PIT/LAPIC/
  IO-APIC arm path. This is the VMX equivalent of M4-6b4, which is what got AMD
  ticking past `/init`.
- [ ] **A4. One Intel guest to an Alpine login prompt** (the M4-6d3 bar).
- [ ] **A5. The 14 remaining raw `hype_svm_*` calls** in the FW-1 region —
  decide port vs. gate for each. Mostly deliberate SVM-only diagnostics
  (`get_debug_state`, `get_mtrr_diag`), but `hype_svm_set_msr_trace(1)` is
  called unconditionally, including on Intel.

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
- [ ] **E2.** Two dump sites (`boot/main.c` ~8634 undecodable-MMIO `hype_fatal`,
  ~8867 any-exception dump) still print SVM debug state that is now
  deterministically **zero** on VMX. Mark those fields `n/a` so zeros are never
  mistaken for readings.
- [ ] **E3.** `probe.sh`'s progress check is fooled by two VMs interleaving on
  one serial port — it shredded `fw-1 EXHIST:` and reported `exits=0` where the
  real value was 1. Verdicts were right, the number wasn't.
- [ ] **E4.** The SONY validation stick still carries the `c6c055c` build.

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
