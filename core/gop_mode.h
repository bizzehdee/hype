#ifndef HYPE_CORE_GOP_MODE_H
#define HYPE_CORE_GOP_MODE_H

#include <stdint.h>

#include "efi_types.h"

/*
 * TERM-7 (#443): the pure half of GOP resolution selection -- matching a
 * requested WxH against the modes an EFI_GRAPHICS_OUTPUT_PROTOCOL actually
 * reports, with no UEFI call in this file at all (mirrors this project's own
 * pure/hardware split, e.g. ahci_host.c vs ahci_host_hw.c). The hardware side
 * that actually calls QueryMode/SetMode lives in gop_mode_hw.c, coverage-
 * exempt per AGENTS.md for the same reason every other `_hw.c` is.
 */

#define HYPE_GOP_MODE_MAX 64u /* generous: real hardware rarely reports more than a few dozen */

typedef struct {
    uint32_t mode_number; /* what SetMode(mode_number) expects -- NOT a WxH pair, several modes
                            * can share one WxH at different pixel formats/stride */
    uint32_t width;
    uint32_t height;
} hype_gop_mode_t;

/*
 * Finds the first mode in `modes[0..count)` matching `width`x`height`. Returns
 * its index, or -1 if none matches. Pure: no ordering/uniqueness assumed
 * about `modes` beyond what the caller actually enumerated.
 */
int hype_gop_mode_find(const hype_gop_mode_t *modes, unsigned int count, uint32_t width,
                       uint32_t height);

/*
 * Hardware side (gop_mode_hw.c, coverage-exempt): enumerates every mode `gop`
 * reports via QueryMode into `out[0..cap)`, returning how many were written
 * (capped at `cap`; a firmware reporting more than HYPE_GOP_MODE_MAX is
 * truncated, not overflowed). A mode QueryMode itself refuses to describe is
 * simply skipped, not an enumeration failure.
 */
unsigned int hype_gop_mode_enumerate(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, hype_gop_mode_t *out,
                                     unsigned int cap);

/*
 * #465: parses "<W>x<H>" (case-insensitive separator) into `out_width`/`out_height`. Returns 0
 * on success, -1 for anything malformed -- no separator, an empty or non-numeric half, a zero
 * dimension, or a value too large for the field. Pure; see gop_mode.c for the bug this exists
 * to have a test for.
 */
int hype_gop_mode_parse_wxh(const char *s, uint32_t *out_width, uint32_t *out_height);

/*
 * #529 (plan.md section 10 decision 44): the mode closest to target_w x target_h, or -1 when the
 * list is empty. hype has no resolution config key -- it aims at 1920x1080 on every host and
 * takes the nearest thing offered.
 *
 * Closest means the smallest difference in TOTAL PIXELS, tie-broken toward the wider mode. Pixel
 * count rather than per-axis distance because it ranks 1920x1200 above 1600x900 for a 1080p
 * target, which is what an operator looking at the panel would call closer; the tie-break keeps
 * the choice deterministic when two modes are equidistant.
 */
int hype_gop_mode_find_nearest(const hype_gop_mode_t *modes, unsigned int count, uint32_t target_w,
                               uint32_t target_h);

/* Hardware side: applies `mode_number` (as returned by hype_gop_mode_enumerate, NOT a raw
 * WxH) via SetMode. Returns 0 on success, -1 if the firmware refused it. */
int hype_gop_mode_set(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, uint32_t mode_number);

#endif /* HYPE_CORE_GOP_MODE_H */
