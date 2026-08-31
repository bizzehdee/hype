#ifndef HYPE_DEVICES_GUEST_LAPIC_H
#define HYPE_DEVICES_GUEST_LAPIC_H

#include <stdint.h>

/*
 * FW-1b: a minimal guest-facing Local APIC, emulated via NPF-trapped
 * MMIO at guest-physical 0xFEE00000 (the region FW-1a already leaves
 * not-present). This is the guest's virtual LAPIC -- distinct from the
 * host's own LAPIC helper in arch/x86_64/cpu/lapic.h, and from the
 * unwired AVIC plumbing in arch/x86_64/svm (M2-4): plain trap-and-
 * emulate, matching devices/bochs_vbe.h / devices/pflash.h.
 *
 * Scope: exactly the register subset real OVMF's BaseXApicX2ApicLib
 * touches on the single-vCPU boot path to the UEFI shell (confirmed by
 * reading edk2/UefiCpuPkg/Library/BaseXApicX2ApicLib and MdePkg
 * Register/Intel/LocalApic.h): software-enable (SVR), ID/VERSION, the
 * APIC timer (LVT/init/current/divide + EOI), and benign LINT0/LINT1/
 * DFR/LDR. TPR is not touched by a uniprocessor boot, so it is
 * intentionally not modeled here.
 *
 * GLADDER-6c: the ICR (0x300/0x310) IS modeled, but only for the one
 * shape a uniprocessor guest actually sends: a FIXED-delivery IPI whose
 * destination includes the local (only) CPU -- a self-IPI. Linux >= 6.16
 * kernels drive SRCU grace-period startup through irq_work_queue(),
 * which on x86 raises IRQ_WORK_VECTOR (0xf6) at the local CPU via an
 * ICR write with the self destination shorthand. Dropping that write
 * (the pre-GLADDER-6c behavior) silently killed every synchronize_srcu()
 * in an Ubuntu 26.04 guest: the GP kick never arrived, fsnotify mark
 * teardown never completed, and udevadm wedged the initramfs on exit.
 * Self-targeted fixed IPIs are queued in a 256-bit pending set that the
 * FW-1 loop drains into the normal EVENTINJ/VINTR injection path;
 * INIT/SIPI/NMI and IPIs aimed at other (nonexistent) CPUs are ignored.
 *
 * OVMF's DXE timer (OvmfPkg/LocalApicTimerDxe) requires a *delivered*
 * periodic timer interrupt (vector from LVT_TIMER, = 32) to advance
 * BDS, so this model also drives a synthetic countdown: exact real-time
 * fidelity is irrelevant for reaching the shell, only that the timer
 * IRQ fires periodically once the guest has armed and unmasked it.
 * Injection itself is the caller's job, via the existing INT-1/INT-2
 * EVENTINJ/VINTR path -- this model only decides WHEN a timer IRQ is
 * due and remembers the vector.
 */

/* xAPIC register byte offsets within the 4KB window (MdePkg
 * Register/Intel/LocalApic.h). The window base is
 * HYPE_LAPIC_DEFAULT_BASE (0xFEE00000, arch/x86_64/cpu/lapic.h). */
#define HYPE_GUEST_LAPIC_MMIO_SIZE 0x1000u
#define HYPE_GUEST_LAPIC_REG_ID 0x020u
#define HYPE_GUEST_LAPIC_REG_VERSION 0x030u
#define HYPE_GUEST_LAPIC_REG_TPR 0x080u
#define HYPE_GUEST_LAPIC_REG_PPR 0x0A0u
#define HYPE_GUEST_LAPIC_REG_EOI 0x0B0u
/*
 * #311: the In-Service Register, eight dwords at 16-byte spacing covering all 256 vectors
 * (ISR0 at 0x100 holds vectors 0-31, ISR1 at 0x110 holds 32-63, ... ISR7 at 0x170).
 */
