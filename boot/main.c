#include "../core/efi_types.h"
#include "../core/console.h"

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    (void)ImageHandle;

    hype_console_print(SystemTable, "hype\n");

    return EFI_SUCCESS;
}
