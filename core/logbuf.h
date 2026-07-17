#ifndef HYPE_CORE_LOGBUF_H
#define HYPE_CORE_LOGBUF_H

/*
 * In-memory capture of everything hype prints to its console
 * (hype_debug_print tees into this). Its purpose is real-hardware
 * debugging without a serial port: the FW-1 guest runs before
 * ExitBootServices(), so boot/main.c flushes this buffer to a file on
 * the volume hype.efi was loaded from (via core/file_io.h) just before
 * exiting Boot Services. The tester then reads the complete, exact log
 * off the USB stick instead of photographing a wrapping framebuffer.
 *
 * A plain linear buffer: appends stop (setting a truncated flag) once
 * capacity is reached rather than wrapping, so the START of the log is
 * always intact -- the capacity is sized well above a full boot's
 * output, so in practice the whole run, including the trailing giveup
 * diagnostics, is captured. Pure logic, fully unit tested.
 */

#define HYPE_LOGBUF_CAPACITY (2u * 1024u * 1024u)

/* Reset the buffer to empty (used by tests; boot never needs it since
 * the buffer starts zero-initialized in BSS). */
void hype_logbuf_reset(void);

/* Append a NUL-terminated string. Drops any bytes past capacity and
 * latches hype_logbuf_truncated(). */
void hype_logbuf_append(const char *s);

/* The captured bytes (NOT NUL-terminated; use hype_logbuf_len()). */
const char *hype_logbuf_data(void);

/* Number of captured bytes (<= HYPE_LOGBUF_CAPACITY). */
unsigned int hype_logbuf_len(void);

/* Non-zero if any append was dropped for lack of capacity. */
int hype_logbuf_truncated(void);

#endif /* HYPE_CORE_LOGBUF_H */
