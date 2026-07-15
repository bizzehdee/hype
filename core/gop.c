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

void hype_gop_flush(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, const hype_gop_console_t *con, void *real_fb) {
    /* EFI_GRAPHICS_OUTPUT_BLT_PIXEL's own byte order (Blue, Green, Red,
     * Reserved) is fixed by the UEFI spec regardless of the real
     * mode's actual PixelFormat -- Blt() itself converts. This
     * project's own fg/bg (white/black, gop_text.h's own console-init
     * call site) are channel-order-insensitive, so no conversion is
     * needed here; a future caller drawing non-greyscale colors would
     * need to account for this. */
    if (gop != 0) {
        gop->Blt(gop, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)con->fb, EfiBltBufferToVideo, 0, 0, 0, 0,
                 con->width, con->height, (UINTN)con->stride * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
        return;
    }

    if (real_fb != 0) {
        unsigned int *dst = (unsigned int *)real_fb;
        unsigned int x, y;

        for (y = 0; y < con->height; y++) {
            unsigned long long row = (unsigned long long)y * con->stride;
            for (x = 0; x < con->width; x++) {
                dst[row + x] = con->fb[row + x];
            }
        }
    }
}
