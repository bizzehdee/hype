#ifndef HYPE_DEVICES_E820_H
#define HYPE_DEVICES_E820_H

#include <stdint.h>

/*
 * FW-1a: builder for the QEMU fw_cfg "etc/e820" file, the memory map
 * real OVMF reads FIRST (before the CMOS 0x34/0x35 fallback) to decide
 * how much guest RAM it has and where the top of low RAM is
 * (edk2/OvmfPkg/Library/PlatformInitLib/MemDetect.c: PlatformScanE820).
 *
 * Wire format (edk2/OvmfPkg/Include/IndustryStandard/E820.h,
 * EFI_E820_ENTRY64, #pragma pack(1)): an array of 20-byte little-endian
 * entries, each { uint64_t BaseAddr; uint64_t Length; uint32_t Type }.
 * Bytes are emitted explicitly little-endian (x86) so the result is
 * independent of this builder's own struct padding.
 */

#define HYPE_E820_ENTRY_SIZE 20u

/* EFI_ACPI_MEMORY_TYPE values OVMF understands (E820.h). */
#define HYPE_E820_TYPE_RAM 1u      /* EfiAcpiAddressRangeMemory -- usable */
#define HYPE_E820_TYPE_RESERVED 2u /* EfiAcpiAddressRangeReserved */

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} hype_e820_region_t;

/*
 * Serializes `count` regions into `out` in the etc/e820 wire format.
 * Returns the number of bytes written (count * HYPE_E820_ENTRY_SIZE),
 * or -1 if `count` is 0 or `out_cap` is too small to hold every entry.
 * Pure data serialization -- no CPU state, no UEFI dependency.
 */
int hype_e820_build(uint8_t *out, uint32_t out_cap, const hype_e820_region_t *regions, unsigned int count);

#endif /* HYPE_DEVICES_E820_H */
