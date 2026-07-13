#ifndef HYPE_FONT8X8_H
#define HYPE_FONT8X8_H

/*
 * 8x8 monochrome bitmap font, basic Latin (U+0000-U+007F), one byte per
 * row, bit N of each byte set = column N lit.
 *
 * Public domain. Traces back through two prior public-domain works:
 *   - Original: IBM VGA BIOS font (public domain), reworked by
 *     Marcel Sondaar for the MOS3 OS-dev project (font8x8.h, public
 *     domain), fetched from
 *     http://dimensionalrift.homelinux.net/combuster/mos3/?p=viewsource&file=/modules/gfx/font8_8.asm
 *   - Repackaged as font8x8_basic by Daniel Hepper
 *     <daniel@hepper.net> (https://github.com/dhepper/font8x8),
 *     explicitly marked "License: Public Domain" in that file's own
 *     header.
 * Embedded here verbatim (byte-for-byte) rather than transcribed by
 * hand, specifically to avoid the silent-corruption risk of
 * hand-copying dense bitmap data.
 */
extern const unsigned char hype_font8x8_basic[128][8];

#endif /* HYPE_FONT8X8_H */