#define HYPE_GUEST_LAPIC_REG_ISR_BASE 0x100u
#define HYPE_GUEST_LAPIC_REG_ISR_LAST 0x170u
/*
 * #789: the Interrupt Request Register, the same eight-dword-at-16-byte-spacing shape as the
 * ISR (IRR0 at 0x200 holds vectors 0-31 ... IRR7 at 0x270). See
 * hype_guest_lapic_set_requested() for why this stopped reading a constant 0.
 */
#define HYPE_GUEST_LAPIC_REG_IRR_BASE 0x200u
#define HYPE_GUEST_LAPIC_REG_IRR_LAST 0x270u
#define HYPE_GUEST_LAPIC_REG_LDR 0x0D0u
#define HYPE_GUEST_LAPIC_REG_DFR 0x0E0u
#define HYPE_GUEST_LAPIC_REG_SVR 0x0F0u
#define HYPE_GUEST_LAPIC_REG_ICR_LOW 0x300u
#define HYPE_GUEST_LAPIC_REG_ICR_HIGH 0x310u
#define HYPE_GUEST_LAPIC_REG_ESR 0x280u
#define HYPE_GUEST_LAPIC_REG_LVT_CMCI 0x2F0u
#define HYPE_GUEST_LAPIC_REG_LVT_TIMER 0x320u
#define HYPE_GUEST_LAPIC_REG_LVT_THERMAL 0x330u
#define HYPE_GUEST_LAPIC_REG_LVT_PMC 0x340u
#define HYPE_GUEST_LAPIC_REG_LVT_ERROR 0x370u
#define HYPE_GUEST_LAPIC_REG_LVT_LINT0 0x350u
#define HYPE_GUEST_LAPIC_REG_LVT_LINT1 0x360u
#define HYPE_GUEST_LAPIC_REG_TIMER_INIT_COUNT 0x380u
#define HYPE_GUEST_LAPIC_REG_TIMER_CURRENT_COUNT 0x390u
#define HYPE_GUEST_LAPIC_REG_TIMER_DIVIDE_CONFIG 0x3E0u

/* Reset/default register values. VERSION: version 0x14, Max-LVT-entry
 * 5 (bits 23:16), matching a typical xAPIC -- OVMF reads it but does
 * not require a specific value on the boot path. */
#define HYPE_GUEST_LAPIC_VERSION_VALUE 0x00050014u
/* LVT_TIMER bit 16 = masked (arch/x86_64/cpu/lapic.h: HYPE_LAPIC_LVT_MASKED);
 * bit 17 = periodic mode; bits 7:0 = vector. */
#define HYPE_GUEST_LAPIC_LVT_MASKED (1u << 16)
#define HYPE_GUEST_LAPIC_LVT_PERIODIC (1u << 17)
#define HYPE_GUEST_LAPIC_LVT_VECTOR_MASK 0xFFu

/* ICR_LOW fields (Intel SDM Vol 3, "Interrupt Command Register"). */
#define HYPE_GUEST_LAPIC_ICR_VECTOR_MASK 0xFFu
#define HYPE_GUEST_LAPIC_ICR_DELMODE_MASK (0x7u << 8)   /* 000 = fixed */
#define HYPE_GUEST_LAPIC_ICR_DESTMODE_LOGICAL (1u << 11)
#define HYPE_GUEST_LAPIC_ICR_DELIVERY_STATUS (1u << 12) /* read-only, always idle here */
#define HYPE_GUEST_LAPIC_ICR_SHORTHAND_MASK (0x3u << 18)
#define HYPE_GUEST_LAPIC_ICR_SHORTHAND_NONE (0x0u << 18)
#define HYPE_GUEST_LAPIC_ICR_SHORTHAND_SELF (0x1u << 18)
#define HYPE_GUEST_LAPIC_ICR_SHORTHAND_ALL_INCL (0x2u << 18)
#define HYPE_GUEST_LAPIC_ICR_SHORTHAND_ALL_EXCL (0x3u << 18)
/* SMP-4 (#188): the delivery modes AP bring-up uses. INIT resets a target vCPU into
 * wait-for-SIPI; STARTUP hands it a real-mode entry at (vector << 12), i.e. CS = vector << 8,
 * IP = 0. Both are ICR_LOW[10:8]. */
