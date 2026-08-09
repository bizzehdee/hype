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

/*
 * #368: how fast is the blit, really?
 *
 * Console rendering slows ~1000x while a guest spins on emulated MMIO, and the guest's CPU cost is
 * now measured at 0.18% of run time (#367, closed), so CPU contention cannot explain it. That
 * leaves the write path itself: this is a plain pixel copy to a high PCIe BAR, and hype ASSERTS in
 * paging.h that PAT WC beats an MTRR of UC there. Asserting is not measuring, and the difference
 * between WC and UC on a PCIe framebuffer is roughly the factor being observed.
 *
 * Bytes and time, separated from the cell drawing that precedes it. Reported by the BSP, so it
 * also shows whether the rate changes when a guest starts misbehaving -- which distinguishes "the
 * mapping is wrong all along" from "the guest starves it", and those need completely different
 * fixes.
 */
static volatile unsigned long long g_blit_tsc, g_blit_bytes, g_blit_calls;

static inline unsigned long long gop_rdtsc(void) {
    unsigned int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

void hype_gop_blit_stats(unsigned long long *tsc, unsigned long long *bytes,
                         unsigned long long *calls) {
    if (tsc != 0) *tsc = g_blit_tsc;
    if (bytes != 0) *bytes = g_blit_bytes;
    if (calls != 0) *calls = g_blit_calls;
}

void hype_gop_flush(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, hype_gop_console_t *con, void *real_fb) {
    unsigned int y0, y1, rows;
    unsigned long long blit_t0;
    unsigned long long band_bytes = 0; /* #368: bytes actually pushed on the per-band path */

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
    blit_t0 = gop_rdtsc();

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
        g_blit_tsc += gop_rdtsc() - blit_t0;
        g_blit_bytes += (unsigned long long)con->width * rows * 4ull;
        g_blit_calls++;
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
                if (bx1 >= bx0) {
                    band_bytes += (unsigned long long)(bx1 - bx0 + 1u) * (by1 - by0 + 1u) * 4ull;
                }
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
            g_blit_tsc += gop_rdtsc() - blit_t0;
            g_blit_bytes += band_bytes;
            g_blit_calls++;
            con->dirty = 0;
            return;
        }

        for (y = y0; y <= y1; y++) {
            unsigned long long row = (unsigned long long)y * con->stride;
            for (x = 0; x < con->width; x++) {
                dst[row + x] = con->fb[row + x];
            }
        }
        g_blit_tsc += gop_rdtsc() - blit_t0;
        g_blit_bytes += (unsigned long long)con->width * rows * 4ull;
        g_blit_calls++;
    }
    bands_clear(con);
    con->dirty = 0;
}
