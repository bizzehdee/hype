#include "gop.h"

EFI_STATUS hype_gop_locate(EFI_BOOT_SERVICES *bs, EFI_GRAPHICS_OUTPUT_PROTOCOL **out_gop) {
    EFI_GUID guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    void *interface = 0;
    EFI_STATUS status;

    status = bs->LocateProtocol(&guid, 0, &interface);
    if (status != EFI_SUCCESS) {
        return status;
    }

    *out_gop = (EFI_GRAPHICS_OUTPUT_PROTOCOL *)interface;
    return EFI_SUCCESS;
}
