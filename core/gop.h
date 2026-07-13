#ifndef HYPE_GOP_H
#define HYPE_GOP_H

#include "efi_types.h"

/*
 * Locates the Graphics Output Protocol via Boot Services. Must be
 * called before ExitBootServices() -- LocateProtocol is a Boot
 * Services call like GetMemoryMap/AllocatePool, mockable the same way
 * (see memmap.c's hype_memmap_get() tests), so this is fully unit
 * tested despite touching UEFI types.
 */
EFI_STATUS hype_gop_locate(EFI_BOOT_SERVICES *bs, EFI_GRAPHICS_OUTPUT_PROTOCOL **out_gop);

#endif /* HYPE_GOP_H */