#define HYPE_GUEST_LAPIC_ICR_DELMODE_SHIFT 8u
#define HYPE_GUEST_LAPIC_ICR_DELMODE_FIXED 0u
#define HYPE_GUEST_LAPIC_ICR_DELMODE_LOWPRI 1u
#define HYPE_GUEST_LAPIC_ICR_DELMODE_NMI 4u
#define HYPE_GUEST_LAPIC_ICR_DELMODE_INIT 5u
#define HYPE_GUEST_LAPIC_ICR_DELMODE_STARTUP 6u
/* ICR_LOW[14] -- level. INIT de-assert is the (level=0, trigger=level) form older firmware
 * still emits after an INIT assert; it must not be mistaken for a second reset. */
#define HYPE_GUEST_LAPIC_ICR_LEVEL_ASSERT (1u << 14)

/*
 * #601: this LAPIC's current IA32_APIC_BASE mode -- which of the two register
 * addressing schemes below is in force. Numerically matches
 * arch/x86_64/cpu/msr_emulate.h's HYPE_APIC_MODE_* (0/1/2); see that header's
 * comment for why the two are separate #defines rather than a shared one.
 * Owned here (not in the arch layer) because this struct is the single
 * per-vCPU LAPIC model both the xAPIC-MMIO and x2APIC-MSR access paths read
 * and write -- the mode is part of that same state, not a second copy of it.
 */
#define HYPE_GUEST_LAPIC_MODE_DISABLED 0u
#define HYPE_GUEST_LAPIC_MODE_XAPIC 1u
#define HYPE_GUEST_LAPIC_MODE_X2APIC 2u

/*
 * #601: the x2APIC MSR interface (Intel SDM Vol 3A 10.12), MSRs 0x800-0x8FF.
 * Every register except two lives at 0x800 + (its xAPIC MMIO offset >> 4) --
 * e.g. ID (MMIO 0x020) is MSR 0x802, SVR (MMIO 0x0F0) is MSR 0x80F. The two
 * exceptions:
 *
 *   - ICR (0x830) folds xAPIC's two 32-bit halves (ICR_LOW 0x300, ICR_HIGH
 *     0x310) into ONE 64-bit MSR: bits 31:0 are the same ICR_LOW layout
 *     xAPIC uses, bits 63:32 carry the FULL 32-bit destination APIC ID --
 *     unlike xAPIC's ICR_HIGH, which shifts an 8-bit ID into bits 31:24.
 *     There is no destination-shorthand high-half write: one MSR write
 *     latches and sends, matching xAPIC's "writing ICR_LOW sends" rule but
 *     with both halves supplied atomically.
 *   - Self IPI (0x83F) has no xAPIC MMIO counterpart: writing a vector to it
 *     is architecturally equivalent to an ICR write with shorthand=SELF and
 *     delivery mode FIXED, just without the ICR round-trip. It is
 *     write-only; a read is a guest #GP.
 */
#define HYPE_GUEST_LAPIC_X2APIC_MSR_BASE 0x800u
#define HYPE_GUEST_LAPIC_X2APIC_MSR_LAST 0x8FFu
#define HYPE_GUEST_LAPIC_X2APIC_MSR_ICR 0x830u
#define HYPE_GUEST_LAPIC_X2APIC_MSR_SELF_IPI 0x83Fu

/*
 * SMP-4/SMP-5 (#188/#189): one IPI this LAPIC has SENT that something other than itself has to
 * act on -- INIT and STARTUP always, and a fixed IPI whose destination includes another vCPU.
 *
 * The LAPIC model is per-vCPU and deliberately knows nothing about its siblings: how many
 * exist, and which host core each runs on, is the VM layer's business. So it decodes the ICR
 * write and records what was asked for; the caller routes it. Keeping the decode here is what
 * makes it unit-testable without a VM.
 */
