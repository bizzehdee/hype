#ifndef HYPE_DEVICES_IOAPIC_H
#define HYPE_DEVICES_IOAPIC_H

#include <stdint.h>

/*
 * M4-6b3: minimal emulated I/O APIC (Intel 82093AA-style), the interrupt
 * router an ACPI-mode guest programs instead of the legacy 8259 PIC. Once
 * hype delivers a real MADT (SVM-STRIO), the Linux kernel masks the PIC and
 * routes every external IRQ line through this chip's redirection table to a
 * LAPIC vector -- so without it, no device (or the PIT clockevent reference
 * the kernel's check_timer() probes) can reach the guest and it hangs in
 * early boot.
 *
 * Register model (well-known 82093AA layout, transcribed from the datasheet
 * and QEMU's hw/intc/ioapic.c -- not project-specific): a two-register MMIO
 * window at the base the MADT advertises (0xFEC00000):
 *   base + 0x00  IOREGSEL  -- write selects a register index (low 8 bits)
 *   base + 0x10  IOWIN     -- read/write the selected 32-bit register
 * Register indices:
 *   0x00  IOAPICID   (bits 27:24 = ID)
 *   0x01  IOAPICVER  (bits 7:0 = version 0x11; bits 23:16 = max redir = 23)
 *   0x02  IOAPICARB  (bits 27:24 = arbitration ID)
 *   0x10 + 2n  redirection entry n, low 32 bits   (n = 0 .. 23)
 *   0x11 + 2n  redirection entry n, high 32 bits
 * Redirection-table entry (64 bits): bits 7:0 vector, 10:8 delivery mode,
 * 11 dest mode, 12 delivery status (RO), 13 polarity, 14 remote IRR (RO,
 * level), 15 trigger mode (0=edge/1=level), 16 mask, 63:56 destination.
 *
 * This is pure logic (no CPU/VMCB/MMIO register access of its own) so it is
 * fully unit tested; the thin SVM NPF shim that feeds it guest MMIO
 * (hype_svm_vcpu_handle_ioapic_npf) and the loop code that raises GSIs are
 * the only exempt pieces.
 */

#define HYPE_IOAPIC_NUM_RTES 24u

/* MMIO window offsets (from the region base). */
#define HYPE_IOAPIC_REG_IOREGSEL 0x00u
#define HYPE_IOAPIC_REG_IOWIN 0x10u
#define HYPE_IOAPIC_MMIO_SIZE 0x20u

/* Register indices selected via IOREGSEL. */
#define HYPE_IOAPIC_INDEX_ID 0x00u
#define HYPE_IOAPIC_INDEX_VER 0x01u
#define HYPE_IOAPIC_INDEX_ARB 0x02u
#define HYPE_IOAPIC_INDEX_REDIR_BASE 0x10u

#define HYPE_IOAPIC_VERSION 0x11u

/* Redirection-entry bit fields (low 32 bits unless noted). */
#define HYPE_IOAPIC_RTE_VECTOR_MASK 0x000000FFu
#define HYPE_IOAPIC_RTE_DELMODE_SHIFT 8
#define HYPE_IOAPIC_RTE_DELMODE_MASK 0x00000700u
#define HYPE_IOAPIC_RTE_DELIVERY_STATUS (1u << 12) /* RO */
#define HYPE_IOAPIC_RTE_REMOTE_IRR (1u << 14)      /* RO, level only */
#define HYPE_IOAPIC_RTE_TRIGGER_LEVEL (1u << 15)
#define HYPE_IOAPIC_RTE_MASK (1u << 16)

/* Delivery mode 000 = Fixed (the only mode this router injects a plain vector
 * for; NMI/SMI/INIT/ExtINT are not modeled -- an ACPI PC guest routes ordinary
 * device lines as Fixed). */
#define HYPE_IOAPIC_DELMODE_FIXED 0u

typedef struct {
    uint32_t ioregsel;                     /* last value written to IOREGSEL */
    uint32_t id;                           /* IOAPICID (bits 27:24 meaningful) */
    uint64_t rte[HYPE_IOAPIC_NUM_RTES];    /* redirection table */
} hype_ioapic_t;

/*
 * Resets to power-on state: IOREGSEL 0, ID 0, and every redirection entry
 * MASKED (bit 16 set) with all other fields 0 -- exactly the 82093AA reset
 * state a guest expects before it programs the table.
 */
void hype_ioapic_reset(hype_ioapic_t *io);

/*
 * MMIO read at `offset` (0x00 IOREGSEL or 0x10 IOWIN) within the window.
 * Returns 0 with *out set on success, -1 for an unsupported offset. Pure.
 */
int hype_ioapic_mmio_read(const hype_ioapic_t *io, uint32_t offset, uint32_t *out);

/*
 * MMIO write of `value` at `offset`. A write to IOREGSEL latches the index;
 * a write to IOWIN updates the selected register. RTE writes preserve the
 * read-only Delivery-Status (bit 12) and Remote-IRR (bit 14) bits from the
 * current entry. Returns 0 on success, -1 for an unsupported offset. Pure.
 */
int hype_ioapic_mmio_write(hype_ioapic_t *io, uint32_t offset, uint32_t value);

/*
 * A device just asserted GSI line `gsi`. If the entry is programmed for
 * delivery (unmasked, Fixed delivery mode) this returns 1 and sets
 * *out_vector to the vector to inject into the guest LAPIC; for a
 * level-triggered entry it also latches Remote-IRR so a still-asserted line
 * won't re-inject until the guest EOIs. Returns 0 (no injection) when the
 * entry is masked, not Fixed, out of range, or a level entry whose Remote-IRR
 * is already set. Pure.
 */
int hype_ioapic_raise(hype_ioapic_t *io, uint32_t gsi, uint8_t *out_vector);

/*
 * Guest signalled EOI for `vector` (its LAPIC broadcasts the EOI to the
 * IO-APIC for level-triggered lines): clears Remote-IRR on every level entry
 * carrying that vector so the next assertion can inject again. Pure.
 */
void hype_ioapic_eoi(hype_ioapic_t *io, uint8_t vector);

/*
 * A level-triggered device deasserted GSI `gsi` (its interrupt condition
 * cleared): drops the entry's Remote-IRR so the next assertion can inject
 * again. hype uses this to model a level line going low without having to
 * decode which vector the guest EOIs (its minimal LAPIC tracks no external
 * ISR). Edge entries, masked entries with no Remote-IRR, and out-of-range
 * GSIs are no-ops. Pure.
 */
void hype_ioapic_deassert(hype_ioapic_t *io, uint32_t gsi);

#endif /* HYPE_DEVICES_IOAPIC_H */
