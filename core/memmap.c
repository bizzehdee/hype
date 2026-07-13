#include "memmap.h"
#include "console.h"

const char *hype_memmap_type_name(UINT32 type) {
    switch (type) {
    case EfiReservedMemoryType: return "Reserved";
    case EfiLoaderCode: return "LoaderCode";
    case EfiLoaderData: return "LoaderData";
    case EfiBootServicesCode: return "BootServicesCode";
    case EfiBootServicesData: return "BootServicesData";
    case EfiRuntimeServicesCode: return "RuntimeServicesCode";
    case EfiRuntimeServicesData: return "RuntimeServicesData";
    case EfiConventionalMemory: return "Conventional";
    case EfiUnusableMemory: return "Unusable";
    case EfiACPIReclaimMemory: return "ACPIReclaim";
    case EfiACPIMemoryNVS: return "ACPIMemoryNVS";
    case EfiMemoryMappedIO: return "MMIO";
    case EfiMemoryMappedIOPortSpace: return "MMIOPortSpace";
    case EfiPalCode: return "PalCode";
    case EfiPersistentMemory: return "Persistent";
    case EfiUnacceptedMemoryType: return "Unaccepted";
    default: return "Unknown";
    }
}

EFI_STATUS hype_memmap_get(EFI_BOOT_SERVICES *bs,
                            EFI_MEMORY_DESCRIPTOR **out_map,
                            UINTN *out_map_size,
                            UINTN *out_desc_size,
                            UINTN *out_map_key) {
    UINTN map_size = 0;
    UINTN desc_size = 0;
    UINT32 desc_version = 0;
    UINTN map_key = 0;
    EFI_MEMORY_DESCRIPTOR *map = 0;
    EFI_STATUS status;

    status = bs->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_version);
    if (status != EFI_BUFFER_TOO_SMALL) {
        return status;
    }

    /* AllocatePool can itself grow the map by an entry; pad generously. */
    map_size += desc_size * 4;

    status = bs->AllocatePool(EfiLoaderData, map_size, (void **)&map);
    if (status != EFI_SUCCESS) {
        return status;
    }

    status = bs->GetMemoryMap(&map_size, map, &map_key, &desc_size, &desc_version);
    if (status != EFI_SUCCESS) {
        bs->FreePool(map);
        return status;
    }

    *out_map = map;
    *out_map_size = map_size;
    *out_desc_size = desc_size;
    *out_map_key = map_key;
    return EFI_SUCCESS;
}

void hype_memmap_dump(EFI_SYSTEM_TABLE *system_table,
                       const EFI_MEMORY_DESCRIPTOR *map,
                       UINTN map_size,
                       UINTN desc_size) {
    UINTN count = (desc_size > 0) ? (map_size / desc_size) : 0;
    UINTN i;
    const UINT8 *base = (const UINT8 *)map;

    hype_console_print(system_table, "memory map: %llu entries\n", (unsigned long long)count);
    for (i = 0; i < count; i++) {
        const EFI_MEMORY_DESCRIPTOR *d = (const EFI_MEMORY_DESCRIPTOR *)(base + i * desc_size);
        hype_console_print(system_table, "  [%llu] %s phys=0x%llx pages=%llu\n",
                            (unsigned long long)i,
                            hype_memmap_type_name(d->Type),
                            (unsigned long long)d->PhysicalStart,
                            (unsigned long long)d->NumberOfPages);
    }
}