typedef struct {
    uint32_t delivery_mode; /* HYPE_GUEST_LAPIC_ICR_DELMODE_* */
    uint32_t vector;        /* ICR_LOW[7:0]; the SIPI start page for STARTUP */
    uint32_t dest_apic_id;  /* physical destination, or the logical mask when `logical` */
    uint32_t shorthand;     /* HYPE_GUEST_LAPIC_ICR_SHORTHAND_* */
    uint32_t logical;       /* nonzero when ICR_LOW[11] selected logical destination mode */
    uint32_t level_assert;  /* ICR_LOW[14]; 0 on an INIT DE-assert */
} hype_guest_lapic_ipi_t;

/*
 * How many hype_guest_lapic_tick() calls (one per guest VM-exit in the
 * FW-1 run loop) elapse between synthetic timer expiries once the timer
 * is armed. Coarse on purpose: small enough that BDS timeouts advance
 * promptly, large enough not to livelock the guest in its timer ISR.
 * Tunable from QEMU/real-hardware serial if the shell is slow to appear
 * or the guest spins in the ISR.
 */
#define HYPE_GUEST_LAPIC_TICK_EXITS 64u

typedef struct {
    /*
     * #311: the In-Service Register, as a 256-bit set. This is not decoration -- a guest
     * reads it to find out WHICH vector it is handling.
     *
     * FreeBSD shares one interrupt stub across each block of 32 vectors and recovers the
     * vector by reading the matching ISR dword and taking the index of its highest set bit:
     *
     *     mov  0x110(%rdx),%eax   ; ISR1 (vectors 32-63)
     *     bsr  %eax,%eax
     *     je   3f                 ; ISR1 == 0 -> skip the handler entirely
     *     add  $0x20,%eax
     *     call lapic_handle_intr
     *  3: jmp  doreti
     *
     * While these offsets read as 0, that `je` is always taken and EVERY I/O interrupt is
     * discarded by the guest before its handler runs -- interrupts hype delivered correctly.
     * FreeBSD's LAPIC timer survived it only because Xtimerint is a dedicated stub that calls
     * lapic_handle_timer without consulting the ISR, which is why this presented as a working
     * hypervisor with one broken AHCI device. Linux never noticed because it uses one IDT
     * entry per vector, so the vector is implicit in the entry point.
     */
    /*
     * SMP-3 (#187): THIS LAPIC's APIC ID, reported by the ID register (0x020) in its
     * architectural bits [31:24]. It used to be hardcoded to 0 on read, which was true only
     * while a VM had one vCPU: with several, every vCPU would read 0 and a guest OS would be
     * unable to tell them apart -- and it would disagree with the MADT and with CPUID leaf 1
     * EBX[31:24], both of which report per-vCPU IDs since SMP-2.
     */
    uint32_t apic_id;
    /*
     * #601: HYPE_GUEST_LAPIC_MODE_* -- which register interface (xAPIC MMIO or
     * x2APIC MSR) currently addresses this LAPIC. Defaults to XAPIC on reset,
     * matching hype's pre-#601 behavior of an always-enabled xAPIC LAPIC: no
     * caller that never touches this field observes any change.
     */
    uint32_t apic_mode;
    uint32_t isr[8];
    uint32_t irr[8]; /* #789: requested-but-not-yet-delivered, published by the VMM layer */
    uint32_t tpr; /* Task Priority Register (0x080); PPR (0x0A0) is derived from it and isr */
    uint32_t svr;
    uint32_t lvt_timer;
    uint32_t lvt_lint0;
    uint32_t lvt_lint1;
    /* #94: the remaining LVT entries and the ESR. Windows programs LVT ERROR
     * (and the PMC entry) at boot, and PatchGuard SHADOWS the whole LVT table:
     * a register whose write is dropped and whose read returns 0 shows up as
     * CRITICAL_STRUCTURE_CORRUPTION arg4=0x17 ("Local APIC modification") --
     * the 0x109 that kept killing Windows mid-install. Nothing is wired to
     * these sources; faithful storage is the entire requirement. */
    uint32_t lvt_thermal;
    uint32_t lvt_pmc;
    uint32_t lvt_error;
    uint32_t lvt_cmci;
    uint32_t esr;
    uint32_t dfr;
    uint32_t ldr;
    uint32_t icr_low;  /* last ICR_LOW written (delivery status reads as idle) */
    uint32_t icr_high; /* destination field, bits 31:24 */
    /* SMP-4 (#188): the outbound-IPI slot -- see hype_guest_lapic_take_ipi. */
    hype_guest_lapic_ipi_t ipi_out;
    int ipi_out_valid;
    uint64_t ipi_out_dropped; /* overwritten before the caller drained: a bug signal, not noise */
    uint64_t ipi_out_count;
    uint32_t self_ipi_pending[8]; /* 256-bit set of self-IPI vectors awaiting injection */
    uint64_t self_ipi_count;      /* GLADDER-6c diag: total self-IPIs accepted */
    uint32_t divide_config;
    uint32_t init_count;
    uint32_t current_count;
    uint32_t tick_accum;   /* VM-exits since the last synthetic expiry */
    uint64_t divide_accum; /* M4-6b5: fractional carry when dividing the base-rate
                            * advance by the guest's divide_config divisor */
    uint32_t lvt_timer_armed_seen; /* M4-6b5 diag: last LVT_TIMER value the guest
                                    * wrote with the mask bit CLEAR (0 if it never
                                    * armed the timer -- tells "never tried" from
                                    * "tried then re-masked") */
    int timer_irq_pending; /* a timer IRQ is due but not yet delivered */
    int timer_in_service;  /* delivered, awaiting guest EOI -- at most one in flight */
    uint64_t eoi_count;    /* M4-6b2: total guest LAPIC EOIs (0xB0 writes). The FW-1
                            * loop watches this to know the guest finished an ISR and
                            * broadcast EOI: for a level-triggered IO-APIC line (AHCI)
                            * that is the signal to drop Remote-IRR so the next
                            * assertion can inject -- real hardware clears Remote-IRR
                            * on the LAPIC EOI broadcast, not on the device line going
                            * low, and relying only on the line-low deassert races a
                            * fresh completion into a stuck Remote-IRR (30s ATAPI
                            * command timeouts + libata reset/retry). */
} hype_guest_lapic_t;

