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

/* #351: every exit from a flush must leave the band table clean, or stale bands accumulate and
 * the next flush copies regions that are already on screen. */
static void bands_clear(hype_gop_console_t *con) {
    unsigned int b;
    for (b = 0; b < con->band_count; b++) {
        con->band_dirty[b] = 0;
    }
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
        bands_clear(con);
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
        /* Pre-ExitBootServices only: Blt() is firmware-accelerated and one call for the union
         * rectangle beats hundreds of per-band calls, so the row range stays right here. */
        bands_clear(con);
        con->dirty = 0;
        return;
    }

    if (real_fb != 0) {
        unsigned int *dst = (unsigned int *)real_fb;
        unsigned int x, y;

        /*
         * #351: this is the measured hot path. Post-ExitBootServices there is no Blt() to hand
         * the work to -- these are scalar writes into uncached VRAM at ~30 MB/s, so copying
         * [y0,y1] full width cost 271 ms per push and starved the guest to 0.16% of the CPU.
         * Copy only the per-band x extents the renderer actually touched: ~11 changed cells move
         * ~700 pixels instead of 2,073,600.
         */
        if (con->band_count != 0u) {
            unsigned int b;
            for (b = 0; b < con->band_count; b++) {
                unsigned int by0, by1;
                unsigned int bx0, bx1;
                if (!con->band_dirty[b]) {
                    continue;
                }
                con->band_dirty[b] = 0;
                by0 = b * HYPE_GOP_GLYPH_H;
                by1 = by0 + HYPE_GOP_GLYPH_H - 1u;
                if (by1 >= con->height) {
                    by1 = (con->height > 0) ? con->height - 1u : 0u;
                }
                bx0 = con->band_x0[b];
                bx1 = con->band_x1[b];
                if (bx1 >= con->width) {
                    bx1 = (con->width > 0) ? con->width - 1u : 0u;
                }
                if (by0 >= con->height || bx0 > bx1) {
                    continue;
                }
                for (y = by0; y <= by1; y++) {
                    unsigned long long row = (unsigned long long)y * con->stride;
                    for (x = bx0; x <= bx1; x++) {
                        dst[row + x] = con->fb[row + x];
                    }
                }
            }
            con->dirty = 0;
            return;
        }

        for (y = y0; y <= y1; y++) {
            unsigned long long row = (unsigned long long)y * con->stride;
            for (x = 0; x < con->width; x++) {
                dst[row + x] = con->fb[row + x];
            }
        }
    }
    bands_clear(con);
    con->dirty = 0;
}
