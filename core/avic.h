#ifndef HYPE_CORE_AVIC_H
#define HYPE_CORE_AVIC_H

#include <stdint.h>

/*
 * #193 (SMP-9): AMD AVIC data structures + capability gate. Hardware-accelerated IPI delivery
 * replaces the trap-and-emulate LAPIC/IPI path (SMP-3..5) with the CPU delivering guest IPIs
 * directly, trapping to hype only for the cases hardware cannot accelerate (AVIC_INCOMPLETE_IPI,
 * AVIC_NOACCEL). This module is the PURE, unit-testable part: the capability decode and the AVIC
 * table-entry encoders. It is only ever ACTIVATED when the host CPU reports AVIC support -- on a
 * CPU (or QEMU) without it, hype keeps the emulated path unchanged, so nothing regresses.
 *
 * AVIC uses three tables (AMD APM Vol 2 §15.29):
 *   - APIC Backing Page (per vCPU, 4 KiB): the virtualized APIC register file the CPU reads/writes.
 *   - Physical APIC ID Table (per VM): indexed by guest physical APIC ID; each entry names the host
 *     APIC ID to doorbell and the target vCPU's backing page.
 *   - Logical APIC ID Table (per VM): maps a logical destination to a physical-table index.
 */

/* AMD AVIC is CPUID leaf 0x8000000A, EDX bit 13. */
#define HYPE_AVIC_CPUID_EDX_BIT (1u << 13)

/* Physical APIC ID Table Entry (64-bit) field positions. */
#define HYPE_AVIC_PHYS_HOST_APIC_ID_MASK 0x00000FFFull /* [11:0] host physical APIC ID */
#define HYPE_AVIC_PHYS_BACKING_MASK 0x000FFFFFFFFFF000ull /* [51:12] backing page PA */
#define HYPE_AVIC_PHYS_IS_RUNNING (1ull << 62)
#define HYPE_AVIC_PHYS_VALID (1ull << 63)

/* Logical APIC ID Table Entry (32-bit). */
#define HYPE_AVIC_LOG_GUEST_PHYS_ID_MASK 0x000000FFu /* [7:0] guest physical APIC ID index */
#define HYPE_AVIC_LOG_VALID (1u << 31)

/* 1 if the CPU supports AVIC (pass CPUID leaf 0x8000000A EDX). Pure. */
int hype_avic_supported(uint32_t svm_feature_edx);

/*
 * Encode a Physical APIC ID Table entry. `host_apic_id` is the physical APIC ID of the core the
 * target vCPU runs on (the doorbell target); `backing_page_phys` is that vCPU's 4 KiB APIC backing
 * page (only bits [51:12] are used); `is_running` and `valid` set the corresponding flags. An
 * invalid entry (valid==0) encodes to 0.
 */
uint64_t hype_avic_physical_entry(uint32_t host_apic_id, uint64_t backing_page_phys,
                                  int is_running, int valid);

/* Encode a Logical APIC ID Table entry mapping a logical destination to physical-table index
 * `guest_physical_id`. An invalid entry encodes to 0. */
uint32_t hype_avic_logical_entry(uint32_t guest_physical_id, int valid);

/*
 * Build a VM's Physical APIC ID Table into `table` (a 4 KiB page, 512 8-byte entries). `count`
 * vCPUs, entry i for guest APIC id i, each pointing at backing_page_phys[i] on host APIC id
 * host_apic_ids[i], marked valid + running. Entries past `count` are cleared. Returns the count
 * written, or 0 on a bad argument. `max_index_out` receives count-1 (the VMCB's
 * AVIC_PHYSICAL_MAX_INDEX field), 0 when count is 0.
 */
unsigned int hype_avic_build_physical_table(uint64_t *table, unsigned int table_entries,
                                            const uint32_t *host_apic_ids,
                                            const uint64_t *backing_page_phys, unsigned int count,
                                            uint8_t *max_index_out);

/*
 * #640 (cause 2): highest set bit across an 8x32-bit register bitmap -- the shape the AVIC
 * backing page's ISR/IRR/TMR ranges use (word i at byte offset base + 16*i within the page),
 * which is the plain x86 local-APIC register layout Chapter 16 defines and every local APIC
 * since the first one has used, not an AVIC-specific guess. -1 if every word is 0. Mirrors
 * devices/guest_lapic.c's own hype_guest_lapic_isr_highest() bit-scan exactly, but over raw
 * words instead of a hype_guest_lapic_t, so it can run directly against the backing page's own
 * bytes: decision 67 (plan.md #67) makes the backing page authoritative once AVIC is active,
 * so this must never read/write g_fw_1_lapic's separate software model.
 */
int hype_avic_bitmap_highest(const uint32_t words[8]);

/*
 * #640 (cause 2): decodes a flat-mode Logical Destination Register write into the Logical APIC
 * ID Table's bit index. AMD APM Vol 2 §15.29.5.3: flat mode uses only the table's first 8
 * entries, and "supported encodings must be of the form 2^i" -- i.e. bits [31:24] of LDR must
 * have exactly one bit set. Returns that bit's index (0-7), or -1 if LDR does not encode a
 * single flat-mode logical ID (cluster mode, or an all-zero/multi-bit value no real guest
 * driver produces in flat mode). Pure bit test.
 */
int hype_avic_ldr_flat_index(uint32_t ldr);

#endif /* HYPE_CORE_AVIC_H */