/*
 * M4-6b5: decode the LAPIC Divide Configuration Register (offset 0x3E0) into
 * the timer's clock divisor. Per Intel SDM the divisor is encoded in bits
 * [3,1,0]: 000->2, 001->4, 010->8, 011->16, 100->32, 101->64, 110->128,
 * 111->1. Pure -- unit tested.
 */
uint32_t hype_guest_lapic_divisor(uint32_t divide_config);

/* Clears to a just-powered-on state (timer masked, no IRQ pending). */

void hype_guest_lapic_reset(hype_guest_lapic_t *lapic);

/*
 * Pop the pending outbound IPI, if any. Returns 1 and fills `out`, or 0.
 *
 * One slot, not a queue: every ICR_LOW write is an MMIO write that takes a VM exit, and the
 * dispatch loop drains here on the same exit, so a second cannot be written before the first
 * is taken. `ipi_out_dropped` counts any that were, so the assumption is measured rather than
 * trusted -- a nonzero count means the drain site is wrong, and says so.
 */
int hype_guest_lapic_take_ipi(hype_guest_lapic_t *lapic, hype_guest_lapic_ipi_t *out);

/* SMP-3 (#187): set this LAPIC's APIC ID. Called once per vCPU after reset; the ID must match
 * what the MADT and CPUID report for the same vCPU or the guest logs an APIC ID mismatch. */
void hype_guest_lapic_set_apic_id(hype_guest_lapic_t *lapic, uint32_t apic_id);

/*
 * #601: set this LAPIC's current mode (HYPE_GUEST_LAPIC_MODE_*). The caller
 * (the arch-layer IA32_APIC_BASE WRMSR handler) is the one that knows whether
 * the requested transition is legal -- see
 * arch/x86_64/cpu/msr_emulate.h's hype_apic_base_mode_transition() -- so this
 * setter does no legality checking of its own; it just stores the result.
 */
