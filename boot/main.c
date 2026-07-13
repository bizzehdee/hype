#include "../core/efi_types.h"
#include "../core/console.h"
#include "../core/memmap.h"

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_MEMORY_DESCRIPTOR *map = 0;
    UINTN map_size = 0, desc_size = 0, map_key = 0;
    EFI_STATUS status;

    (void)ImageHandle;

    hype_console_print(SystemTable, "hype\n");

    status = hype_memmap_get(SystemTable->BootServices, &map, &map_size, &desc_size, &map_key);
    if (status != EFI_SUCCESS) {
        hype_console_print(SystemTable, "failed to get memory map: 0x%llx\n", (unsigned long long)status);
        return status;
    }

    hype_memmap_dump(SystemTable, map, map_size, desc_size);
    SystemTable->BootServices->FreePool(map);

    return EFI_SUCCESS;
}
