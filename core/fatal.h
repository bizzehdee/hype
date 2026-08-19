#ifndef HYPE_FATAL_H
#define HYPE_FATAL_H

#include "efi_types.h"
#include "log_level.h"
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

/*
 * #533: hype_debug_print() above is unconditional and stays that way -- it is the DEBUG level, and
 * every one of its ~600 existing call sites keeps today's behaviour rather than being reclassified
 * by guesswork. What a level buys is the ability to keep the lines an operator needs when something
 * goes wrong and drop the rest, so the loud ones are marked explicitly with HYPE_LOGF() and the
 * unmarked remainder is debug by definition. That is documented rather than implicit: an unmarked
 * line is a debug line.
 */
void hype_debug_print_always(const char *fmt, ...);
void hype_debug_vprint_always(const char *fmt, va_list ap);
void hype_debug_set_level(hype_log_level_t level);
hype_log_level_t hype_debug_get_level(void);
int hype_debug_level_enabled(hype_log_level_t level);

/* Emit at `lvl`. A statement, so it reads like a call and costs nothing when filtered out. */
#define HYPE_LOGF(lvl, ...)                                                                        \
    do {                                                                                           \
        if (hype_debug_level_enabled(lvl)) {                                                       \
            hype_debug_print_always(__VA_ARGS__);                                                         \
        }                                                                                          \
    } while (0)

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

/* Testable, atomic state behind the debug GOP tee. The BSP disables the tee
 * before starting guest APs. Every core then observes one renderer owner.
 * `note_gop_write` is called immediately before the tee changes the shadow. */
int hype_debug_gop_is_enabled(void);
void hype_debug_note_gop_write(void);
unsigned long long hype_debug_gop_write_count(void);

/*
 * #461: a core that takes an unhandled fault halts ALONE -- hype_fatal() masks interrupts and
 * halts the calling core, and every other core carries on. The log therefore keeps flowing past
 * a PANIC line, which reads as "hype survived" when a VM has in fact just lost its vCPU and the
 * dashboard is still drawing it as `running`.
 *
 * The dying core records itself here before panicking; the survivors read it and say so.
 */
void hype_fatal_note_core_panic(unsigned int apic_id);
unsigned int hype_fatal_core_panic_count(void);
unsigned int hype_fatal_core_panic_apic(void); /* the most recent panicking core */

#endif /* HYPE_FATAL_H */