void hype_guest_lapic_set_apic_mode(hype_guest_lapic_t *lapic, uint32_t apic_mode);

/*
 * #601: x2APIC MSR read/write (MSRs 0x800-0x8FF), over the SAME per-vCPU state
 * hype_guest_lapic_read()/hype_guest_lapic_write() serve over MMIO -- no
 * separate model, just a second decode of the register number and, for the
 * handful of registers that differ (ID's width, the ICR's 64-bit shape, LDR
 * and EOI's x2APIC-specific rules), different rules for the SAME storage.
 *
 * Both fail (-1) rather than falling back to a benign default when the LAPIC
 * is not currently in x2APIC mode (HYPE_GUEST_LAPIC_MODE_X2APIC), or for a
 * register/value that is architecturally illegal to access this way (DFR and
 * ICR_HIGH do not exist as separate x2APIC MSRs; ID/VERSION/PPR/LDR are
 * read-only; EOI must be written 0; Self IPI is write-only and rejects
 * vectors 0-15). The caller's job on failure is to inject #GP(0), matching
 * every other synthetic-MSR rejection in this project. Returns 0 on success.
 */
int hype_guest_lapic_x2apic_read(hype_guest_lapic_t *lapic, uint32_t msr_number, uint64_t *out);
int hype_guest_lapic_x2apic_write(hype_guest_lapic_t *lapic, uint32_t msr_number, uint64_t value);

/*
 * MMIO read/write within the 4KB window (offset is relative to
 * 0xFEE00000). `size` must be 4 (xAPIC registers are 32-bit dword
 * accesses); other sizes return -1 so the NPF handler fails closed.
 * Returns 0 on success.
 */
int hype_guest_lapic_read(hype_guest_lapic_t *lapic, uint32_t offset, unsigned int size, uint32_t *out);
int hype_guest_lapic_write(hype_guest_lapic_t *lapic, uint32_t offset, unsigned int size, uint32_t value);

/*
 * Advance the synthetic timer by one VM-exit. When the timer is armed
 * (init_count != 0) and unmasked, sets timer_irq_pending once every
 * HYPE_GUEST_LAPIC_TICK_EXITS calls, and keeps current_count visibly
 * counting down (reloading from init_count) so guest calibration/delay
 * reads of TIMER_CURRENT_COUNT observe motion.
 */
void hype_guest_lapic_tick(hype_guest_lapic_t *lapic);

/*
 * Advances the timer by `ticks` timer-clock cycles in one O(1) step --
 * the real-time-proportional form of hype_guest_lapic_tick() for M4-6b1,
 * where the FW-1 loop advances the LAPIC timer by the same real-elapsed
 * tick count it advances the PIT. When armed+unmasked, decrements
 * current_count by `ticks`; when the count reaches terminal, sets
 * timer_irq_pending (periodic -> reloads from init_count keeping phase;
 * one-shot -> stays at 0). Because current_count now moves at the same
 * real rate as the PIT, a guest that calibrates the LAPIC timer against
 * the PIT/TSC gets a consistent, usable frequency and its programmed
 * initial count fires at the real rate it intended.
 */
void hype_guest_lapic_advance(hype_guest_lapic_t *lapic, uint64_t ticks);

/*
 * If a timer IRQ is due and none is currently in service, returns 1 and
 * writes the timer vector (LVT_TIMER bits 7:0) to *vector_out, marking
 * it in-service and clearing the pending flag. Otherwise returns 0. A
 * subsequent guest write to the EOI register clears the in-service
 * state (in hype_guest_lapic_write), re-arming the next delivery.
 */
int hype_guest_lapic_take_timer_irq(hype_guest_lapic_t *lapic, uint8_t *vector_out);

/* #436: recover a wedged timer in-service latch. One lost guest EOI (an MMIO
 * write form the decoder missed) permanently gated ALL further timer IRQs off
 * -- OVMF's periodic events (incl. its keyboard poll) died with it. Returns 1
 * if it cleared a stuck latch. */
