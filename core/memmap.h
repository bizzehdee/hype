#ifndef HYPE_MEMMAP_H
#define HYPE_MEMMAP_H

#include "efi_types.h"

/* Human-readable name for a UEFI EFI_MEMORY_TYPE value. Pure lookup. */
const char *hype_memmap_type_name(UINT32 type);

/*
 * Fetches the current UEFI memory map via the classic two-call pattern:
 * probe for the required size (EFI_BUFFER_TOO_SMALL), allocate with slack
 * for the extra descriptor AllocatePool's own allocation can introduce,
 * then fetch for real. On success, *out_map is caller-owned and must be
 * freed with system_table->BootServices->FreePool(). On failure, no
 * allocation is left outstanding.
 */
EFI_STATUS hype_memmap_get(EFI_BOOT_SERVICES *bs,
                            EFI_MEMORY_DESCRIPTOR **out_map,
                            UINTN *out_map_size,
                            UINTN *out_desc_size,
                            UINTN *out_map_key);

/*
 * Prints one line per descriptor (index, type, physical start, page
 * count) via ConOut. desc_size is the stride between entries (NOT
 * necessarily sizeof(EFI_MEMORY_DESCRIPTOR) -- the spec allows firmware
 * to report a larger, versioned descriptor).
 */
void hype_memmap_dump(EFI_SYSTEM_TABLE *system_table,
                       const EFI_MEMORY_DESCRIPTOR *map,
                       UINTN map_size,
                       UINTN desc_size);

/*
 * M1-4: calls ExitBootServices(), handling the spec-mandated retry --
 * if the memory map changes between GetMemoryMap() and
 * ExitBootServices() (map_key goes stale), ExitBootServices() fails and
 * boot services are still active, so the correct response is to fetch a
 * fresh map and try again (this converges quickly in practice; nothing
 * bounds the retry count because nothing in the spec does either -- a
 * system where this never converges has bigger problems than this loop).
 *
 * On success, boot services (including ConOut) are gone -- the caller
 * must not touch system_table->BootServices or system_table->ConOut
 * again. On failure (a GetMemoryMap error unrelated to staleness), boot
 * services are still usable and no allocation is left outstanding.
 */
EFI_STATUS hype_exit_boot_services(EFI_HANDLE image_handle, EFI_BOOT_SERVICES *bs);

/*
 * ADM-1: total bytes of memory that become available for our own use
 * once Boot Services are gone -- EfiConventionalMemory (already free)
 * plus EfiBootServicesCode/Data (only reserved while firmware is still
 * using it). Deliberately excludes EfiLoaderCode/Data (our own loaded
 * image), ACPI/Runtime/Reserved/MMIO regions (not general-purpose RAM).
 * Pure arithmetic over an already-fetched map -- no UEFI calls, no
 * allocation.
 */
UINT64 hype_memmap_usable_bytes(const EFI_MEMORY_DESCRIPTOR *map, UINTN map_size, UINTN desc_size);


/*
 * #290: the largest SINGLE contiguous EfiConventionalMemory block, in bytes.
 *
 * AllocateAnyPages needs one contiguous run, so a request can fail with
 * gigabytes free -- measured on this project: a 524800-page (2051 MiB) guest RAM
 * allocation failed against a largest free block of 522626 pages, short by 2174
 * pages with plenty free elsewhere in the map. An EFI_OUT_OF_RESOURCES on a host
 * with free memory reads as a hype bug until you know that, and establishing it
 * previously took a manual memory-map read.
 *
 * Only EfiConventionalMemory counts. BootServicesCode/Data are included in
 * hype_memmap_usable_bytes()'s total because they become free after
 * ExitBootServices, but they are NOT available to an AllocatePages call made
 * before it -- and the failures this exists to explain all happen pre-EBS.
 *
 * Pure. Adjacent descriptors of the same type are NOT merged: firmware may split
 * one physical run across several entries, so this is a lower bound on the true
 * largest run, which is the safe direction for a diagnostic (it can understate
 * what is available, never overstate).
 */
UINT64 hype_memmap_largest_conventional_bytes(const EFI_MEMORY_DESCRIPTOR *map, UINTN map_size,
                                              UINTN desc_size);

#endif /* HYPE_MEMMAP_H */