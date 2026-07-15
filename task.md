# Thin UEFI Hypervisor — Task List

Derived from `plan.md`. Each task has a stable ID and a `Deps:` line listing
the task IDs that must be **done** before it can start. Tasks with no unmet
deps are unblocked and can be picked up immediately; tasks in different
epics with no dependency between them can run in parallel.

Checkbox = done. `Deps: —` = no prerequisites.

**Minimum supported guest target (plan.md §1, decided 2026-07-14): Windows
(any 64-bit), Linux (any 64-bit), and BSD (any 64-bit) — no 32-bit guests.**
M3 validates the core VM-exit loop via a Linux-specific direct `bzImage` boot
(cheapest path, not the only supported one); M4+ guest-firmware work and M6's
dedicated BSD milestone bring Windows and BSD (and firmware-booted Linux) to
the same bar.

---

## SETUP — Pre-M0 readiness (plan.md §11)

- [x] **SETUP-1** — `git init`; add `LICENSE` (full GPLv3 text) and
  `.gitignore` for build artifacts.
  Deps: —
- [x] **SETUP-2** — Install and pin versions: C cross-toolchain targeting
  `x86_64-unknown-uefi` (clang/lld or GNU-EFI), QEMU, OVMF firmware image.
  Deps: —
- [x] **SETUP-3** — Confirm Secure Boot can be disabled on both the Intel
  and AMD test machines.
  Deps: —
- [x] **SETUP-4** — Confirm both test machines expose a serial (or
  equivalent) fallback debug channel available before GOP init succeeds.
  Deps: —
- [x] **SETUP-5** — Settle debugging workflow: QEMU `-s -S` + GDB against a
  debug build with symbols, plus serial logging as the real-hardware path.
  Deps: SETUP-2
- [x] **SETUP-6** — Write minimal freestanding primitives: `printf`-equivalent
  over UEFI `ConOut`, and a panic/assert stub.
  Deps: SETUP-2

---

## M0 — UEFI hello world (plan.md §9 M0, §12)

- [x] **M0-1** — Scaffold repo layout per plan.md §7 (`/boot`, `/core`,
  `/arch`, `/devices`, `/storage`, `/net`, `/fw`, `/tools`, `/docs`).
  Deps: SETUP-1
- [x] **M0-2** — Minimal UEFI app: print "hype" via `ConOut`, return
  `EFI_SUCCESS` cleanly.
  Deps: M0-1, SETUP-6
- [x] **M0-3** — Dump the UEFI memory map.
  Deps: M0-2
- [x] **M0-4** — Validate build/boot/deploy loop in QEMU+OVMF.
  Deps: M0-3, SETUP-2
- [ ] **M0-5** — Validate build/boot/deploy loop on real Intel + AMD
  hardware.
  Deps: M0-3, SETUP-3, SETUP-4

  *AMD half confirmed 2026-07-14: full build/boot/deploy loop (own
  memory map dump, `ExitBootServices()`, own GDT/paging/IDT swap, timer
  tick loop) now runs cleanly on two different real AMD machines (a
  laptop and a Ryzen 9 5950X/B550 desktop, both 32GB RAM) via the USB
  test package (`tools/make-usb-package.sh`) -- see M2-8's note for the
  real bugs found and fixed along the way. Intel/VMX half still
  unvalidated -- no physical Intel test hardware reachable from this
  environment; that half of this checkbox stays open.*

---

## M1 — Boot Services exit + own kernel context (plan.md §9 M1)

- [x] **M1-1** — `hype.cfg` config parser (plan.md §5 schema: `vcpus`,
  `cpu_set`, `mem_mb`, `boot`, `install_media`, `target_disk`,
  `target_disk_size_gb`, `firmware`, `os_hint`, `net_mode`).
  Deps: M0-4, M0-5
- [x] **M1-2** — Own GDT/IDT.
  Deps: M0-4, M0-5
- [x] **M1-3** — Own paging.
  Deps: M0-4, M0-5
- [x] **M1-4** — `ExitBootServices()` sequence; hypervisor becomes the only
  kernel.
  Deps: M1-2, M1-3
- [x] **M1-5** — Serial console driver.
  Deps: M1-4
- [x] **M1-6** — GOP linear-framebuffer text renderer (bitmap font blitter)
  — reused later by the dashboard (§6b) and guest firmware GOP exposure
  (§6/§6c).
  Deps: M1-4
- [x] **M1-7** — Panic handler: halt cleanly with a message, via M1-5/M1-6.
  Deps: M1-5, M1-6
- [x] **M1-8** — Timer tick (PIT/HPET bring-up for the host itself).
  Deps: M1-4

---

## ADM — Startup admission control (plan.md §6i, §10 decision #14/#16)

- [x] **ADM-1** — Sum configured `mem_mb` across all VMs; reject startup if
  it (plus hypervisor/device reserve) exceeds physical RAM from the UEFI
  memory map.
  Deps: M1-1, M0-3
- [x] **ADM-2** — Sum configured `vcpus` against physical core count;
  reject if it can't be satisfied under 1:1 pinning.
  Deps: M1-1
- [x] **ADM-3** — Validate explicit `cpu_set` entries: cores exist, count
  matches `vcpus`, and no two VMs' `cpu_set` ranges overlap (hard reject on
  overlap, not a warning).
  Deps: ADM-2