int hype_guest_lapic_recover_in_service(hype_guest_lapic_t *lapic);

/*
 * GLADDER-6c: if any self-IPI vector is pending (an ICR write with fixed
 * delivery whose destination included this CPU), returns 1, writes the
 * lowest pending vector to *vector_out and clears it from the pending
 * set. Returns 0 when the set is empty. The caller injects via the same
 * request-interrupt path as every other source; its IRR coalesces
 * duplicates, matching real fixed-IPI semantics.
 */
int hype_guest_lapic_take_self_ipi(hype_guest_lapic_t *lapic, uint8_t *vector_out);

/*
 * SMP-6 (#190): pend `vector` on a LAPIC that belongs to a DIFFERENT vCPU. Safe to call from
 * another core -- it is an atomic OR into the pending set, and the owning vCPU drains it into
 * its own VMCB. Use this for cross-vCPU delivery instead of touching the target's context.
 */
void hype_guest_lapic_post_vector(hype_guest_lapic_t *lapic, uint8_t vector);

/*
 * #311: record that `vector` has been committed to the guest, by setting its ISR bit.
 *
 * Call this wherever hype hands a vector to the guest, so that a guest which reads the ISR
 * to identify what it is servicing gets the truth. The matching clear happens on the guest's
 * EOI write, which clears the HIGHEST set ISR bit -- so nested delivery behaves as a stack,
 * the way real hardware and a guest's LIFO EOI order both expect.
 *
 * "Committed" rather than "delivered" is deliberate and is the honest limit of this model:
 * hype does not queue interrupts at the LAPIC (a vector that cannot be injected immediately
 * is queued in the VMCB/VMCS layer's pending set, not here), so this is the last moment the
 * LAPIC model is told anything. The bit can therefore be set marginally before the guest
 * actually takes the interrupt. That does not affect the readers this exists for: a guest
 * reads the ISR from *inside* its handler, which is strictly after delivery.
 *
 * The IRR (0x200-0x270) used to be left unmodelled on the reasoning that no point in this
 * design knows a vector to be requested-but-not-delivered. That reasoning was wrong, and #789
 * is what it cost: the VMM layer's per-vCPU `pending_irr` is exactly that set, and it is not
 * rare -- a Windows AP spinning with IF=0 sat on 16,340,460 NPFs at 0xFEE00210 while hype held
 * two undelivered vectors for it and coalesced 16,777 further injection attempts. A guest that
 * polls its own IRR to find out what is waiting is entitled to see it. See
 * hype_guest_lapic_set_requested().
 */
void hype_guest_lapic_accept_vector(hype_guest_lapic_t *lapic, uint8_t vector);

/*
 * #789: publish the VMM layer's pending-vector bitmap as this LAPIC's IRR.
 *
 * The LAPIC model does not own this state and must not: a vector lives in the VMCB/VMCS
 * layer's `pending_irr` from the moment it is requested until the moment it is injected, and
 * duplicating that here would be a second copy to keep in step. This is a VIEW, refreshed by
 * whoever serves an IRR read, so the guest always sees the one authoritative set.
 *
 * `irr8` is eight dwords, vector v at word v/32 bit v%32 -- the architectural IRR layout, and
 * already the layout `pending_irr` uses.
 */
void hype_guest_lapic_set_requested(hype_guest_lapic_t *lapic, const uint32_t *irr8);

/*
 * The highest vector currently marked in service, or -1 if none. This is the SDM's ISRV, and
 * it is what a guest's `bsr` over an ISR dword is computing.
 */
int hype_guest_lapic_isr_highest(const hype_guest_lapic_t *lapic);

/*
 * The Processor Priority Register value (MMIO 0x0A0), derived per Intel SDM Vol 3 "Task and
 * Processor Priorities": the priority class is the greater of TPR's and that of the highest
 * in-service vector, and the sub-class is TPR's only when TPR's class wins outright.
 */
uint32_t hype_guest_lapic_ppr(const hype_guest_lapic_t *lapic);

#endif /* HYPE_DEVICES_GUEST_LAPIC_H */
