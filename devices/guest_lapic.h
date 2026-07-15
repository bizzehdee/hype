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
 * DFR/LDR. TPR and ICR (INIT/SIPI) are NOT touched by a uniprocessor
 * boot, so they are intentionally not modeled here.
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
#define HYPE_GUEST_LAPIC_REG_EOI 0x0B0u
#define HYPE_GUEST_LAPIC_REG_LDR 0x0D0u
#define HYPE_GUEST_LAPIC_REG_DFR 0x0E0u
#define HYPE_GUEST_LAPIC_REG_SVR 0x0F0u
#define HYPE_GUEST_LAPIC_REG_LVT_TIMER 0x320u
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
    uint32_t svr;
    uint32_t lvt_timer;
    uint32_t lvt_lint0;
    uint32_t lvt_lint1;
    uint32_t dfr;
    uint32_t ldr;
    uint32_t divide_config;
    uint32_t init_count;
    uint32_t current_count;
    uint32_t tick_accum;   /* VM-exits since the last synthetic expiry */
    int timer_irq_pending; /* a timer IRQ is due but not yet delivered */
    int timer_in_service;  /* delivered, awaiting guest EOI -- at most one in flight */
} hype_guest_lapic_t;

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
 * If a timer IRQ is due and none is currently in service, returns 1 and
 * writes the timer vector (LVT_TIMER bits 7:0) to *vector_out, marking
 * it in-service and clearing the pending flag. Otherwise returns 0. A
 * subsequent guest write to the EOI register clears the in-service
 * state (in hype_guest_lapic_write), re-arming the next delivery.
 */
int hype_guest_lapic_take_timer_irq(hype_guest_lapic_t *lapic, uint8_t *vector_out);

#endif /* HYPE_DEVICES_GUEST_LAPIC_H */