- [x] **ADM-4** — Reject startup if any two VMs' `target_disk` resolve to
  the same `file:` path or `physical:` serial/GUID, or if any two VMs would
  resolve to the same persisted varstore file (security-critical — closes
  the gap between §6d's "exclusively owned" claim and what's actually
  enforced; found in security review, §10 decision #20).
  Deps: M1-1
- [x] **ADM-5** — Validate `net_peers`: every listed name resolves to a VM
  actually defined in `hype.cfg`, and both VMs in a pairing have
  `net_mode = nat`; reject startup otherwise (§10 decision #21) — keeps
  guest-to-guest connectivity an explicit, auditable opt-in rather than a
  typo silently no-op'ing or leaving an unintended VM reachable.
  Deps: M1-1

*Note: ADM-1..5 gate VM startup and must be complete before M8's
multi-VM concurrency milestone, even though early single-guest milestones
(M2/M3) don't yet exercise the multi-VM overlap checks.*

---

## M2 — VMX/SVM bring-up, single vCPU (plan.md §9 M2, §10 decision #6/#17)

- [x] **M2-1** — CPU feature detection (VMX/EPT vs. SVM/NPT) and
  `vmm_ops` vtable dispatch (`vmx_ops` / `svm_ops`).
  Deps: M1-4
- [x] **M2-2** — VMXON (Intel) / SVM mode enable (AMD).
  Deps: M2-1
- [x] **M2-3** — Minimal VMCS (Intel) / VMCB (AMD) construction.
  Deps: M2-2
- [x] **M2-4** — Enable APICv (Intel) / AVIC (AMD) — required from this
  milestone, not deferred.
  Deps: M2-3
- [x] **M2-5** — VM-exit handler dispatch loop skeleton.
  Deps: M2-3
- [x] **M2-6** — Guest RAM zeroing before first VM-entry, on every
  (re)start (§10 decision #15).
  Deps: M2-3
- [x] **M2-7** — Launch a hand-written `hlt`-loop guest; confirm VM-exit
  round trip.
  Deps: M2-4, M2-5, M2-6
- [ ] **M2-8** — Real-hardware validation (Intel + AMD).
  Deps: M2-7, SETUP-3, SETUP-4

  *AMD half confirmed 2026-07-14 on two real machines (a laptop and a
  Ryzen 9 5950X/B550 desktop, both 32GB RAM), via a dedicated USB test
  package (`tools/make-usb-package.sh`) carrying every M2-M4-5 test
  guest plus screen-visible debug checkpoints (`hype_debug_print()`,
  core/fatal.h/halt.c) bracketing every real-hardware-risky step --
  added specifically because `hype_fatal()` never printed via UEFI's
  own ConOut, and GOP console registration originally happened only
  after the test guests ran, making an early panic indistinguishable
  from a silent hang on a screen-only setup with no serial capture.
  Two real, non-obvious bugs found this way, neither reproducible under
  this project's QEMU/KVM nested-SVM dev environment:
  1. `HYPE_NPT_MAX_GB`/M3-5's guest-CR3 identity map were both hardcoded
     to 4GB while the host's own paging already used 64GB -- QEMU's
     small test VMs always load the image under 4GB, but real UEFI
     firmware on a 32GB machine loaded it just past 5GB, leaving the
     guest's own entry point unmapped and triple-faulting
     (VMEXIT_SHUTDOWN) on its first fetch. Fixed by tying both to
     `HYPE_PAGING_MAX_GB` directly (arch/x86_64/svm/npt.h).
  2. M2-7's real-mode guest pointed CS.base/SS.base straight at a
     static buffer's address; AMD SVM only implements the low 32 bits
     of most VMCB segment base fields (vmcb.h's own `hype_vmcb_seg_t`
     comment already flagged this), so real silicon silently truncated
     CS.base whenever that buffer landed above 4GB -- nested SVM under
     QEMU/KVM apparently honors the full 64-bit field regardless. Fixed
     by wiring up `EFI_BOOT_SERVICES.AllocatePages` (previously an
     unused stub) and explicitly allocating that guest's code+stack
     below 4GB via `AllocateMaxAddress` (boot/main.c).
  VMX/Intel half still completely open -- no physical Intel test
  hardware reachable from this environment, and VMX's vcpu_create/
  vcpu_run trampoline (deferred at M2-7, see vmx_ops.c) still doesn't
  exist. Downstream milestones have proceeded past this point by the
  same explicit user decision (2026-07-13) as M0-5.*

---

## M3 — EPT + first real guest boot (plan.md §9 M3)

- [x] **M3-1** — EPT/NPT table construction (identity-mapped).
  Deps: M2-7
- [x] **M3-2** — 1:1 vCPU-to-pCPU pinning, including explicit `cpu_set`
  support.
  Deps: ADM-3, M2-7

  *Scope note: the real pinning mechanism is built and QEMU-validated
  -- EFI_MP_SERVICES_PROTOCOL dispatches the test guest's vCPU onto a
  specific, chosen non-BSP physical core (core/mp.c, boot/main.c),
  confirmed 5/5 clean runs with `-smp 2`. `hype_mp_pick_target_ap()`
  currently picks "any enabled non-BSP processor," not yet a specific
  core number driven by hype.cfg's parsed `cpu_set` list (ADM-3
  already validates that config, but nothing wires it to a real
  per-VM pinning decision yet) -- that's deferred to M8 alongside the
  rest of real multi-VM concurrency, matching every other
  single-instance-for-now scoping decision through M2/M3 (single VMCB,
  single AVIC/NPT table, ...).*
- [x] **M3-3** — Basic Linux boot protocol shim (direct `bzImage` boot, no
  firmware).
  Deps: M3-1

  *Scope note: parsing/construction logic only (setup-header
  validation, payload offset, 64-bit entry address, zero-page/E820
  construction), pure and 100%-tested against the documented Linux
  boot protocol -- not yet wired into an actual guest launch. That
  integration (loading a real bzImage, building guest page tables,
  VMRUNning it, confirming it actually runs) is M3-5's job once M3-4's
  device stubs exist too, per task.md's own dependency graph.*
- [x] **M3-4** — Minimal guest-visible device stubs: PIC/IOAPIC, PIT/HPET.
  Deps: M3-1

  *Scope note: implements the classic i8259 PIC + i8254 PIT pair (the
  same minimal choice real minimal VMMs like Firecracker make) rather
  than IOAPIC/HPET -- sufficient for a guest's early boot
  interrupt-controller/timer programming. Both are pure, tested
  register-level state machines (ICW1-4 init, OCW1/2/3, PIT
  mode/access/latch programming) -- not yet wired into an actual SVM
  IOIO-intercept dispatch (no guest has ever executed IN/OUT yet); that
  wiring, and validating a real guest programming these without
  hanging, is M3-5's job.*
- [x] **M3-5** — Boot a minimal Linux kernel end-to-end; validate
  APICv/AVIC interrupt delivery and the VM-exit loop under real device I/O.
  Deps: M3-3, M3-4, M2-4

  *Scope note: "boot a minimal kernel" done via a synthetic,
  hand-built bzImage (real setup_header validated through M3-3's
  shim, not bypassed) rather than a real production kernel -- same
  reasoning as M2-7's hand-written test guest: full control over the
  outcome, proving every new piece of plumbing actually works
  end-to-end (guest identity paging, long-mode VMCB construction, RSI
  delivery per the boot protocol, SVM IOIO interception with a real
  VM-exit loop that resumes the guest across exits, dispatch to
  M3-4's PIC/PIT stubs) before attempting a real, unpredictable
  kernel. Confirmed 5/5 clean QEMU/KVM runs: the guest halts cleanly
  after masking all PIC IRQs and latching+reading PIT channel 0,
  both observably reflected in the emulated devices' own state
  afterward. Real Fedora kernel boot attempt deferred as a stretch
  goal (user decision, 2026-07-14) -- expected to reach only early
  boot messages before panicking/hanging on missing ACPI/PCI/
  initramfs, which this hypervisor doesn't provide. Full AVIC
  interrupt-delivery validation (a guest's own ISR actually firing)
  is also deferred -- this pass enables AVIC's structural
  prerequisites (NPT via M3-1) but the test guest has no IDT of its
  own to receive an injected interrupt; that's a materially bigger
  lift (guest-side IDT + ISR) saved for real guest OS boot work.
  Two real, non-obvious bugs found and fixed along the way:
  HYPE_SVM_INTERCEPT_IOIO_PROT only enables the IOIO-intercept
  mechanism -- the IOPM bitmap (left all-zero, correct for M2-7's
  real-mode guest) must be explicitly filled to actually mark ports
  as intercepted, or guest port I/O silently reaches real hardware;
  and VMRUN clobbers every general-purpose register a guest touches,
  not just the ones given explicit asm constraints -- a missing
  clobber let the compiler assume a live pointer survived in RSI
  across VMRUN, corrupting it once guest code actually used RSI.*
- [ ] **M3-6** — Real-hardware validation (Intel + AMD).
  Deps: M3-5, SETUP-3, SETUP-4

  *AMD half confirmed 2026-07-14, same real-hardware pass as M2-8's
  note (two AMD machines, laptop + Ryzen 9 5950X/B550): M3-1's NPT,
  M3-2's pinning, and M3-5's full synthetic-bzImage guest launch
  (IOIO-intercept dispatch to the PIC/PIT stubs, clean HLT exit) all
  now confirmed on real AMD silicon, not just QEMU/KVM nested SVM --
  see M2-8's note for the two real-hardware-only bugs found and fixed
  (NPT/guest-paging map size, real-mode guest segment-base placement).
  The two bugs M3-5 itself found under QEMU (IOPM bitmap fill, VMRUN
  register clobbering) did not resurface on real hardware, for what
  that's worth. VMX/Intel half still completely open -- no physical
  Intel test hardware reachable from this environment, and VMX's
  still-nonexistent vcpu_run trampoline (deferred since M2-8) would
  still need to be written and debugged against real Intel hardware
  for the first time. Downstream milestones have proceeded past this
  point by the same explicit user decision (2026-07-13) as M0-5/M2-8.*

---

## INT — SVM guest-interrupt injection infrastructure

Discovered as a genuine prerequisite while scoping INPUT-1 (2026-07-15):
this project has no way to actually deliver an interrupt to a guest yet.
`devices/pic.h`'s `hype_pic_emu_raise_irq()` only sets an internal IRR
bit; nothing reads it or writes anything into the VMCB.
`arch/x86_64/svm/vmcb.h`'s own `eventinj`/`vintr` fields (offsets
0xA8/0x060) are already laid out from the AMD SDM but never populated,
and `arch/x86_64/cpu/vmexit.h` already flags this directly:
"[exception/interrupt] injection doesn't exist until M3+, so there is
nothing else this project can do with a VM-exit yet" -- still true. A
guest-facing PS/2 keyboard (INPUT-1) genuinely needs this: real OSes'
PS/2 drivers are IRQ1-driven, not purely polling, and this is
foundational infrastructure well beyond PS/2 (AHCI/virtio storage and
networking will eventually want it too, though everything built so far
has gotten away with pure guest-side polling). User decision
(2026-07-15): build this now, before INPUT-1, rather than starting with
a polling-only keyboard and deferring real interrupt delivery.

- [x] **INT-1** — EVENTINJ-based immediate interrupt injection: inject a
  maskable (INTR-type) interrupt directly via the VMCB's EVENTINJ field
  when the guest can accept it right now (RFLAGS.IF=1, no interrupt
  shadow). Pure bit-encoding logic unit tested; the actual VMCB write
  is exempt glue, same split as every other VMCB-touching function
  here.
  Deps: M3-1
- [x] **INT-2** — VINTR-window-based deferred injection: when the guest
  can't accept an interrupt immediately, request an interrupt-window
  VMEXIT (V_IRQ in the VINTR/int_ctl field, VINTR intercept enabled)
  and actually inject once that VMEXIT fires -- the real-hardware-
  correct path a guest with IF=0 (or mid-interrupt-shadow) needs.
  Deps: INT-1

  *Implemented together (one test guest proves both). `arch/x86_64/svm/
  vmcb.h`'s EVENTINJ/VINTR/interrupt_shadow bit-layout constants and
  `HYPE_SVM_EXITCODE_VINTR`/`HYPE_SVM_INTERCEPT_VINTR` were fetched and
  verified against the real AMD64 Architecture Programmer's Manual
  Volume 2, Rev 3.30 (Sept 2018) -- not reconstructed from memory,
  matching this file's own established rigor for AMD-specific fields.
  New pure functions in `vmcb.c` (100%/100%/100% unit tested,
  `core/tests/test_vmcb.c`): `hype_svm_encode_eventinj_intr()`,
  `hype_svm_can_accept_interrupt()` (RFLAGS.IF + interrupt-shadow
  gating), `hype_svm_arm_vintr_request()`/`_disarm_vintr_request()`.
  Exempt glue (`svm_vcpu.c`): `hype_svm_vcpu_request_interrupt()` (the
  device-facing API -- injects directly if the guest can accept now,
  otherwise arms a VINTR window and remembers the pending vector, one
  at a time, matching this project's own current single-IRQ-source
  scope) and `hype_svm_vcpu_handle_vintr_window()` (disarms + retries
  once the window genuinely opens). Also added
  `hype_svm_vcpu_set_idt()`/`_set_gdt()`/`_set_cs_ss_selectors()`,
  since every prior long-mode test guest's default GDTR/IDTR (base=0,
  no real table) and null CS/SS selectors were fine for VMRUN's own
  direct state load but not for a *genuine* hardware-validated
  interrupt delivery + IRETQ return.

  Proven end-to-end by a new dedicated test guest (`run_int_test()`,
  boot/main.c) -- a real guest GDT (flat code/data descriptors) + IDT
  (one populated 64-bit interrupt gate) in guest memory, a payload that
  loads a marker address, STIs, and busy-polls the marker until HLT,
  plus an ISR at a fixed offset that sets the marker and IRETQs.
  `hype_svm_vcpu_request_interrupt()` is called *before* the first
  VMRUN (RFLAGS.IF=0 at that point), deliberately exercising INT-2's
  deferred path first (arms the window) and INT-1's direct path second
  (once the guest's own STI opens it, firing EXITCODE_VINTR). Two real
  bugs found and fixed getting a clean run:
  - ***Bug***: the guest's own `vmcb->save.cs.selector`/`ss.selector`
    (0, this project's existing convention -- fine for VMRUN's direct
    state load, which never validates selectors against a real GDT)
    caused a SHUTDOWN (triple fault) right at the ISR's own IRETQ: a
    real interrupt delivery pushes the *current* CS selector onto the
    stack (and, in 64-bit mode, always SS too, unlike 32-bit mode)
    and IRETQ genuinely reloads both from the popped values --
    reloading the null selector is architecturally invalid for CS/SS
    (#GP), cascading into a triple fault with no #GP/#DF handler
    installed. Fixed via the new `hype_svm_vcpu_set_cs_ss_selectors()`
    setter, pointing both at real, present descriptors in the test's
    own constructed GDT.
  - Confirmed clean QEMU run: "vector 0x31 delivered via an armed
    VINTR window (INT-2) then direct EVENTINJ (INT-1), ISR ran and set
    the marker" -- every other existing test guest still halts
    cleanly.*

---

## INPUT — Input devices (plan.md §6b, §6c)

- [x] **INPUT-1** — Guest-facing PS/2 keyboard device.
  Deps: M3-4, INT-2

  *New `devices/ps2_keyboard.h/.c` (100%/100%/100% unit tested,
  `core/tests/test_ps2_keyboard.c`): a real i8042 controller/keyboard
  channel model (ports 0x60 data, 0x64 status/command) -- self-test
  (0xAA), interface test (0xAB), read/write config byte (0x20/0x60),
  enable/disable keyboard port (0xAE/0xAD), and a generic ACK for any
  keyboard-device command sent via 0x60, plus the single-pending-byte
  scancode buffer every real read ultimately goes through. Also closed
  the loop on `devices/pic.h`'s own long-dead
  `hype_pic_emu_raise_irq()`: a new `hype_pic_emu_acknowledge_highest_priority()`
  performs the real 8259 INTA-cycle equivalent (finds the
  highest-priority pending+unmasked IRQ, moves it IRR->ISR, computes
  its real vector from the chip's own ICW2-programmed offset) --
  fully unit tested, `core/tests/test_pic_emu.c`. Exempt glue
  (`svm_vcpu.c`): `hype_svm_vcpu_handle_ps2_kbd_ioio()` and the
  reusable `hype_svm_vcpu_deliver_pic_irq()` (raise + acknowledge +
  INT-1/INT-2 injection in one call -- every future PIC-routed device
  should reuse this same entry point).

  Proven end-to-end by a new test guest (`run_input_1_test()`,
  boot/main.c) -- a *realistic* OS-shaped sequence, not just a
  synthetic harness: the guest programs the master 8259 itself
  (ICW1-4, unmasking only IRQ1), enables interrupts, and busy-waits;
  its own ISR reads the delivered scancode from port 0x60, sends a
  real EOI, and sets a marker. Two real timing bugs found and fixed
  getting a clean run:
  - ***Bug***: the initial design raised IRQ1 *before* the guest's
    first VMRUN, expecting it to stay masked-but-pending through the
    guest's own PIC init -- but a real 8259's own ICW1 unconditionally
    clears IRR as part of a fresh initialization (discarding any
    previously pending state, matching real hardware), wiping out the
    pre-raised bit before the guest ever unmasks it. A real keypress
    arriving before an OS has initialized its own PIC is genuinely
    lost on real hardware too -- fixed by only raising IRQ1 once the
    guest's own PIC initialization has genuinely finished (tracked via
    `hype_pic_emu_chip_t`'s own `init_state` reaching 0), matching
    realistic timing instead.
  - ***Bug***: an early retry design called the combined
    raise+acknowledge+inject helper on *every* subsequent PIC port
    write (to catch the guest's OCW1 unmask) -- since raising is
    unconditional, this re-raised and redelivered the same keypress
    forever, including via the EOI write the ISR's own interrupt
    handler performs (an infinite redelivery loop). Fixed by
    separating "raise" (done exactly once) from "acknowledge" (safe to
    retry speculatively any number of times -- a genuine no-op once
    IRR's bit is already serviced).
  - Confirmed clean QEMU run: "scancode 0x1e delivered via PS/2 -> PIC
    (vector 0x21) -> INT-1/INT-2, ISR read it back correctly" -- every
    other existing test guest still halts cleanly.*
- [x] **INPUT-2** — Guest-facing PS/2 mouse device (for GUI installers,
  §6c).
  Deps: INPUT-1

  *New `devices/ps2_mouse.h/.c` (100%/100%/100% unit tested,
  `core/tests/test_ps2_mouse.c`): the i8042's own "auxiliary" channel
  -- RESET (queues ACK+self-test-pass+device-ID, matching real
  power-on semantics), enable/disable data reporting, get device ID,
  set defaults, a generic ACK for anything else, and a small FIFO
  (unlike the keyboard's single-pending-byte scope -- RESET's own
  3-byte response and every movement packet both need in-order,
  multi-byte reads). `devices/ps2_keyboard.h/.c` (INPUT-1) extended
  with the controller-level routing every real i8042 needs to
  multiplex both channels through the same ports: 0xA7/0xA8/0xA9
  (aux port disable/enable/test) and 0xD4 (write-to-aux, a one-shot
  prefix consumed by the new `hype_ps2_kbd_take_aux_data_write()`)
  plus `hype_ps2_kbd_has_pending_byte()` (mirroring the mouse's own
  query, letting the combined status byte reflect either channel).
  New exempt glue `hype_svm_vcpu_handle_ps2_ioio()` (`svm_vcpu.c`)
  routes port 0x60 writes to keyboard or mouse based on that one-shot
  flag, and reads (0x60 data, 0x64 status) prefer the mouse's own
  pending byte when present, setting `HYPE_PS2_STATUS_AUX_DATA` --
  matching real hardware's single shared data path. Delivery reuses
  INPUT-1's own `hype_svm_vcpu_deliver_pic_irq()` unchanged, now
  targeting the *slave* PIC's IRQ4 (IRQ12 overall) instead of the
  master's IRQ1.

  Proven end-to-end by a new test guest (`run_input_2_test()`,
  boot/main.c) -- a genuinely realistic mouse-enable sequence: the
  guest initializes *both* the master 8259 (unmasking only IRQ2, the
  slave's own cascade line -- required for any slave-originated IRQ to
  reach the CPU at all) and the slave 8259 (unmasking only IRQ4),
  enables the controller's aux port, then speaks to the mouse itself
  through the 0xD4 prefix to enable data reporting -- reading back its
  ACK before proceeding, exactly as a real driver must. The ISR reads
  the delivered 3-byte movement packet from port 0x60 and sends EOI to
  *both* the slave (ending this specific IRQ) and the master (ending
  the cascade's own in-service state), a real driver detail this
  project's own PIC model doesn't enforce but every real OS observes.
  Clean QEMU run on the first attempt (the timing lessons from
  INPUT-1's own two bugs -- gate the device event on the guest's own
  readiness, never re-raise on retry -- applied directly this time):
  "mouse packet 0x8/0x5/0xfb delivered via PS/2 -> PIC (vector 0x2c)
  -> INT-1/INT-2, ISR read it back correctly" -- every other existing
  test guest still halts cleanly.*
- [x] **INPUT-3** — Host-level keyboard controller ownership + raw scancode
  interception, beneath any guest.
  Deps: M1-4

  *A real hardware driver (not guest emulation) -- once M1-4's
  `ExitBootServices()` has run, UEFI's own Simple Text Input Protocol
  is gone for good, so the host itself must read the real i8042
  controller directly for its own purposes (the dashboard leader
  chord, INPUT-4). Same split as every other host driver here
  (`arch/x86_64/cpu/pit.c`/`pit_hw.c`): new `arch/x86_64/cpu/ps2_host.h/.c`
  (a pure ring buffer, 100%/100%/100% unit tested,
  `core/tests/test_ps2_host.c`) plus `ps2_host_hw.c` (exempt -- real
  `inb` from port 0x60, `hype_isr_register()`, `hype_pic_unmask_irq()`),
  wired into `efi_main()` right after M1-8's own timer/PIC setup
  (`HYPE_HOST_KBD_VECTOR = HYPE_TIMER_VECTOR + 1`, reusing the SAME PIC
  remap the timer already did rather than remapping again, which would
  re-mask every line). `hype_host_kbd_poll_scancode()` is the API the
  dashboard/leader-chord recognizer (INPUT-4) will poll.

  Structurally identical to `hype_timer_isr()`'s own shape (already
  validated on real AMD hardware, M2-8/M3-6's own notes) -- same
  ISR-register + IRQ-unmask + EOI pattern, just a different port/
  vector. **Not live-verified via an actual keypress this session**:
  attempted via QEMU's own monitor (`sendkey`) while hype.efi was
  running, but discovered that `run_all_test_guests()` (line ~3609)
  executes *before* this timer/keyboard bring-up block (line ~3719) in
  `efi_main()`'s own sequential flow -- meaning FW-1's own parked,
  deliberate panic (`hype_fatal()` -> `hype_halt_forever()`) halts the
  entire host before execution ever reaches this code at all. This is
  a genuine, pre-existing ordering quirk (test-guest dispatch blocks
  all host-level bring-up that comes after it), not something INPUT-3
  itself introduced -- worth revisiting whether host kernel bring-up
  should happen *before* test-guest dispatch instead, once FW-1 is
  unparked or the test-guest sequence is reworked. Deferred rather than
  reordering boot-critical code as a side effect of this task.*
- [x] **INPUT-4** — Leader-chord recognition: `Right-Ctrl+Right-Alt` held +
  action key (`D`, `1`-`9`, `Left`/`Right`, `Esc`).
  Deps: INPUT-3

  *Scope was deliberately narrow, matching the task's own title -- pure
  chord *recognition* over raw host scancode bytes, not the dashboard/
  VM-switching it will eventually drive (that's M8-1's job, per
  plan.md §6b). New `arch/x86_64/cpu/leader_chord.h/.c`: a pure decoder
  (100% line, 91.30% branch coverage, `core/tests/test_leader_chord.c`,
  13 tests) that tracks Right-Ctrl/Right-Alt held state byte-by-byte
  and, once both are held, recognizes `D` (toggle dashboard), `1`-`9`
  (jump to VM N), `Left`/`Right` (cycle prev/next), `Esc` (return to
  dashboard) -- returning one of `HYPE_CHORD_ACTION_*` plus a `vm_index`
  for the digit case. No hardware access at all; feed it bytes
  (`hype_host_kbd_poll_scancode()`, INPUT-3), get back actions.

  Scan Code Set 1 make/break byte values (Right-Ctrl `E0 1D`/`E0 9D`,
  Right-Alt `E0 38`/`E0 B8`, `D` `20`/`A0`, `1`-`9` `02`-`0A`/`82`-`8A`,
  `Esc` `01`/`81`, Left-Arrow `E0 4B`/`E0 CB`, Right-Arrow `E0 4D`/
  `E0 CD`) were fetched and confirmed against a real reference table at
  implementation time, not reconstructed from memory -- same rigor this
  project applies to every other hardware protocol constant. Left-Ctrl/
  Left-Alt share the same base byte as their right-side counterparts
  but arrive with no `0xE0` prefix -- a dedicated test
  (`test_left_ctrl_alt_are_not_confused_with_right_variants`) confirms
  they're correctly rejected rather than accidentally satisfying the
  chord.

  Wired a minimal driving loop into `efi_main()`'s existing ~1s tail
  loop (right after the M1-8 timer/PIC bring-up block): drains
  `hype_host_kbd_poll_scancode()`, feeds each byte through the decoder,
  and reports any recognized action via `hype_serial_print()` -- the
  only currently-observable proof available, since no dashboard/VM
  list exists yet to actually act on it. **Live keypress verification
  is blocked by the same pre-existing ordering issue documented under
  INPUT-3**: `run_all_test_guests()` runs before this code, and FW-1's
  parked panic halts the host before it's ever reached. Unit tests are
  the full verification for this task until that ordering issue is
  resolved (post-FW-1-unpark) or M8-1 gives this a real consumer to
  exercise end-to-end.*

---

## VIDEO — Display devices (plan.md §6, §6b)

- [x] **VIDEO-1** — (= M1-6) GOP linear-framebuffer text renderer.
  Deps: M1-6
- [x] **VIDEO-2** — Guest-facing GOP protocol exposure, pre-OS-driver
  (writes into a per-VM framebuffer in guest RAM).
  Deps: M1-6, M3-1

  *Implemented as QEMU's "ramfb" protocol (`devices/ramfb.h`/`.c`),
  not a custom scheme -- this project's vendored, unmodified OVMF
  (M4-2) already ships `OvmfPkg/QemuRamfbDxe` (confirmed present in the
  vendored build), which discovers a writable fw_cfg file `"etc/ramfb"`
  and writes a 28-byte `RAMFB_CONFIG` struct (guest-chosen framebuffer
  address + format/width/height/stride, every field big-endian) back
  into it once it has allocated its own framebuffer in guest RAM --
  struct layout and field order transcribed directly from
  `edk2/OvmfPkg/QemuRamfbDxe/QemuRamfb.c`, not reconstructed from
  memory. Required extending `devices/fw_cfg.c` (M4-4) with its first
  writable file: `hype_fw_cfg_add_writable_file()` plus a
  `hype_fw_cfg_dma_execute()` WRITE path that copies guest-supplied
  bytes into the file's own backing buffer (bounds-checked against a
  guest-supplied length, per VALID's own invariant) -- every other file
  this project serves via fw_cfg stays structurally read-only by
  construction (a separate `write_data` field, not a dropped `const`),
  so this doesn't loosen guest-isolation for the ACPI content M4-4
  already serves. Confirmed OVMF's own `QemuFwCfgWriteBytes()` uses the
  DMA write path here, not the classic port-based one, since this
  project's fw_cfg model already advertises DMA support (M4-4).
  Validated end-to-end with a synthetic long-mode test guest, same
  rigor/pattern as M4-4's own fw_cfg DMA test with the roles reversed:
  host pre-builds a RAMFB_CONFIG in guest memory (standing in for what
  a real OVMF driver computes), the guest payload triggers/polls the
  DMA write, and the host decodes the fw_cfg file's resulting backing
  buffer (`hype_ramfb_decode_config()`) and confirms every field
  matches byte-for-byte. Multiple clean QEMU runs.
  Scope is the protocol/transport only -- actually presenting the
  guest's framebuffer content on the host's real screen is VIDEO-3's
  job, and a real OVMF instance actually driving this (not a synthetic
  test guest mimicking its wire behavior) is M4-6's job, same
  "primitive now, integration later" split as M4-3/M4-4/M4-5.*
- [x] **VIDEO-3** — Post-boot VGA/Bochs-VBE-class virtual display adapter
  (for Windows' inbox Basic Display Adapter and Linux/BSD `vesafb`/`efifb`).
  Deps: VIDEO-2

  *Modeled after QEMU's "bochs-display" device specifically (vendor
  0x1234/device 0x1111, class 0x03/0x80/0x00 -- deliberately not the
  combined legacy-VGA "std-vga" device, matching this project's own
  "simplest device that satisfies real guest drivers" bias). Register
  indices, the MMIO-window addressing formula (BAR2 offset 0x500 +
  register*2), ENABLE flag bits, and framebuffer/mode semantics were
  fetched and confirmed directly from QEMU's own source
  (`hw/display/bochs-display.c`, `include/hw/display/bochs-vbe.h`) via
  a dedicated research pass, not reconstructed from memory -- same
  discipline as this project's other wire-format structs.

  New `devices/bochs_vbe.h/.c`: a pure DISPI register model
  (`hype_bochs_vbe_mmio_read/_write`) plus `hype_bochs_vbe_get_mode()`,
  which computes stride/fb-offset from XRES/YRES/BPP/VIRT_WIDTH/
  X_OFFSET/Y_OFFSET, auto-raising a too-small VIRT_WIDTH/HEIGHT to the
  requested resolution (the well-documented real Bochs VBE "auto-
  configure" convention). 100% line, 97.06% branch coverage
  (`core/tests/test_bochs_vbe.c`, 12 tests). Only bpp 16/32 are real,
  matching bochs-display's own restricted mode set.

  New `devices/fb_blit.h/.c`: the other half of VIDEO-2's own note that
  "the actual blit of the guest's framebuffer content onto the host's
  real screen is VIDEO-3's job" -- a pure pixel-format-converting
  row-copy (XRGB8888/XBGR8888/RGB565), clipped to the smaller of
  source/destination dimensions. 97.50% line, 93.18% branch coverage
  (`core/tests/test_fb_blit.c`, 11 tests).

  PCI wiring follows PCI-2's exact established pattern: exempt
  `hype_svm_vcpu_handle_bochs_vbe_npf()` (`arch/x86_64/svm/svm_vcpu.c`)
  NPT-traps only BAR2 (the MMIO register window, both-bounds-checked
  like the ECAM handler, rejecting anything but a 2-byte-wide MOV since
  DISPI registers are architecturally 16-bit-only) -- BAR0 (the
  framebuffer) is deliberately **never** NPT-trapped, so pixel writes
  take zero VM-exits, matching real VRAM's own behavior. This is also
  why BAR0's chosen address is the test's own real static buffer
  address (`g_video_3_framebuffer`), not an arbitrary formula-based GPA
  the way BAR2/ECAM are -- pixel writes need genuinely backed, readable
  memory, unlike a register window that's always intercepted.

  New synthetic test guest `run_video_3_test()` (`boot/main.c`):
  discovers the device via PCI/ECAM (byte-for-byte PCI-2's own
  enumeration idiom, extended to place a second BAR), programs
  XRES=320/YRES=200/BPP=32/ENABLE via BAR2, writes a first/last-pixel
  test pattern directly into BAR0, then HLTs. Host verifies
  `hype_bochs_vbe_get_mode()` decodes the expected mode, the pixel
  writes round-tripped through the framebuffer, and
  `hype_fb_blit_copy()` correctly carries both pixels into a
  stand-in "host screen" buffer -- proving the blit against this
  device's own real, guest-driven output, not just synthetic unit-test
  buffers. **Clean QEMU run on the second attempt**: the first attempt
  used a test pixel value with a nonzero top ("reserved"/X) byte, which
  `pack_rgb()` legitimately zeroes on every repack (it's padding, not
  real color data) -- not a blit bug, just an unrealistic test value;
  fixed by choosing pixel values with a zero reserved byte.

  Wiring an actual *live* blit into `efi_main()`'s own tail loop
  (rendering onto the real GOP framebuffer) was deliberately NOT done
  here -- there's no "current VM"/console-focus concept yet for a live
  blit to attach to (that's M8's dashboard job), and the same
  `run_all_test_guests()`-before-host-bringup ordering issue already
  documented under INPUT-3/INPUT-4 means it couldn't be live-verified
  yet regardless. Revisit once M8 gives this a real consumer.*

---

## VALID — Guest-supplied input validation (plan.md §6j, §10 decision #19)

Found during security review: the actual guest-escape vector isn't
EPT/NPT (that only stops direct guest-to-guest memory access) — it's the
hypervisor trusting a guest-supplied address/length in device emulation
code. Foundational to every device model task below; not optional.

- [ ] **VALID-1** — Guest-physical-address translation/bounds-check helper:
  given a VM, a guest-physical address, and a length, validate against that
  VM's own EPT/NPT-mapped range before returning a host-virtual pointer. All
  device emulation code paths must go through this, never dereference a
  raw guest-supplied address directly.
  Deps: M3-1
- [ ] **VALID-2** — Apply VALID-1 to virtio queue descriptor processing
  (virtio-blk, virtio-net).
  Deps: VALID-1
- [ ] **VALID-3** — Apply VALID-1 to AHCI/NVMe command FIS buffer pointers,
  plus explicit LBA+sector-count bounds-checking against the backing
  store's actual size (file length or physical disk capacity) before any
  read/write — reject out-of-range requests, never clamp/truncate silently.
  Deps: VALID-1
- [ ] **VALID-4** — Apply VALID-1 to any other guest-supplied buffer used
  by device emulation (PS/2, framebuffer-adjacent paths) as those devices
  are built.
  Deps: VALID-1

*Note: M5's `blk_backend` (file and physical implementations) and NET's
guest-facing devices depend on VALID-1/2/3, not just their own device-model
tasks — see updated deps below.*

---

## M4 — Guest UEFI firmware + ACPI synth (plan.md §9 M4, §10 decision #1)

- [x] **M4-1** — EDK2 build pipeline for the guest firmware blob (separate
  from `hype.efi`'s own toolchain).
  Deps: SETUP-2

  *`edk2` vendored as a git submodule pinned to `edk2-stable202511`,
  plus one local commit fixing a real NASM 3.x regression (see that
  commit's message) -- this dev environment's Fedora release ships
  NASM 3.01 with no older version available, and 3.x rejects "push
  strict dword <imm>"/"push dword <imm>" in 64-bit mode, which
  UefiCpuPkg's X64 IDT stub generation relies on. `tools/build-fw.sh`
  automates the whole pipeline (BaseTools built with clang, then
  OvmfPkg/OvmfPkgX64.dsc via the CLANGDWARF tag) end-to-end,
  confirmed reproducible by re-running it.*
- [x] **M4-2** — Vendor/strip an OVMF build as the guest firmware base.
  Deps: M4-1

  *`fw/OVMF_CODE.fd`/`fw/OVMF_VARS.fd` committed (plan.md §7's "vendored
  blob," not just a build script -- downstream consumers get a working
  firmware pair without needing the EDK2 toolchain themselves).
  Smoke-tested standalone (reaches BdsDxe correctly) and booting
  `hype.efi` itself through its own full existing test suite exactly
  as `edk2-ovmf` already does. Not yet used as actual guest-facing
  firmware for a VM -- that's M4-3 onward.*
- [x] **M4-3** — Emulated flash/varstore, persisted to disk.
  Deps: M4-2

  *CFI (Common Flash Interface) NOR-flash command-protocol emulation
  (`devices/pflash.h`/`.c`: WRITE_BYTE/BLOCK_ERASE/CLEAR_STATUS/
  READ_STATUS/READ_DEVID/WRITE_TO_BUFFER/READ_ARRAY) backed by an
  in-memory buffer, fully unit-tested. MMIO trapping via NPT: guest
  accesses to the flash's 2MB range are forced to fault
  (`hype_npt_mark_not_present()`) into a real SVM NPF (#VMEXIT_NPF)
  handler (`hype_svm_vcpu_handle_npf()`) that decodes the faulting
  MOV/MOVZX instruction (a narrow, purpose-built x86_64 decoder,
  `arch/x86_64/cpu/mmio_decode.h`/`.c`, scoped to exactly the forms
  EDK2's own MmioRead8/MmioWrite8-style library calls compile to;
  fully unit-tested including ModRM/SIB/disp8/disp32/RIP-relative
  addressing) and dispatches to the flash model. Originally planned to
  read the faulting instruction via AMD SVM Decode Assist
  (VMCB `num_bytes_fetched`/`guest_instruction_bytes`, confirmed
  present via CPUID on this dev environment's host CPU) but confirmed
  empirically, via real QEMU/KVM runs, that nested SVM does not
  reliably populate those fields even when the CPU advertises the
  feature -- switched to reading guest memory directly at RIP instead
  (a plain host pointer dereference, since this project's guest/NPT
  setup is a flat identity map), which is more portable and no longer
  depends on that hardware feature at all. `hype_svm_vcpu_run()`'s
  VMRUN sequence was also extended to capture/restore every
  general-purpose register (previously only RAX/RSI), needed so the
  NPF handler can read a write's source register or patch a read's
  destination register for any register compiled MMIO-accessor code
  happens to use.
  Validated end-to-end with a synthetic long-mode test guest
  (hand-written machine code, same rigor as M3-5): issues a real
  WRITE_BYTE command and data byte through genuine memory-mapped
  stores, reads the byte back through a genuine memory-mapped load,
  then writes that read-back value out to a second offset -- so the
  host can confirm both the write and read paths from the flash's
  backing array alone. 5/5 clean QEMU runs, plus confirmed on real AMD
  hardware 2026-07-14 (see M2-8's note).
  Real persistence to a host file explicitly deferred -- that needs a
  disk driver, M5's job; this milestone's own dependency graph would
  otherwise be circular. The in-memory device model and NPT-based MMIO
  trap mechanism are both reusable as-is once M5 exists.*
- [x] **M4-4** — Per-VM ACPI table synthesis (RSDP/XSDT/FADT/MADT/MCFG).
  Deps: M4-2, M3-2

  *Since M4-2 uses real, vendored OVMF (not custom-written firmware),
  its stock AcpiPlatformDxe driver never builds ACPI content itself --
  it fetches it via QEMU's own fw_cfg device and a "linker/loader"
  script that tells firmware how to allocate memory for the blob,
  patch cross-table pointers, and recompute checksums once real
  addresses are known. Implementing genuine fw_cfg + linker-loader
  support (rather than patching our own OVMF build to skip it) was an
  explicit user choice, so stock OVMF works unmodified. `devices/acpi.h`/
  `.c` builds RSDT-independent XSDT+FADT+MADT+MCFG+DSDT content as one
  relocatable blob (every cross-table pointer field pre-filled with
  the *target's offset within that blob*, not a final address; every
  checksum byte left 0) -- FADT targets ACPI's Hardware-Reduced
  profile, needing no legacy PM1a/PM-timer/GPE emulation; DSDT is a
  header-only placeholder (no AML), deferred until a real device
  actually needs one. `devices/acpi_loader.h`/`.c` builds the exact
  128-byte-entry "etc/table-loader" wire format QEMU/OVMF define
  (ALLOCATE/ADD_POINTER/ADD_CHECKSUM), struct layout and field order
  fetched from QEMU's own bios-linker-loader.h, not reconstructed from
  memory. `devices/fw_cfg.h`/`.c` is the device model itself (classic
  selector/data ports 0x510/0x511 plus the real DMA interface at
  0x514/0x518) -- port numbers, well-known keys, and the DMA struct's
  big-endian wire encoding fetched from QEMU's own
  standard-headers/linux/qemu_fw_cfg.h and cross-checked directly
  against this project's own vendored OVMF driver source
  (edk2/OvmfPkg/Library/QemuFwCfgLib), which is also what caught that
  OVMF's actual DMA-support probe reads a classic-interface feature bit
  (FW_CFG_ID) rather than reading back the DMA address register's
  "QEMU CFG" signature -- this device doesn't need that probe path at
  all. Every one of these three modules is pure logic, 100%/100%
  region/line covered (acpi.c, acpi_loader.c) or 96.9%/97.5%/93.5%
  region/line/branch (fw_cfg.c); wiring into the exempt SVM IOIO glue
  (`hype_svm_vcpu_handle_fw_cfg_ioio()`, arch/x86_64/svm/svm_vcpu.c) is
  a thin adapter, same layering as M3-4's PIC/PIT and M4-3's pflash.
  Validated end-to-end with a synthetic long-mode test guest
  (hand-written machine code, same rigor as M3-5/M4-3): the guest
  speaks fw_cfg's real DMA protocol to fetch "etc/acpi/rsdp" into a
  guest buffer, and the host confirms every byte matches what
  `hype_acpi_build_rsdp()` built. 5/5 clean QEMU runs, plus confirmed
  on real AMD hardware 2026-07-14 (see M2-8's note). Found and fixed
  one real bug this way: an 8-byte little-endian immediate-patch helper
  was reused against a 4-byte immediate slot in the test payload,
  silently corrupting the following instruction's opening bytes.
  NOT yet validated: real, vendored OVMF actually booting as a nested
  guest and its AcpiPlatformDxe successfully consuming this content
  end-to-end (confirming the linker-loader script itself, not just the
  fw_cfg transport) -- that integration is M4-6's job, matching this
  project's own "build the primitive now, defer the harder integration"
  pattern (e.g. M4-3's flash persistence).*
- [x] **M4-5** — Virtual optical drive device (read-only ISO passthrough,
  AHCI/ATAPI or virtio-scsi CD-ROM).
  Deps: M3-1

  *AHCI/ATAPI chosen over virtio-scsi (explicit user decision): every
  guest OS family (Linux/BSD/Windows) has inbox AHCI/ATAPI drivers, so
  this is reusable as-is for M7's Windows installer boot later instead
  of needing a second CD-ROM transport built then. Register offsets,
  bit layouts, and the Command Header/PRDT/Register-FIS wire formats
  (`devices/ahci.h`/`.c`) are transcribed directly from the Linux
  kernel's own AHCI driver (drivers/ata/ahci.h,
  drivers/ata/libata-sata.c's ata_tf_to_fis()/ata_tf_from_fis()) --
  fetched and read for this task, not reconstructed from memory.
  Scoped to exactly one port with one ATAPI device attached (this
  milestone's own scope). `devices/atapi.h`/`.c` is the ATAPI/SCSI
  command layer (TEST UNIT READY, INQUIRY, READ CAPACITY(10),
  READ(10), REQUEST SENSE -- the commands a real ATAPI driver actually
  issues to enumerate and read a disc), backed by an in-memory "ISO"
  buffer -- real host-file reading needs M5's disk driver, the same
  circular-dependency situation M4-3's flash persistence and M4-4's
  ACPI blob already had, so real media is deferred the same way.
  Both modules are pure logic, 100%/100%/99%+ region/line/branch
  covered; MMIO wiring reuses M4-3's NPF/hype_mmio_decode() mechanism
  unchanged (AHCI registers are accessed via ordinary MOV instructions,
  same as pflash's), with a new exempt command-processing step
  (`hype_svm_vcpu_handle_ahci_npf()`/`process_ahci_command_slot0()`,
  arch/x86_64/svm/svm_vcpu.c) that walks the guest's Command List/
  Command Table/PRDT on a PxCI (Command Issue) write and copies the
  ATAPI response into the guest's own PRDT-described buffer(s).
  Validated end-to-end with a synthetic long-mode test guest
  (hand-written machine code for the register setup/trigger/poll
  sequence; the Command Header/Table/CDB/PRDT content itself is
  host-built directly into guest memory, same convention as M4-4's
  fw_cfg test): issues a real READ(10) for one sector via the actual
  AHCI/ATAPI protocol, and the host confirms the transferred sector
  matches the backing buffer byte-for-byte. 5/5 clean QEMU runs, plus
  confirmed on real AMD hardware 2026-07-14 (see M2-8's note).
  NOT yet validated: a real guest OS's own AHCI/ATAPI driver (Linux's
  ahci+sr_mod, or UEFI's own AhciBusDxe during M4-6's boot) actually
  enumerating and reading from this device -- that's M4-6's job.*
- [ ] **M4-6** — Boot a stock Linux UEFI installer ISO (e.g. Debian
  netinst) end-to-end through GRUB.
  Deps: CPUMSR-2, RAM-2, PCI-2, FW-2, ISO-2

  *Scoping this task out (2026-07-14) surfaced that "boot a stock
  Linux ISO through GRUB" actually needs ~7 substantial new subsystems
  never separately planned: CPUID/MSR interception (currently neither
  exists at all -- every guest CPUID/RDMSR/WRMSR reaches real hardware
  unmediated, a guest-isolation gap), dynamic per-VM guest RAM
  allocation + NPT sizing (currently a fixed blanket map, not driven by
  hype.cfg's mem_mb), a real OVMF reset-vector boot path (every guest
  so far starts at a hand-picked entry_phys, never real firmware), and
  PCI configuration-space emulation (devices/acpi.h's own MCFG comment
  already flagged this as unbacked -- without it no guest driver can
  even discover M4-5's AHCI device). Real ISO loading, by contrast,
  does NOT need M5 (which depends on M4-6, not the reverse) -- UEFI's
  own Boot-Services file I/O can read a file from the same ESP hype.efi
  boots from. Split into the new CPUMSR/RAM/PCI/FW/ISO sections below
  rather than attempted as one task -- M4-6 itself is now the final
  GRUB+Linux integration step once all five are done.*

---

## CPUMSR — CPUID/MSR interception baseline (plan.md's guest-isolation
## invariant; a gap M4-6's own scoping surfaced, 2026-07-14)

- [x] **CPUMSR-1** — CPUID intercept + minimal safe/synthesized leaf set.
  Deps: M2-3

  *Confirmed via grep that CPUID previously had zero interception at
  all -- executed natively against the real host CPU, a guest-
  isolation gap surfaced while scoping M4-6. Adds
  `HYPE_SVM_INTERCEPT_CPUID` (bit 18 of intercept_misc1) and
  `HYPE_SVM_EXITCODE_CPUID` (0x72) to `vmcb.h`, cross-referenced
  against the AMD SVM Intercept Vector 3 layout and Appendix C exit
  codes -- confirmed internally consistent with this project's own
  already-established neighboring constants (HLT=24/0x78,
  IOIO_PROT=27/0x7B, MSR_PROT=28/0x7C, SHUTDOWN=31/0x7F all come from
  the same real table). Set in both VMCB builders (`vmcb.c`) -- a
  correctness fix applying retroactively to every existing test guest,
  though none of them execute CPUID so no behavior change for M2-7
  through VIDEO-2's own tests.
  New pure-logic module `arch/x86_64/cpu/cpuid_emulate.h`/`.c`
  (`hype_cpuid_emulate()`) synthesizes a deliberately minimal leaf set
  rather than reinventing every field from scratch: reads the real
  host CPU's own CPUID result for the same leaf/subleaf and passes
  most fields straight through (family/model/stepping and most feature
  bits aren't isolation-sensitive), curating only what matters --
  max basic/extended leaf capped at 1/0x80000001 so well-behaved guest
  software never reaches an unhandled leaf (anything else safely
  returns all-zero, the same convention real hardware uses for a
  reserved leaf); leaf 1's hypervisor-present bit (ECX 31) forced set
  and MTRR bit (EDX 12) forced clear (so guest software doesn't attempt
  MTRR MSR access this project doesn't emulate, narrowing CPUMSR-2's
  own scope); leaf 0x80000001's SVM bit (ECX 2) forced clear (this
  project doesn't emulate nested SVM for guests, so must not advertise
  it); leaf 0x40000000 (the Xen/KVM/Hyper-V/VMware hypervisor-CPUID
  convention) reports a distinct, honest "HypeHypeHype" signature, not
  pretending compatibility with any of those (that's M7-1's later,
  Windows-specific job). 100%/100%/100% region/line/branch covered.
  Exempt glue `hype_svm_vcpu_handle_cpuid()` (`svm_vcpu.c`) executes
  the real `cpuid` instruction (mirrors `cpu_features_hw.c`'s own
  `cpuid()` helper), calls `hype_cpuid_emulate()`, writes EAX/EBX/ECX/
  EDX back (zero-extended, matching CPUID's own 64-bit-mode behavior),
  advances RIP by 2 (CPUID's fixed instruction length).
  Validated end-to-end with a new synthetic long-mode test guest:
  issues real CPUID for leaves 0/1/0x40000000, stores each result into
  a host-inspectable buffer via ordinary guest-RAM writes (no MMIO/NPF
  involved), and the host independently recomputes the expected result
  via `hype_cpuid_emulate()` fed with its own real CPUID output,
  confirming a byte-for-byte match -- proving the whole VM-exit path,
  not just the pure decode logic. Clean QEMU run.*
- [x] **CPUMSR-2** — MSR intercept baseline (RDMSR/WRMSR).
  Deps: CPUMSR-1

  *Confirmed via grep that `g_msrpm` was wired into both VMCBs'
  `msrpm_base_pa` but never filled -- stayed all-zero ("intercept
  nothing"), unlike `g_iopm` (filled with 0xFF for the long-mode
  guest). Every guest RDMSR/WRMSR reached real hardware unmediated, the
  same class of gap CPUMSR-1 fixed for CPUID.
  Adds `HYPE_SVM_INTERCEPT_MSR_PROT` (bit 28 of intercept_misc1 --
  this project's own comment already documented this bit's position,
  just never defined/set it) and `HYPE_SVM_EXITCODE_MSR` (0x7C) to
  `vmcb.h`. Set in both VMCB builders; `g_msrpm` filled with 0xFF in
  both guest-create paths (`svm_vcpu.c`), mirroring `g_iopm`'s own
  existing pattern exactly -- the intercept bit alone is not
  sufficient, VMRUN always consults the bitmap to decide whether any
  *specific* MSR actually traps.
  New pure-logic module `arch/x86_64/cpu/msr_emulate.h`/`.c`
  (`hype_msr_decide()`) is a small, explicit allow-list rather than a
  full MSR emulation layer -- CPUMSR-1's leaf-1 MTRR bit is already
  forced clear specifically so well-behaved guest software never
  attempts an MTRR MSR access, narrowing what actually needs handling
  here: `APIC_BASE` (read-only, a fixed synthesized value --
  `hype_msr_apic_base_value()` -- built from `lapic.h`'s
  `HYPE_LAPIC_DEFAULT_BASE` with Global Enable/BSP bits set, matching
  M2-4's AVIC scope), `EFER` (read/write, routed directly to/from the
  VMCB's own already-tracked `save.efer` field), `TSC` (read-only,
  real `rdtsc()` plus the VMCB's own `tsc_offset` control field).
  Everything else is fail-closed, matching every other unrecognized-
  access convention already established (IOIO/NPF/CPUID) -- to be
  iterated based on what a real OVMF/GRUB/Linux boot log actually
  demands. 100%/100%/100% region/line/branch covered.
  Exempt glue `hype_svm_vcpu_handle_msr()` (`svm_vcpu.c`) decodes
  direction from EXITINFO1 bit 0 and the MSR number from RCX, executes
  a real RDTSC when needed, and returns -1 for a rejected MSR (the
  caller's job to treat as fatal).
  Validated by extending CPUMSR-1's own test guest: after its three
  CPUID leaves, it now also issues RDMSR against APIC_BASE and EFER
  (writes deliberately not exercised here -- WRMSR against a guest's
  own EFER mid-test risks destabilizing its long-mode state, not worth
  the risk for a baseline test) and stores both results into the same
  host-inspectable buffer; the host confirms APIC_BASE matches
  `hype_msr_apic_base_value()` exactly and EFER's returned value is a
  plausible 64-bit-mode EFER (SVME set). Clean QEMU run; every other
  existing test guest (M2-7 through VIDEO-2) still halts cleanly with
  both intercepts now active.*

---

## RAM — Dynamic per-VM guest RAM + NPT sizing

- [x] **RAM-1** — Allocate a real, mem_mb-sized guest RAM region via
  UEFI AllocatePages; wire ADM's already-validated mem_mb into it.
  Deps: ADM-1, M3-1

  *Confirmed via grep that neither `hype.cfg` parsing nor ADM's
  admission checks were wired into the real boot path at all before
  this -- `core/cfg.c`/`core/admission.c` existed only as standalone,
  unit-tested modules, never actually called from `boot/main.c`, and
  `mem_mb` was never used for anything except ADM's own budget-sum
  arithmetic. Scope note: this task builds and validates the
  allocation/NPT-sizing *mechanism* itself (the genuinely new
  engineering work) using a synthetic one-VM config with a fixed test
  `mem_mb` (`HYPE_RAM_1_TEST_MEM_MB` = 64), standing in for a real
  parsed config -- reading an actual `hype.cfg` from the ESP needs
  UEFI's Simple File System Protocol, which doesn't exist yet either
  (the same file-I/O gap ISO-1 will need); wiring a *real* parsed
  config in is a follow-on integration step, not this task's own scope,
  matching M4-3/M4-4/M4-5's own established "primitive now, harder
  integration later" pattern.
  What *is* real: `core/admission.c` added to the main build
  (`Makefile`) for the first time, and `hype_adm_check_memory()`
  (ADM-1, already fully unit tested) now actually runs in the real boot
  path -- against `hype_memmap_usable_bytes()`'s real computation over
  this machine's own actual UEFI memory map (`core/memmap.c`, also
  already existed but had never been consumed this way), not a
  hardcoded/assumed figure. `hype_alloc_pages_any()`
  (`AllocatePages(AllocateAnyPages)`, boot/main.c) is this project's
  first UEFI allocation with no address constraint -- correct here
  because the guest that runs inside it is long-mode (RIP addressing
  has no 32-bit segment-base truncation risk the way M2-7's real-mode
  guest does, per that guest's own real-hardware-bug comment).
  Allocation happens on the BSP before MP dispatch, same ordering as
  M2-7's own below-4GB allocation and for the same reason (Boot
  Services calls from a non-BSP AP context are untested territory this
  project has deliberately avoided so far).*
- [x] **RAM-2** — Size NPT identity mapping to the actual allocated
  region instead of a fixed blanket constant.
  Deps: RAM-1

  *`hype_ram_1_gb_to_map()` (`boot/main.c`) computes the GB count
  needed to cover a guest-physical end address by rounding up to the
  next whole GB, bounded by `HYPE_PAGING_MAX_GB` (the real compile-time
  capacity of every `g_npt_pd`/`g_guest_pd`-style array in this file) --
  fails closed (`hype_fatal()`) rather than silently overrunning a
  static array if a future, larger `mem_mb` ever needs more coverage
  than that. Both `hype_paging_build_identity()` (guest CR3) and
  `hype_npt_build_identity()` (NPT) map from GB index 0 upward, the
  same shape as every existing identity map here -- this computes *how
  many* GB that shared shape needs to reach, not a new "map only this
  region" builder.
  Validated with a new synthetic test guest whose code/stack live
  *inside* RAM-1's dynamically-allocated region (not a separate fixed
  buffer): a single HLT, deliberately trivial -- what's actually being
  proven is that the dynamically-computed NPT/guest-CR3 coverage
  genuinely reaches wherever `AllocatePages(AllocateAnyPages)` actually
  placed the allocation, the same class of real-hardware bug this
  project already found and fixed once this session (`arch/x86_64/svm/
  npt.h`'s own `HYPE_NPT_MAX_GB` comment) for a differently-sized gap
  (compiler-placed static buffers, not firmware-placed dynamic
  allocations). Clean QEMU run; every other existing test guest (M2-7
  through CPUMSR-2) still halts cleanly.*

---

## PCI — PCI configuration-space + host-bridge emulation

- [x] **PCI-1** — Minimal ECAM-based PCI host bridge + config space
  device model (ACPI MCFG already synthesized, M4-4).
  Deps: M4-4

  *Confirmed via an existing code comment (`devices/acpi.h`'s own MCFG
  field, "not yet backed by a real MMIO/PCI config-space device model")
  that this was a known, already-flagged gap -- ACPI advertised an ECAM
  window to guest firmware, but nothing responded to accesses within
  it, so no guest PCI bus driver could discover any device at all,
  including M4-5's AHCI controller.
  New `devices/pci.h`/`.c`: a small, fixed bus-0-only topology (no
  PCI-to-PCI bridges modeled -- every device presents a Type 0, not
  Type 1, header, so compliant firmware never looks for a further bus;
  single-function devices only, since the multi-function header bit is
  never set). Config-space register layout (Vendor/Device ID, Command/
  Status, Class Code, Header Type, BARs) is stable, decades-old PCI
  Local Bus Specification knowledge, not something needing external
  verification the way this session's AMD-specific VMCB work did.
  Implements the standard BAR sizing/programming protocol (write
  all-1s, read back the size mask; write a real address, read back
  masked to the BAR's own alignment) and the "absent device reads as
  all-1s" convention every real PCI bus-walk relies on to know where
  the device list ends -- confirmed this project's placeholder vendor
  ID (`HYPE_PCI_VENDOR_ID_HYPE`, not a real PCI-SIG assignment, same
  "honest, not pretending compatibility" choice as CPUMSR-1's
  "HypeHypeHype" CPUID signature) has no effect on real driver
  compatibility, since AHCI-class drivers bind on class code, not
  vendor/device ID. 100%/100%/96.88% region/line/branch covered.
  Exempt glue `hype_svm_vcpu_handle_pci_ecam_npf()` (`svm_vcpu.c`)
  reuses M4-3/M4-5's exact NPF/`hype_mmio_decode()` mechanism (ECAM is
  accessed via ordinary MOV instructions) -- unlike every other NPF
  handler here, `hype_pci_config_read()`/`_write()` always succeed
  (config-space accesses architecturally never fault), so this handler
  has no "unrecognized access" failure mode beyond the instruction
  itself failing to decode.
  Validated with a synthetic long-mode test guest: reads a registered
  host bridge's vendor/device ID and class code, probes an absent
  device (confirms the all-1s convention), and runs the full BAR
  sizing/programming protocol against a fake AHCI-class device's BAR0
  -- all via real ECAM MMIO accesses through the actual VM-exit path.
  Found and fixed one real bug this way: the test payload's original
  BAR-write instructions used `mov dword [mem], imm32` (opcode 0xC7,
  immediate-to-memory), a form `hype_mmio_decode()` was never built to
  support (every other test guest here loads a register first, then
  stores it) -- fixed the test payload to match that existing
  convention rather than extending the decoder. Clean QEMU run; every
  other existing test guest (M2-7 through RAM-2) still halts cleanly.*
- [x] **PCI-2** — Expose the existing AHCI device (M4-5) as a
  discoverable PCI function (vendor/device ID, class code, ABAR as
  BAR5) instead of a fixed guest-physical address.
  Deps: PCI-1, M4-5

  *The genuinely hard part flagged when this was scoped out of M4-6:
  real PCI enumeration means the guest *chooses* a device's MMIO
  address itself (via BAR sizing/programming), not something the host
  can hardcode ahead of time -- unlike every other MMIO device in this
  project (pflash/AHCI/PCI's own ECAM window), which all live at a
  fixed, host-picked guest-physical address whose NPT entry is marked
  not-present once, up front. Solved with a *reactive* NPT update: the
  test's own dispatch loop watches every PCI config-space write PCI-1's
  handler processes, and the moment one results in
  `hype_pci_memory_space_enabled()` becoming true with a nonzero
  `hype_pci_get_bar_value()` (BAR5), it calls
  `hype_npt_mark_not_present()` right then, at whatever address the
  guest just chose -- mapping the device's MMIO window into existence
  only once the guest has actually finished enumerating it, exactly
  when real hardware would start decoding it too.
  Found and fixed one real, if currently untriggered-by-this-test's-own
  addresses, bug while building this: `hype_svm_vcpu_handle_pci_ecam_npf()`
  (PCI-1) only checked a *lower* bound on the ECAM region -- harmless
  with one NPT-trapped region active, but wrong once a second one (a
  device's dynamically-BAR-programmed window) can coexist; fixed by
  checking both bounds (`HYPE_PCI_ECAM_BUS0_SIZE`, devices/pci.h).
  Validated with a synthetic test guest playing the role a real PCI bus
  driver would: programs AHCI's BAR5 with an address deliberately
  different from the old fixed `HYPE_M4_5_AHCI_GPA` (proving this is
  genuinely dynamic, not incidentally the same constant), sets
  Command.Memory Space Enable, then runs M4-5's own already-proven
  AHCI/ATAPI register setup + READ(10) sequence unchanged, just
  retargeted to the newly-discovered address. Clean QEMU run on the
  first attempt; every other existing test guest (M2-7 through PCI-1)
  still halts cleanly.*

---

## FW — Real OVMF firmware boot wiring

**Note (2026-07-15): FW-* needs real-hardware validation with durable
debug logging, once M5 (storage) exists.** This session's FW-1 work was
debugged entirely in a nested-SVM dev environment (this project's own
hype.efi running as an L1 hypervisor under an outer QEMU/KVM host,
itself launching real OVMF as an L2 guest) -- a setup that could have
its own artifacts not present on bare metal (matching M2-8's own
precedent: "two real, non-obvious bugs found [on real hardware], neither
reproducible under [nested virtualization]"). Screen-only debug
checkpoints (`hype_debug_print()`, per M2-8's `tools/make-usb-package.sh`
precedent) are workable for simple pass/fail milestones, but FW-1's own
remaining blocker needed dozens of iterative, detailed diagnostic dumps
(NPF/exception state, raw instruction bytes, stack walks,
`addr2line`-correlated addresses) -- entirely impractical to read off a
screen with no scrollback and no persistent record. Once M5 lands a real
`blk_backend`, FW-* real-hardware validation should write its debug log
directly to disk -- e.g. appending to a file on the FAT32 EFI System
Partition of the same USB drive `tools/make-usb-package.sh` already
builds -- giving a durable, retrievable log from real hardware the same
way `/tmp/qemu_*.log` capture already does for this dev environment.
Revisit this note when picking FW-1 back up.

- [ ] **FW-1** — New "firmware guest" VMCB builder: real x86
  reset-vector convention, executing directly from OVMF_CODE.fd mapped
  as ordinary executable NPT-backed guest memory (not the pflash
  MMIO-trap model, which stays correct for OVMF_VARS.fd only).
  Deps: RAM-2, CPUMSR-2, M4-2, M4-3

  **PARKED 2026-07-15** -- substantial progress made (see below), but
  the remaining blocker (a NULL-pointer fault in a dynamically-
  relocated DXE driver) needs live debugging or a different
  investigative angle, not more of the same source-correlation
  technique. User decision: move on to INPUT, then VIDEO-3, then M5,
  and circle back to FW-1/FW-2/M4-6 later. Nothing here needs
  unwinding -- every fix committed so far (file I/O, PCI CF8/CFC, ACPI/
  fw_cfg, CMOS, ACPI PM Timer) is real, independently correct
  infrastructure, not a throwaway experiment.

  *Substantial progress, not yet complete -- real OVMF now executes
  correctly through real-mode -> protected-mode -> long-mode with
  paging enabled, several real, independently-valuable bugs found and
  fixed along the way, but a guest-internal #PF (destination address 0
  in a CopyMem-style bulk copy, deep in SEC-phase C code) still blocks
  full boot. In order found:*
  - *`core/file_io.h/.c` (new): reads OVMF_CODE.fd/OVMF_VARS.fd from
    the same ESP hype.efi was booted from, via
    EFI_LOADED_IMAGE_PROTOCOL + EFI_SIMPLE_FILE_SYSTEM_PROTOCOL +
    EFI_FILE_PROTOCOL. 100%/100%/100% unit-tested via fake protocol
    structs (`core/tests/test_file_io.c`).*
  - ***Bug (transcription)***: `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` in
    `core/efi_types.h` was missing its leading `UINT64 Revision;`
    field, so `OpenVolume()` called through the wrong byte offset --
    corrupted the *host* (outer) firmware's own memory and crashed it.
    Caught via a real host-firmware `#PF` crash dump, fixed by adding
    the field.
  - **`hype_npt_map_range()`** (new, `arch/x86_64/svm/npt.h/.c`):
    remaps an arbitrary guest-physical range to a *different*
    host-physical range (non-identity), needed because the reset
    vector's guest-physical address (top of 4GB) is NOT safe to
    identity-map -- that literal host-physical range is where the
    outer/host firmware's own real flash lives on this nested-SVM dev
    box (and would be real BIOS/UEFI flash on bare metal). Both bases
    must be 2MB-aligned (this project's NPT/paging is 2MB-PS-only, no
    4KB level). 100%/100%/100% unit-tested.
  - ***Bug (alignment)***: the combined VARS+CODE buffer was allocated
    via a plain `AllocatePages(AllocateAnyPages, ...)`, which UEFI only
    guarantees 4KB-aligned, not the 2MB alignment `hype_npt_map_range`
    requires for its PS=1 PDEs. A misaligned host-phys base leaves
    garbage in the PDE's reserved low bits -- hardware reports this as
    a *permission violation* NPF (EXITINFO1 bit0=1, "entry present")
    on the very first guest fetch, not a not-present fault, which is
    actively misleading. Fixed by adding
    `hype_alloc_pages_any_2mb_aligned()` (over-allocates by up to one
    extra 2MB granule, returns the aligned address within it).
  - ***Bug (guest isolation)***: `hype_vmcb_build_realmode_guest()`
    never set `HYPE_SVM_INTERCEPT_IOIO_PROT`, and
    `hype_svm_vcpu_create()` never filled `g_iopm` -- every prior
    real-mode test guest never executed I/O, so this went unnoticed,
    but real OVMF does real port I/O (A20 gate, CMOS, fw_cfg, legacy
    PCI, ...) immediately. Without this, that I/O reaches real host
    hardware completely unmediated -- the exact same class of gap
    CPUMSR-1/2 fixed for CPUID/MSR, now extended to IOIO for the
    real-mode path too (matching M3-5's long-mode path, which already
    had it). Also added `hype_svm_vcpu_handle_unknown_ioio()` (a safe
    generic default -- IN reads back all-1s, matching devices/pci.h's
    own "absent device" convention; OUT is dropped) since real OVMF
    probes far more ports than the PIC/PIT allow-list covers.
  - ***Bug (guest isolation, diagnostic)***: no exception vector was
    ever intercepted (`intercept_exceptions` was always 0). An
    unhandled guest exception (e.g. #GP) was silently delivered to the
    guest's own IDT instead of exiting to us -- real EDK2 firmware's
    own early exception handlers frequently just spin forever
    (`CpuDeadLoop()`-style) rather than crash, which is
    indistinguishable from a genuine hang with no further VM-exits at
    all. Fixed by intercepting all 32 vectors (`0xFFFFFFFF`) and
    reporting vector/error-code/CS/CR0/CR2/CR3/RIP/RSP on any hit.
  - ***Bug (reset-vector convention)***: this project's established
    "CS.base = full reset address, RIP = 0" convention (correct for
    every hand-written synthetic real-mode test guest since M2-7) does
    NOT match real x86 hardware reset state (`CS.base=0xFFFF0000,
    RIP=0xFFF0` -- same resulting linear address, `0xFFFFFFF0`, but a
    different RIP). Real firmware's own ResetVector code depends on RIP
    starting near the *top* of its 16-bit range: its own first jump is
    a small backward/negative displacement, which stays positive from
    RIP=0xFFF0 but *underflows* 16-bit arithmetic into a huge positive
    offset from RIP=0, exceeding the real-mode CS limit (0xFFFF) and
    raising #GP. Confirmed empirically (guest faulted at RIP=0x10000,
    exactly one past the limit, on the very first instruction). Fixed
    by adding `hype_svm_vcpu_set_rip()` and using the genuine hardware
    convention for FW-1 specifically (every other real-mode test guest
    is untouched, still using RIP=0 deliberately).
  - ***Bug (CPUID under-reporting)***: `hype_cpuid_emulate()` reported
    max extended leaf `0x80000001`, so real firmware skips querying
    leaf `0x80000008` (physical/linear address widths) entirely,
    falling back to a hardcoded default. Fixed by bumping the
    max-extended-leaf report to `0x80000008` and passing that leaf
    through from the real host value (address width isn't
    guest-isolation-sensitive, only correctness-sensitive here).
    Confirmed via unit test; did NOT resolve the current blocking #PF
    on its own, but is a real, independently-valuable correctness fix
    kept regardless.
  - **Root-caused and fixed the NULL-`CopyMem` #PF above**, using the
    vendored `/edk2` submodule's own surviving build artifacts
    (`edk2/Build/OvmfX64/RELEASE_CLANGDWARF/`, including per-module
    `.map`/`.debug` files with full DWARF info -- `addr2line`/`nm`
    against `OvmfPkg/PlatformPei/PlatformPei/DEBUG/PlatformPei.debug`
    symbolized the fault's RIP to `InternalMemCopyMem` and its caller to
    `InitializePlatform` (`OvmfPkg/PlatformPei/Platform.c:135`, LTO-
    inlined `MicrovmInitialization()`). Root cause: this project had
    **no PCI configuration-space emulation wired up for FW-1 at all**
    (PCI-1/PCI-2's own `devices/pci.h` model was never registered for
    this guest) -- OVMF's `PlatformPei` reads the host bridge's device
    ID via the *legacy* 0xCF8/0xCFC ports (confirmed via source:
    `OvmfPkg/PlatformPei/Platform.c:346`,
    `PciRead16(OVMF_HOSTBRIDGE_DID)`), well before ACPI's MCFG table --
    and by extension ECAM -- would even be parsed. With those ports
    unhandled, they hit the existing generic
    `hype_svm_vcpu_handle_unknown_ioio()` absorb-as-all-1s default,
    making OVMF read the host bridge device ID back as `0xFFFF` --
    which OVMF's own platform detection treats as the QEMU "microvm"
    machine-type sentinel (`HostBridgeDevId == 0xffff`), sending it down
    a completely different, fw_cfg-FDT-based init path this project
    doesn't support, eventually crashing in `MicrovmInitialization()`'s
    own `CopyMem(NewBase, EmptyFdt, FdtSize)` once its own
    `AllocatePages()`-derived `NewBase` came out wrong. Confirmed via
    the full unhandled-port log: repeated `0xcf8`/`0xcfe` accesses
    (interleaved with `0x70`/`0x71` CMOS and `0x510`/`0x511` fw_cfg,
    also unhandled) right before every fault.
    - **Fix**: `devices/pci.h/.c` gained legacy CF8/CFC support --
      `hype_pci_cf8_write()`/`_read()` (stores/reads the selected
      config address, now a field of `hype_pci_t` itself, matching how
      `devices/fw_cfg.h`'s own protocol state is kept internally),
      `hype_pci_decode_cf8_address()` (pure bit extraction: bits 23:16
      = bus, 15:11 = device, 10:8 = function, 7:2 = dword-aligned
      register), and `hype_pci_cf8_config_read()`/`_write()` (composes
      the decode with the already-tested `hype_pci_config_read()`/
      `_write()`, plus a `byte_offset` for CFD/CFE/CFF sub-accesses).
      100%/100%/96.88% unit tested (`core/tests/test_pci.c`).
    - Exempt glue: `hype_svm_vcpu_handle_pci_cf8_ioio()`
      (`arch/x86_64/svm/svm_vcpu.c`) -- width-aware (1/2/4-byte) IOIO
      dispatch to the CF8/CFC ports, reusing
      `hype_mmio_merge_read_value()`/`hype_mmio_extract_write_value()`
      for RAX merge/extract (unlike `hype_svm_vcpu_handle_ioio()`'s
      PIC/PIT, always 8-bit).
    - `boot/main.c`'s `run_fw_1_test()` now registers a real host bridge
      (Intel Q35 MCH, vendor `0x8086`/device `0x29C0` --
      `INTEL_Q35_MCH_DEVICE_ID`, transcribed from
      `edk2/OvmfPkg/Include/IndustryStandard/Q35MchIch9.h`) via
      `hype_pci_add_device()`, and wires
      `hype_svm_vcpu_handle_pci_cf8_ioio()` into the dispatch loop ahead
      of the generic absorb-unknown fallback.
    - ***Bug (self-inflicted, in this session's own diagnostic code)***:
      the stack-caller-byte-dump diagnostic added earlier blindly
      dereferenced `stack[2]` assuming it held a plausible return
      address (true for the temp-RAM-window fault, which happened to
      have the `0x5AA5...` fill marker validating it) -- a *different*
      fault (this one) had a genuinely fresh, zeroed stack slot,
      making the dereference read host address `-8`, corrupting *this
      project's own* host-side execution (a raw host-pointer read from
      L1's own context, not the guest's). Fixed by guarding on
      `stack[2] != 0 && stack[2] < 4GB` before dereferencing.
  - **Investigated and fixed the follow-on #PF at `rip=0xE0006`** above.
    Ruled out a legacy VGA-BIOS/option-ROM shadow-scan false positive
    first (a pre-launch diagnostic dump found no `0x55 0xAA` signature
    anywhere near `0xC0000`/`0xE0000`). The real cause, found via the
    same source-level method that solved the microvm bug: this
    project's vendored OVMF had **no fw_cfg device registered for FW-1
    at all** -- ACPI content (RSDP/XSDT/FADT/MADT/MCFG/DSDT,
    `devices/acpi.h`, already built and proved working by M4-4) was
    never exposed via fw_cfg for this guest, and OVMF's own memory-size
    fallback (`OvmfPkg/Library/PlatformInitLib/MemDetect.c`,
    `PlatformGetSystemMemorySizeBelow4gb()`) reads CMOS registers
    0x34/0x35 when fw_cfg's "etc/e820" file isn't present -- also
    unhandled, also absorbed as all-1s. Both gaps are now fixed:
    - `run_fw_1_test()` now builds real ACPI content
      (`hype_acpi_build_tables_blob()`/`_build_rsdp()`/
      `_loader_build_script()`, exactly M4-4's own already-tested
      functions) and registers it with a real `hype_fw_cfg_t`
      (`g_fw_1_fw_cfg`), wiring `hype_svm_vcpu_handle_fw_cfg_ioio()`
      into the dispatch loop.
    - **New device model**: `devices/cmos.h/.c` -- a minimal CMOS/RTC
      register file (128 bytes, index/data port pair at 0x70/0x71,
      bit 7 of the index write masked off as the conventional
      NMI-disable bit, not a register-select bit). Only registers
      0x34/0x35 (system memory above 16MB, in 64KB units) are ever
      given a meaningful value -- computed from the *actual* usable
      RAM this machine has (`hype_memmap_usable_bytes()`'s own result,
      already computed for RAM-1's admission check, now also kept in
      `g_usable_ram_bytes`), not a guessed constant, the same
      "reflect reality" principle RAM-1 established. 100%/100%/100%
      unit tested (`core/tests/test_cmos.c`). Exempt glue:
      `hype_svm_vcpu_handle_cmos_ioio()`.
    - Confirmed fixed: the exact `rip=0xE0006` #PF no longer occurs at
      all with these two fixes in place -- the guest now runs well
      past that point.
  - **Root-caused and fixed the follow-on port-`0x6` infinite poll**
    above too -- it turned out to be the exact same class of bug as the
    microvm misdetection, one PCI device earlier in the chain. Symbolized
    via `nm`/`addr2line` against `edk2/Build/.../MdeModulePkg/Core/Pei/
    PeiMain/DEBUG/PeiCore.debug`: the polling RIP resolved to
    `IoRead32()` called from `OvmfPkg/Library/AcpiTimerLib/
    BaseAcpiTimerLib.c`'s `GetPerformanceCounter()`. That library
    computes the ACPI PM Timer's I/O port as
    `(PciRead32(Pmba) & ~PMBA_RTE) + ACPI_TIMER_OFFSET(8)`, where `Pmba`
    is the ICH9 LPC bridge's own PCI config register (bus 0/device
    0x1f/function 0, offset `0x40`) -- a **second** PCI device this
    project had never registered. Reading it returned the absent-device
    default `0xFFFFFFFF`; `& ~PMBA_RTE(1)` = `0xFFFFFFFE`; `+ 8`,
    truncated to `UINT32`, = exactly `0x00000006` -- confirmed via the
    live trace, byte-for-byte matching the observed port.
    `OvmfPkg/Library/PlatformInitLib/Platform.c` confirmed the
    *intended* fix path already exists in OVMF itself:
    `PciAndThenOr32(Pmba, ...)` programs a real value into this
    register during early boot -- but since device 0x1f didn't exist in
    this project's PCI model, that write was silently dropped
    (`hype_pci_config_write()`'s own documented "write to an absent
    device is dropped" behavior), so the later read still saw
    `0xFFFFFFFF`.
    - **Fix**: `run_fw_1_test()` now also registers the ICH9 LPC bridge
      (device 0x1f, a real Intel device ID) via `hype_pci_add_device()`
      -- with the device present, `PciAndThenOr32()`'s write actually
      lands, and the later read gets back the real, intended
      `ICH9_PMBASE_VALUE(0x600) + ACPI_TIMER_OFFSET(8) = 0x608` --
      confirmed via the live trace: the poll port changed from `0x6` to
      exactly `0x608` after this fix alone, before the timer itself was
      even implemented.
    - Port `0x608` still needed an actual monotonically-increasing
      value (the all-1s absorbed default can never satisfy a
      calibration/stall loop waiting for time to pass). New exempt
      handler `hype_svm_vcpu_handle_acpi_pm_timer_ioio()`
      (`arch/x86_64/svm/svm_vcpu.c`) returns a real `RDTSC` read masked
      to 24 bits (this project's own FADT, `devices/acpi.c`, never sets
      the `TMR_VAL_EXT` flag, so the guest itself expects a 24-bit, not
      32-bit, counter -- kept consistent with what we actually tell it).
    - Confirmed fixed: the port-`0x6`/`0x608` poll is completely gone;
      the guest now runs well past PEI, into genuine DXE-phase driver
      execution (confirmed via `rip` values in the hundreds-of-MB range,
      far beyond any PEI/DXE FV's own "Fixed Flash Address" range).
  - **Current blocker (new, after the PM Timer fix)**: a new #PF --
    this time a *read* (`err=0x0`: not-present, read, supervisor) from
    guest-linear address 0 again (`CR2=0`), at `rip` in the hundreds-of-
    MB range. The instruction bytes decode as `cmp ebp, [rbx]` with
    `rbx=0` -- a genuine NULL-pointer read, somewhere in a DXE driver.
    This is the first fault this session that the established
    `nm`/`addr2line`-against-`edk2/Build` technique **cannot resolve**:
    the address doesn't fall within any FV's own statically-listed
    "Fixed Flash Address" module range (`DXEFV.Fv.map` has none at this
    address) -- DXE Core dynamically loads and relocates most drivers
    at runtime-chosen addresses that aren't known ahead of time from
    the static build artifacts alone, unlike every earlier PEI-phase
    fault this session (PEI's own modules ARE all fixed-address,
    hence resolvable). Making further progress here needs either live
    debugging (not straightforward against a nested-SVM L2 guest in
    this environment) or a differently-targeted investigation (e.g.
    identifying which DXE driver dispatches around this point in boot
    and reasoning about what it reads from a NULL/uninitialized
    pointer -- possibly another not-yet-registered device, following
    the same pattern as PCI/fw_cfg/CMOS/ACPI-timer above).
  - New diagnostic accessors added for this investigation, all exempt
    from unit testing (same reasoning as every other
    `hype_svm_vcpu_handle_*` glue function): `hype_svm_vcpu_get_last_npf()`,
    `hype_svm_vcpu_get_debug_state()` (CS/CR0/CR2/CR3/RIP/RFLAGS/RSP),
    `hype_svm_vcpu_set_rip()`, `hype_svm_vcpu_handle_unknown_ioio()`,
    `hype_svm_vcpu_handle_pci_cf8_ioio()`, `hype_svm_vcpu_handle_cmos_ioio()`,
    `hype_svm_vcpu_handle_acpi_pm_timer_ioio()`.

- [ ] **FW-2** — Load OVMF_CODE.fd/OVMF_VARS.fd from fw/ into guest RAM
  at boot, replacing the fixed hand-written test-guest entry points
  used through M4-5/VIDEO-2. (File loading itself is already done as
  part of FW-1's own work above -- this task now just tracks the
  remaining "guest actually boots" milestone once FW-1's blocker is
  resolved.)
  Deps: FW-1

---

## ISO — Real installer media loading

- [x] **ISO-1** — Read a real installer ISO from the same ESP hype.efi
  was booted from, via UEFI's own Simple File System Protocol (Boot
  Services, before ExitBootServices) -- does not need M5.
  Deps: none new

  *Reused FW-1's own `core/file_io.h` unchanged (already generic, not
  OVMF-specific -- its own header comment already anticipated this).
  `Makefile`'s new `TEST_ISO` variable (default
  `/usr/share/edk2/ovmf/UefiShell.iso`, the same edk2-ovmf package's
  own real ~2.8MB bootable ISO9660 image -- not vendored into this
  repo, just copied onto the ESP at test time, matching how the
  outer/host OVMF_VARS.fd is handled) is copied to `\iso\test.iso` by
  the `run` target. `efi_main()` reads it via
  `hype_file_locate_root()`/`_get_size()`/`_read_into()`, then verifies
  the read is real (not a short read that happened to report success)
  by checking for ISO9660's own "CD001" Primary Volume Descriptor
  standard identifier (ECMA-119 SS7.1.1, always at byte offset 32769)
  in the actual bytes read back. Clean QEMU run on the first attempt:
  "iso-1: read a real 2895872-byte ISO9660 image ... "CD001" identifier
  verified at offset 32769" -- every other existing test guest (M2-7
  through PCI-2) and FW-1's own progress point are both unaffected.*
- [x] **ISO-2** — Back M4-5's existing AHCI/ATAPI in-memory model with
  the real loaded ISO buffer instead of a synthetic one.
  Deps: ISO-1, M4-5

  *A new, dedicated test guest (`run_iso_2_test()`, boot/main.c) --
  structurally an exact copy of M4-5's own test (same payload template,
  same fixed-AHCI-address convention; PCI discovery stays PCI-2's own
  separate concern) -- but `hype_atapi_reset()` is backed by ISO-1's
  real loaded `\iso\test.iso` buffer instead of a synthetic pattern.
  Reads LBA 16 (the ISO9660 Primary Volume Descriptor sector, always
  the 17th 2048-byte sector per ECMA-119 §8.4) via the guest's own
  AHCI/ATAPI READ(10) command, then verifies both a byte-for-byte match
  against the real file's own content at that exact offset *and* the
  "CD001" identifier at the sector's own byte offset 1 -- the same
  signature ISO-1 already verified via direct UEFI file I/O, now
  confirmed reachable through the emulated AHCI/ATAPI hardware path
  too. Clean QEMU run on the first attempt (after fixing the test
  dispatch order -- `run_fw_1_test()` currently ends in a `hype_fatal()`
  that never returns, so `run_iso_2_test()` must run *before* it in
  `run_all_test_guests()`, not after): "iso-2: AHCI/ATAPI test guest
  halted cleanly ... real ISO LBA 16 read byte-for-byte via emulated
  hardware, "CD001" identifier verified" -- every other existing test
  guest (M2-7 through PCI-2) and FW-1's own progress point are all
  unaffected.*

---

## NET — Networking (plan.md §6e, §10 decision #9 — required)

- [ ] **NET-1** — Host NIC driver, e1000/e1000e-class first target.
  Deps: M3-1
- [ ] **NET-2** — virtio-net guest-facing device (Linux/BSD default).
  Deps: NET-1, VALID-2
- [ ] **NET-3** — Emulated e1000-compatible NIC guest-facing device
  (Windows path, mirroring the AHCI-for-Windows split).
  Deps: NET-1, VALID-1
- [ ] **NET-4** — Basic host-level NAT for guest outbound connectivity,
  guest→WAN + established-return only.
  Deps: NET-1
- [ ] **NET-4a** — Guest-to-guest isolation by default: each guest's
  virtual NIC in its own segment, never a shared L2/broadcast domain with
  another guest's — prevents *accidental* guest-to-guest communication
  (security review finding, §10 decision #21).
  Deps: NET-4
- [ ] **NET-4b** — `net_peers` opt-in: narrow host-mediated forwarding rule
  between exactly the VM pairs an operator explicitly names, without
  opening a general shared broadcast domain — deliberate guest-to-guest
  connectivity is a supported use case, just never the default.
  Deps: NET-4a, ADM-5
- [ ] **NET-5** — `net_mode`/`net_peers` config wiring (`none` default /
  `nat`, optional `net_peers` list).
  Deps: NET-2, NET-3, NET-4b, M1-1

---

## M5 — virtio-blk/AHCI + full install (plan.md §9 M5)

- [x] **M5-1** — virtio-blk guest-facing device.
  Deps: M4-5

  *A modern (non-transitional, virtio 1.x) virtio-blk PCI device --
  what a real Linux/BSD guest's own inbox virtio_blk driver discovers
  and drives, structurally unrelated to M4-5's AHCI/ATAPI transport
  (no SATA/ATA layer at all). PCI identity (vendor 0x1AF4/device
  0x1042/class 0x01/0x00/0x00 -- QEMU's own real convention, not the
  more generic 0x0180 a naive spec reading might suggest), the
  virtio-pci capability structure, common-config register layout,
  device-status handshake, virtqueue wire format, and virtio_blk_req
  layout were fetched and confirmed against the real OASIS VIRTIO v1.1
  spec plus the Linux kernel's own headers and QEMU's source, not
  reconstructed from memory -- same discipline as every other
  wire-format struct here.

  New `devices/virtio_blk.h/.c`: a pure common-cfg/device-cfg register
  model (each register enforcing its own real access width),
  `hype_virtio_blk_is_queue_ready()`, and `hype_virtq_decode_desc()`
  (pure virtq_desc bit extraction). Offers zero optional
  `VIRTIO_BLK_F_*` feature bits -- only `VIRTIO_F_VERSION_1` -- which
  the real Linux driver source confirms is sufficient to probe/bind
  (every optional feature's absence is an already-handled fallback).
  100% line, 99.01% branch coverage (`core/tests/test_virtio_blk.c`,
  19 tests, including a data-driven sweep closing the "wrong access
  width" branch for every one of the ~19 registers rather than
  duplicating near-identical tests by hand).

  Scoped to exactly one virtqueue and exactly 3 descriptors per chain
  (header/one data segment/status -- no scatter-gather across
  multiple data descriptors), mirroring AHCI's own "single ATAPI
  device, one command at a time" scope-narrowing. Exempt glue
  `hype_svm_vcpu_handle_virtio_blk_npf()` (`arch/x86_64/svm/
  svm_vcpu.c`) NPT-traps only the single MMIO BAR (all four
  capability regions -- COMMON_CFG/NOTIFY_CFG/ISR_CFG/DEVICE_CFG --
  live in one BAR at fixed sub-offsets, this implementation's own
  choice since the spec doesn't mandate a layout); a NOTIFY write
  walks the virtqueue via a private `process_virtio_blk_queue()`
  helper, draining every newly-avail chain since the device's own
  internal `last_avail_idx` bookkeeping (a real device keeps the
  equivalent privately too -- not part of the wire format).

  A real virtio-pci capability list (spec §4.1.4, 4 capabilities +
  the NOTIFY one's own trailing `notify_off_multiplier`) is
  faithfully constructed in the test's PCI config-space bytes -- not
  walked by the synthetic test guest itself (which targets the known
  BAR4 offset directly, the same "test guest knows the device's own
  structure" convention PCI-2/VIDEO-3 already established), but built
  correctly so a real guest OS driver's own generic capability walk
  would find it.

  New synthetic test guest `run_m5_1_test()` (`boot/main.c`) is
  structurally different from every earlier one here: the
  virtqueue's own descriptor table/avail ring/used ring and both
  requests' header/data/status buffers are pre-built by HOST-side C
  code, not the guest's own instruction stream -- mirroring how a
  real device's DMA engine reads/writes guest memory independently of
  the guest CPU, so the guest payload only ever touches PCI/MMIO
  registers (every access NPF-routed). Exercises both directions in
  one run: a WRITE (`VIRTIO_BLK_T_OUT`) persisting a guest-supplied
  pattern to a fabricated sector, and a READ (`VIRTIO_BLK_T_IN`)
  delivering a host-pre-placed pattern from a different sector back
  to the guest -- both chains queued before a single NOTIFY kick
  (proving the device drains every new avail entry per notify, not
  just one), the guest polling the used ring until both complete.
  Feature negotiation is a genuine read-then-accept (not a blindly
  assumed value): the guest reads `device_feature`, then writes that
  exact value back as `driver_feature`.

  **Clean first-attempt QEMU run** -- both requests round-tripped
  correctly (backing-store bytes match the guest's write pattern
  byte-for-byte; the guest's own read buffer matches the host's
  pre-placed pattern byte-for-byte; both status bytes read
  `VIRTIO_BLK_S_OK`). A real host-file-backed store is explicitly
  M5-3's own job ("blk_backend") -- this task's backing store is a
  fixed in-memory buffer, matching M4-3 pflash's own "primitive now,
  integration later" precedent.*
- [ ] **M5-2** — AHCI guest-facing device.
  Deps: M4-5
- [ ] **M5-3** — `blk_backend` vtable + file-backed implementation (§6d),
  with guest LBA/length bounds-checking against the backing file's actual
  size per VALID-3.
  Deps: M5-1, M5-2, VALID-3
- [ ] **M5-4** — `/tools` disk-image prep script (`target_disk_size_gb`
  handling).
  Deps: M5-3
- [ ] **M5-5** — Full unattended Linux install to a virtual disk (needs
  network for netinst package fetch).
  Deps: M4-6, M5-3, M5-4, NET-5
- [ ] **M5-6** — Reboot into the installed OS (`boot = disk` two-phase
  flip, §6d).
  Deps: M5-5

---

## M6 — BSD guest (plan.md §9 M6)

- [ ] **M6-1** — FreeBSD-specific ACPI/loader quirk fixes.
  Deps: M5-6, M4-4
- [ ] **M6-2** — FreeBSD installer boot + install.
  Deps: M6-1, M5-3

---

## M7 — Windows guest (plan.md §9 M7, §10 decision #2)

- [ ] **M7-1** — Full Hyper-V-compatible CPUID/MSR leaf set
  (`0x40000000`–`0x40000006` + synthetic MSRs).
  Deps: M2-1
- [ ] **M7-2** — Windows AHCI/NVMe storage path validated for installer use.
  Deps: M5-2, M7-1
- [ ] **M7-3** — Windows GUI install path: PS/2 keyboard + mouse, VGA/VBE
  display adapter.
  Deps: INPUT-1, INPUT-2, VIDEO-3
- [ ] **M7-4** — Windows Setup boot + full install.
  Deps: M7-2, M7-3, M4-3, M4-4

---

## M8 — Multi-VM concurrency, dashboard, lifecycle control, fault isolation
## (plan.md §9 M8, §6b, §6f, §6g, §10 decisions #11/#12)

- [ ] **M8-1** — Dashboard rendering: per-VM name/os_hint/state/vCPU
  utilization/memory/uptime/boot-media list.
  Deps: VIDEO-1
- [ ] **M8-2** — Per-vCPU stats collection (exit counts, `HLT` time,
  last-scheduled timestamp) feeding the dashboard.
  Deps: M2-5
- [ ] **M8-3** — Dashboard navigation via the leader chord (toggle, jump to
  VM N, cycle prev/next).
  Deps: INPUT-4, M8-1
- [ ] **M8-3a** — Input exclusivity while the dashboard has focus: every
  keystroke consumed by the dashboard, explicit focus-owner check, zero
  forwarding to any guest (including whichever VM had focus immediately
  before) until focus is explicitly switched back (security review
  finding, §10 decision #22).
  Deps: M8-3
- [ ] **M8-4** — VM lifecycle: **Start** (fresh boot, zeroed RAM per M2-6).
  Deps: M2-6, M6-2, M7-4
- [ ] **M8-5** — VM lifecycle: **Stop**/Resume (pause vCPU(s) in place,
  retain RAM/device state).
  Deps: M2-5
- [ ] **M8-6** — VM lifecycle: **Shutdown** (emulated ACPI power-button GPE,
  guest-driven S5, bounded timeout).
  Deps: M4-4, M8-4
- [ ] **M8-7** — VM lifecycle: **Force power off** (immediate teardown).
  Deps: M8-4
- [ ] **M8-8** — Per-vCPU watchdog: detect a genuinely faulted guest
  (triple fault / unrecognized VM-exit storm) and auto-apply Force power
  off to that VM only. Note: this is a liveness/hang detector, not a
  substitute for VALID-1..4's input validation — it does not catch
  memory-safety violations in device emulation.
  Deps: M2-5, M8-7
- [ ] **M8-9** — Run Windows + Linux + BSD concurrently; console-switch via
  dashboard; confirm EPT/NPT isolation and pinning hold; confirm stats
  match reality.
  Deps: M8-3, M8-5, M8-6, M8-8, ADM-3
- [ ] **M8-10** — Real-hardware validation (Intel + AMD).
  Deps: M8-9, SETUP-3, SETUP-4

---

## TERM — GOP-rendered VM management terminal (v1; new milestone,
## user request 2026-07-15 — complements M8's own GUI dashboard, §6b)

VM management (not just status viewing) via two GOP-rendered surfaces:
the existing GUI dashboard (M8-1, LVGL-class per §6b) for visual
navigation, **plus** an emulated text terminal with a basic command
set for the same lifecycle actions M8-4..M8-8 already define. Explicitly
**GOP-only for v1** — no serial or network exposure, matching §6b's own
"local-only for v1" decision. SSH-based remote management is explicitly
a v2 feature (see `V2-MGMT-1` below), not started now.

- [ ] **TERM-1** — GOP-rendered text terminal surface: a monospace
  character-grid renderer on the real host screen (reuses VIDEO-1's own
  bitmap-font primitives, `core/gop_text.c`, rather than the "corporate"
  LVGL-class dashboard renderer), with input from the host's own PS/2
  keyboard (INPUT-3) and shown/dismissed via the leader chord (INPUT-4).
  Deps: VIDEO-1, INPUT-3, INPUT-4
- [ ] **TERM-2** — Basic command parser + command set operating on the
  same per-VM state M8's dashboard reads/controls: list VMs, show
  per-VM status (M8-2's stats), start/stop/pause/resume/shutdown/force
  power off a VM by name, switch console focus.
  Deps: TERM-1, M8-2, M8-4, M8-5, M8-6, M8-7
- [ ] **TERM-3** — Wire the terminal in as an alternate view alongside
  the GUI dashboard, both reachable via the same leader-chord toggle
  (M8-3). Exact UX split (two separate views vs. one primary with the
  other as fallback) is undecided — revisit when this is actually
  scoped/started.
  Deps: M8-3, TERM-1

---

## M9 — Persistence & host power lifecycle
## (plan.md §9 M9, §6h, §10 decision #13)

- [ ] **M9-1** — Verify varstore persistence survives a host reboot.
  Deps: M4-3
- [ ] **M9-2** — Host shutdown/reboot sequence: graceful Shutdown across
  all running guests in parallel, bounded per-VM timeout, Force power off
  fallback.
  Deps: M8-6, M8-7
- [ ] **M9-3** — Persist a run/stopped state record (ESP, alongside
  `hype.cfg`) as part of the shutdown sequence.
  Deps: M9-2
- [ ] **M9-4** — On hypervisor startup, read the state record and
  auto-Start every VM that was previously running.
  Deps: M9-3, M8-4
- [ ] **M9-5** — Reboot the host into `hype.efi` again; boot
  already-installed guest disks (not just fresh installers).
  Deps: M9-1
- [ ] **M9-6** — Real-hardware validation of the full persistence cycle.
  Deps: M9-4, M9-5, SETUP-3, SETUP-4

---

## M10 — Physical disk install target
## (plan.md §9 M10, §6d, §10 decision #7/#8/#18)

- [ ] **M10-1** — Adapt a GPLv3-compatible-licensed AHCI/NVMe host driver
  (native AHCI mode + NVMe-over-PCIe only). Confirm source license header
  before adapting (project-level license check, see top of `plan.md`).
  Deps: M3-1
- [ ] **M10-2** — Physical disk enumeration pre-`ExitBootServices` (walk
  UEFI Block I/O handles, capture serial/GUID).
  Deps: M0-3
- [ ] **M10-3** — `blk_backend` physical-disk implementation, with guest
  LBA/length bounds-checking against the real disk's actual capacity per
  VALID-3 — same rule as the file-backed implementation, higher stakes
  since a miss here touches real hardware.
  Deps: M10-1, M10-2, M5-3, VALID-3
- [ ] **M10-4** — Match-before-write safety check: re-confirm enumerated
  serial/GUID matches config at VM start; refuse to boot on mismatch.
  Deps: M10-3
- [ ] **M10-5** — Interactive confirmation on the dashboard (drive
  model/serial/size) before first write, plus non-empty-partition-table
  guard requiring an explicit per-disk override.
  Deps: M8-1, M10-4
- [ ] **M10-6** — Install one guest straight to a real drive; boot it
  natively outside the hypervisor to confirm it's a normal, non-virtualized
  -dependent install.
  Deps: M10-5

---

## STRETCH (plan.md §9, no hard deps beyond a working v1)

- [ ] **STRETCH-1** — Legacy/CSM boot shim.
  Deps: M4-2
- [ ] **STRETCH-2** — Secure Boot signing (self-sign + operator `db`/MOK
  enrollment, per §10 decision #5's "revisit if needed").
  Deps: M0-5
- [ ] **STRETCH-3** — Passthrough NIC via VT-d/AMD-Vi (IOMMU).
  Deps: NET-1
- [ ] **STRETCH-4** — Guest disk image snapshotting (explicit non-goal for
  v1 — only revisit if a real ask emerges, per §10 decision #3).
  Deps: M5-3

---

## DOCS — User-facing documentation

- [ ] **DOCS-1** — End-user `README.md`: written for someone downloading a
  packaged build/installer, not building from source. Covers what hype is,
  supported host/guest requirements, install steps, first-run/basic usage,
  and where to get help — no build/toolchain instructions (those stay in
  `fw/README.md`/`tools/`, not the top-level README). Keep in sync with
  whatever v1's actual packaging/installer mechanism ends up being, so this
  is best written once that's settled (post-M9/M10 area) rather than
  early.
  Deps: none hard, but most useful once install/packaging is real (M9/M10)

---

## V2 — Post-v1 features (explicitly out of v1 scope)

Not part of any v1 milestone above; recorded here so the ask isn't lost,
not scheduled against the critical path.

- [ ] **V2-TELEM-1** — Per-VM vCPU usage telemetry: track actual
  (burst) CPU time used per second per vCPU, plus a separate "reserved"
  figure that assumes 100% utilization of whatever share of a pCPU the
  VM's config reserves (distinct numbers — burst is measured, reserved
  is a static entitlement figure, not sampled).
  Deps: user request 2026-07-15 (during INPUT-4); no v1 milestone Deps
  yet — needs its own scoping pass (where the per-vCPU exit-count/HLT-
  time stats this'd build on, already read by the dashboard per §6b,
  get sampled/rolled up on a 1-second cadence; where per-second
  history is buffered/retained before the API client ships it).
- [ ] **V2-TELEM-2** — Per-VM memory reservation telemetry: the
  `mem_mb` figure already validated by `core/admission.c`, reported
  out per-VM (not working-set/usage — that's the dashboard's own
  best-effort EPT/NPT approximation, §6b; this is the static
  reservation figure).
  Deps: V2-TELEM-1 (shares whatever per-second sampling/buffering
  infrastructure V2-TELEM-1 builds)
- [ ] **V2-TELEM-3** — Per-VM bandwidth usage telemetry (network
  throughput; needs NET-* to exist first — no virtual NIC device
  model exists yet to measure).
  Deps: V2-TELEM-1, NET-1
- [ ] **V2-TELEM-4** — Telemetry client built into the hypervisor
  itself, shipping V2-TELEM-1/2/3's per-VM samples to a **separate,
  not-yet-specced API project** (out of this repo's scope until that
  project has its own protocol/schema/transport decided — this task is
  blocked on that spec existing, not on anything in this codebase).
  Deps: V2-TELEM-1, V2-TELEM-2, V2-TELEM-3
- [ ] **V2-MGMT-1** — SSH-based remote VM management, deferred from
  TERM's v1 GOP-only scope (user request 2026-07-15: "the terminal
  should also be via gop only for now. SSH will be a v2 feature").
  Needs its own scoping pass (network stack doesn't exist yet at all —
  NET-* only covers guest-facing NICs, not a host-side management
  network stack; auth/access-control model undecided).
  Deps: TERM-2, NET-1

---

## Suggested critical path

The shortest path to "install one of each OS family, running concurrently,
survives a host reboot" runs:

```
SETUP-* → M0-* → M1-* → ADM-* → M2-* → M3-* → VALID-* → M4-1..M4-5
   → CPUMSR-*/RAM-*/PCI-*/FW-*/ISO-* (feed M4-6) → M4-6
   → NET-* → M5-* → M6-*  ─┐
                    M7-* ──┼→ M8-* → M9-*
              INPUT-*/VIDEO-* (feed M7 and M8) ┘
```

**VALID-* (§6j) sits on the critical path now**, not off to the side — it
gates every device-emulation task in M4/NET/M5/M7/M10, since it's the fix
for the security review's top finding (guest-supplied addresses trusted
without bounds-checking is the real guest-escape vector, not something
EPT/NPT alone prevents).

M10 (physical disk) and STRETCH items can start any time their listed
dependencies are satisfied — they don't sit on the critical path above and
can be picked up in parallel once M3/VALID (device model + input
validation basics) and M5/M8 (blk_backend, dashboard) exist.
