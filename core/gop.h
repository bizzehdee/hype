#ifndef HYPE_GOP_H
#define HYPE_GOP_H

#include "efi_types.h"
#include "gop_text.h"

/*
 * Locates the Graphics Output Protocol via Boot Services. Must be
 * called before ExitBootServices() -- LocateProtocol is a Boot
 * Services call like GetMemoryMap/AllocatePool, mockable the same way
 * (see memmap.c's hype_memmap_get() tests), so this is fully unit
 * tested despite touching UEFI types.
 */
EFI_STATUS hype_gop_locate(EFI_BOOT_SERVICES *bs, EFI_GRAPHICS_OUTPUT_PROTOCOL **out_gop);

/*
 * Real-hardware GOP-rendering perf fix (found via real-hardware FW-1
 * testing): pushes `con`'s own framebuffer (expected to be a shadow
 * buffer in ordinary RAM, not the real hardware framebuffer directly
 * -- see boot/main.c's own console-init site) onto the real screen.
 *
 * If `gop` is non-NULL, uses its Blt() function
 * (EfiBltBufferToVideo) -- typically hardware-accelerated/DMA'd by the
 * platform's own GOP driver, and the whole reason this function
 * exists: `con`'s own pixel-plotting logic (gop_text.c) writes into
 * ordinary cached RAM, which is fast regardless of the real hardware's
 * own framebuffer caching attributes, and this is the ONE bulk
 * transfer per flush that actually touches the (potentially
 * uncached/slow-to-access) real framebuffer memory.
 *
 * If `gop` is NULL (the caller's own convention for "Boot Services
 * have exited, Blt() is no longer safe to call" -- see boot/main.c's
 * own hype_fatal_set_gop_protocol(0, ...) call right after
 * ExitBootServices() succeeds), falls back to a direct memory copy
 * into `real_fb` instead -- an ordinary write, safe indefinitely,
 * unlike a Boot-Services-era protocol call. A NULL `real_fb` in that
 * case is a safe no-op (nothing to flush into).
 *
 * Mockable the same way hype_gop_locate() is (a caller-supplied Blt
 * function pointer on a fake EFI_GRAPHICS_OUTPUT_PROTOCOL), so this is
 * fully unit tested despite touching UEFI types.
 */
/* Copies the console backbuffer to the screen. RT-1c: only the dirty
 * pixel-row range accumulated since the last flush is copied (and a clean
 * console is skipped entirely), then the dirty range is cleared -- hence
 * `con` is non-const. */
void hype_gop_flush(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, hype_gop_console_t *con, void *real_fb);

/*
 * #368: blit cost -- cycles, bytes and calls, separated from the cell drawing that precedes it.
 * A rate that is already slow on an IDLE machine means the framebuffer mapping is wrong (an MTRR
 * of UC can defeat a PAT of WC); a rate that collapses only when a guest misbehaves means
 * contention. Those need different fixes and cannot be told apart by eye.
 */
void hype_gop_blit_stats(unsigned long long *tsc, unsigned long long *bytes,
                         unsigned long long *calls);

#endif /* HYPE_GOP_H */
