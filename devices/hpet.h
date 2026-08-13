#ifndef HYPE_DEVICES_HPET_H
#define HYPE_DEVICES_HPET_H

#include <stdint.h>

/*
 * #436: HPET (High Precision Event Timer), the one piece of standard PC timer
 * hardware hype did not model at all. Every QEMU q35 platform provides one --
 * confirmed by dumping a KVM guest's tables with the UEFI shell's `acpiview`,
 * which lists HPET beside FACP/APIC/MCFG -- and an OS that finds the block
 * described in ACPI expects to read a monotonic counter from it.
 *
 * The counter is derived from the host TSC exactly as the ACPI PM timer's is,
 * so both agree with real elapsed time and with each other. The period is a
 * round 100 ns (10 MHz), the value every real HPET and every emulated one
 * advertises, so a guest's femtosecond arithmetic divides evenly.
 *
 * Three comparators are the architectural minimum and are what is modelled:
 * advertising fewer would describe hardware no HPET may be, and advertising
 * more than are implemented is the mistake this project keeps finding in its
 * own tables.
 */

#define HYPE_HPET_MMIO_BASE 0xFED00000ULL
#define HYPE_HPET_MMIO_SIZE 0x400u

/* Counter period in femtoseconds: 100 ns == 100,000,000 fs == 10 MHz. */
#define HYPE_HPET_PERIOD_FS 0x05F5E100u
#define HYPE_HPET_TICKS_PER_SECOND 10000000ull

#define HYPE_HPET_NUM_TIMERS 3u

/* Register offsets (ACPI/IA-PC HPET specification 1.0a, section 2.3). */
#define HYPE_HPET_REG_CAP_ID 0x000u
#define HYPE_HPET_REG_CONFIG 0x010u
#define HYPE_HPET_REG_INT_STATUS 0x020u
#define HYPE_HPET_REG_MAIN_COUNTER 0x0F0u
#define HYPE_HPET_REG_TIMER_BASE 0x100u
#define HYPE_HPET_REG_TIMER_STRIDE 0x20u

/* General Configuration bits. */
#define HYPE_HPET_CONFIG_ENABLE (1ull << 0)
#define HYPE_HPET_CONFIG_LEGACY_ROUTE (1ull << 1)

/* Per-timer configuration bits that a guest may set. */
#define HYPE_HPET_TIMER_INT_TYPE_LEVEL (1ull << 1)
#define HYPE_HPET_TIMER_INT_ENABLE (1ull << 2)
#define HYPE_HPET_TIMER_PERIODIC (1ull << 3)
#define HYPE_HPET_TIMER_PERIODIC_CAP (1ull << 4)
#define HYPE_HPET_TIMER_SIZE_CAP (1ull << 5)
#define HYPE_HPET_TIMER_VAL_SET (1ull << 6)
#define HYPE_HPET_TIMER_32BIT_MODE (1ull << 8)

typedef struct {
    uint64_t config;     /* per-timer Tn_CONF */
    uint64_t comparator; /* per-timer Tn_CMP */
    uint64_t period;     /* periodic reload, captured when VAL_SET is used */
} hype_hpet_timer_t;

typedef struct {
    uint64_t config;      /* general configuration */
    uint64_t int_status;  /* general interrupt status (write-1-to-clear) */
    uint64_t counter;     /* main counter, in 100 ns ticks */
    hype_hpet_timer_t timers[HYPE_HPET_NUM_TIMERS];
} hype_hpet_t;

void hype_hpet_reset(hype_hpet_t *hpet);

/* The read-only capabilities register this model reports. */
uint64_t hype_hpet_capabilities(void);

/*
 * Advance the main counter by `ticks` 100 ns units and latch any comparator
 * matches into the interrupt status. Returns a bitmask of timers that fired on
 * this call (bit N = timer N), so the caller can raise the guest interrupt the
 * same way it does for the PIT and the LAPIC timer. Counting only happens when
 * ENABLE_CNF is set, exactly as the hardware behaves.
 */
uint32_t hype_hpet_advance(hype_hpet_t *hpet, uint64_t ticks);

/* 64-bit register read/write. `size` is 4 or 8; 4-byte accesses address either
 * half of a 64-bit register, which is how 32-bit guests drive an HPET. */
uint64_t hype_hpet_read(const hype_hpet_t *hpet, uint32_t offset, unsigned size);
void hype_hpet_write(hype_hpet_t *hpet, uint32_t offset, unsigned size, uint64_t value);

#endif /* HYPE_DEVICES_HPET_H */
