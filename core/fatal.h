#ifndef HYPE_FATAL_H
#define HYPE_FATAL_H

#include "efi_types.h"
#include "gop_text.h"

/*
 * M1-7: the one panic handler, via M1-5 (serial, always available from
 * shortly after boot onward) and M1-6 (GOP, if a console was found and
 * registered here). Supersedes the transitional ConOut-based
 * hype_panic() this project used before serial/GOP existed -- ConOut
 * needs firmware in a good state, which isn't guaranteed once Boot
 * Services are gone (see arch/x86_64/cpu/isr_entry.c's history for why
 * that distinction is load-bearing, not cosmetic), whereas serial works
 * identically before and after ExitBootServices().
 */

/* Registers the GOP console hype_fatal() also prints to, if any (call
 * once GOP is initialized in boot/main.c; leave unset if none was
 * found). hype_fatal_get_gop() exists for testing the roundtrip. */
void hype_fatal_set_gop(hype_gop_console_t *con);
hype_gop_console_t *hype_fatal_get_gop(void);

/*
 * Registers the raw GOP protocol handle (used by hype_gop_flush()'s
 * Blt() path) and the real hardware framebuffer address (used by its
 * post-ExitBootServices memcpy fallback) -- found necessary via real-
 * hardware FW-1 testing: hype_gop_console_t's own framebuffer pointer
 * is now a shadow buffer in ordinary RAM (see boot/main.c's console-
 * init site), so every hype_debug_print()/hype_fatal() call needs a
 * way to flush that shadow buffer onto the real screen after printing
 * into it. Call once alongside hype_fatal_set_gop(); call again with
 * `gop=0` (real_fb unchanged) right after ExitBootServices() succeeds
 * -- Blt() is a Boot-Services-era protocol call, unsafe to use
 * afterward, unlike a direct write to the real framebuffer address,
 * which stays valid indefinitely.
 */
void hype_fatal_set_gop_protocol(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, void *real_fb);
EFI_GRAPHICS_OUTPUT_PROTOCOL *hype_fatal_get_gop_protocol(void);
void *hype_fatal_get_real_fb(void);

/* A hook hype_fatal() calls just before halting, so a mid-run panic
 * still flushes the captured console log to disk. Registered by
 * boot/main.c; unset (NULL) by default. */
typedef void (*hype_flush_hook_t)(void);
void hype_fatal_set_flush_hook(hype_flush_hook_t hook);
hype_flush_hook_t hype_fatal_get_flush_hook(void);

/*
 * Formats fmt/... as "PANIC: <message>", prints it via serial and (if
 * registered) the GOP console, then halts forever. Never returns. Not
 * unit tested: it ends in the noreturn hype_halt_forever(), so calling
 * it in a test would hang the test binary rather than verify anything
 * -- same reasoning as hype_halt_forever() itself (halt.h).
 */
__attribute__((noreturn)) void hype_fatal(const char *fmt, ...);

/*
 * Non-fatal sibling of hype_fatal(): formats fmt/... and prints it via
 * serial and (if registered) the GOP console, same two channels, but
 * returns normally instead of halting. Added for real-hardware
 * bring-up: a screen-only setup (no serial capture) previously had no
 * way to see any of the fine-grained "about to do X" / "X done"
 * checkpoints that only ever went to hype_serial_print() -- meaning a
 * hang partway through a risky real-hardware-only sequence (enabling
 * SVM, VMRUN, ...) looked identical to one after it. Not unit tested,
 * same reasoning as hype_fatal() (halt.c) -- it's a thin wrapper around
 * hype_serial_print()/hype_gop_print(), both themselves exempt.
 */
void hype_debug_print(const char *fmt, ...);

/* RT-2c: defer hype_debug_print()'s framebuffer push (still renders to the
 * shadow buffer) so a hot loop can batch VRAM flushes on its own cadence via
 * hype_debug_flush_gop(). deferred=0 restores immediate per-print flushing.
 * hype_fatal() flushes unconditionally, so panics are never deferred away. */
void hype_debug_set_gop_deferred(int deferred);
void hype_debug_flush_gop(void);
/* Rendering isolation: disable (0) hype_debug_print's tee to the GOP framebuffer
 * once the guest loop's terminal renderer owns the screen, so relayed VM serial +
 * diagnostics go only to serial+logbuf and never bleed onto the focused view.
 * Re-enable (1) is unused today (boot starts enabled). Panics paint GOP directly. */
void hype_debug_set_gop_enabled(int enabled);

#endif /* HYPE_FATAL_H */
