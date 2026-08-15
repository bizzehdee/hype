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
    uint32_t isr[8];
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
 * For the same reason the IRR (0x200-0x270) is deliberately NOT modelled: there is no point
 * in this design where a vector is known to be requested-but-not-delivered, so any value put
 * there would be invented. Those offsets keep reading 0.
 */
void hype_guest_lapic_accept_vector(hype_guest_lapic_t *lapic, uint8_t vector);

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
