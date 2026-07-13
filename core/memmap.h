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

#endif /* HYPE_MEMMAP_H */
