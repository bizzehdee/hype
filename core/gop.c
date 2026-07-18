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

void hype_gop_flush(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, hype_gop_console_t *con, void *real_fb) {
    unsigned int y0, y1, rows;

    /* RT-1c: only copy what changed since the last flush. Skipping a
     * clean console avoids a whole-framebuffer copy on every
     * hype_debug_print/idle iteration; copying just [dirty_y_min,
     * dirty_y_max] turns a one-line print from a full-frame blit into ~8
     * rows -- critical post-ExitBootServices where the real_fb path is a
     * plain pixel copy straight to VRAM. */
    if (!con->dirty) {
        return;
    }
    y0 = con->dirty_y_min;
    y1 = con->dirty_y_max;
    if (y1 >= con->height) {
        y1 = (con->height > 0) ? con->height - 1 : 0;
    }
    if (y0 > y1) {
        con->dirty = 0;
        return;
    }
    rows = y1 - y0 + 1u;

    /* EFI_GRAPHICS_OUTPUT_BLT_PIXEL's own byte order (Blue, Green, Red,
     * Reserved) is fixed by the UEFI spec regardless of the real
     * mode's actual PixelFormat -- Blt() itself converts. This
     * project's own fg/bg (white/black, gop_text.h's own console-init
     * call site) are channel-order-insensitive, so no conversion is
     * needed here; a future caller drawing non-greyscale colors would
     * need to account for this. */
    if (gop != 0) {
        /* Blt a sub-rectangle: source row y0 in con->fb -> the same row on
         * screen (SourceX/Y = DestX/Y = 0,y0; Height = rows). */
        gop->Blt(gop, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)con->fb, EfiBltBufferToVideo, 0, y0, 0, y0,
                 con->width, rows, (UINTN)con->stride * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
        con->dirty = 0;
        return;
    }

    if (real_fb != 0) {
        unsigned int *dst = (unsigned int *)real_fb;
        unsigned int x, y;

        for (y = y0; y <= y1; y++) {
            unsigned long long row = (unsigned long long)y * con->stride;
            for (x = 0; x < con->width; x++) {
                dst[row + x] = con->fb[row + x];
            }
        }
    }
    con->dirty = 0;
}
