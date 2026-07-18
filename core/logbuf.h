#ifndef HYPE_CORE_LOGBUF_H
#define HYPE_CORE_LOGBUF_H

#include <stdint.h>

/*
 * In-memory capture of everything hype prints to its console
 * (hype_debug_print tees into this). Its purpose is real-hardware
 * debugging without a serial port.
 *
 * Through M4-6 the guest ran before ExitBootServices(), so boot/main.c
 * flushed this buffer to \hype-log.txt via Boot-Services file I/O. The RT
 * track moves guest execution AFTER ExitBootServices (post-EBS is MP-safe
 * and lets hype preempt guests), where that file I/O is gone -- so this
 * buffer becomes the primary observability channel and must be
 * self-describing: a magic-tagged header (RT-1a) lets a later boot's
 * scanner (RT-1b) locate and validate the captured log in physical RAM
 * and dump it to a file, and lets an on-screen/serial dumper trust it.
 *
 * A plain linear buffer: appends stop (setting a truncated flag) once
 * capacity is reached rather than wrapping, so the START of the log is
 * always intact -- capacity is sized well above a full boot's output, so
 * in practice the whole run is captured. Header + data are one contiguous,
 * 8-byte-aligned region so the magic sits at a fixed offset ahead of the
 * bytes. Pure logic, fully unit tested.
 */

#define HYPE_LOGBUF_CAPACITY (2u * 1024u * 1024u)

/* RT-1d: the buffer is page-aligned (see g_logbuf's __attribute__((aligned))
 * in logbuf.c) and UEFI loads hype.efi's image at a page-aligned physical
 * base, so a previous boot's magic always lands on a 4 KB boundary in RAM.
 * The RT-1b scanner steps by this instead of 8 bytes -- ~512x fewer probes
 * across a multi-GB sweep, which is what removes the pre-EBS scan pause. */
#define HYPE_LOGBUF_SCAN_ALIGN 4096u

/* Distinctive 64-bit tag at the head of the region ("hypeLOG\0") --
 * vanishingly unlikely to occur in random firmware RAM, so a memory scan
 * that finds it (RT-1b) has almost certainly found a real hype log rather
 * than coincidental bytes. */
#define HYPE_LOGBUF_MAGIC 0x00474F4C65707968ULL /* 'h''y''p''e''L''O''G''\0' */
#define HYPE_LOGBUF_VERSION 1u

/*
 * Self-describing capture region. The header precedes the data so a
 * scanner can find the magic and read len/checksum before trusting the
 * bytes. checksum is a simple rolling sum over data[0..len) -- combined
 * with the 64-bit magic + version it's enough to reject coincidental or
 * partially-overwritten memory, not a cryptographic integrity guarantee.
 */
typedef struct {
    uint64_t magic;     /* HYPE_LOGBUF_MAGIC once stamped */
    uint32_t version;   /* HYPE_LOGBUF_VERSION */
    uint32_t len;       /* captured bytes in data[] (<= HYPE_LOGBUF_CAPACITY) */
    uint32_t truncated; /* non-zero if any append was dropped for capacity */
    uint32_t checksum;  /* rolling sum of data[0..len) */
    char data[HYPE_LOGBUF_CAPACITY];
} hype_logbuf_t;

/* Reset to empty AND stamp the magic/version header. Must be called once
 * early in boot (efi_main) before any logging so the region is
 * self-describing from the first byte; also used by tests. */
void hype_logbuf_reset(void);

/* Append a NUL-terminated string. Drops any bytes past capacity and
 * latches hype_logbuf_truncated(); maintains len + checksum. */
void hype_logbuf_append(const char *s);

/* The captured bytes (NOT NUL-terminated; use hype_logbuf_len()). */
const char *hype_logbuf_data(void);

/* Number of captured bytes (<= HYPE_LOGBUF_CAPACITY). */
unsigned int hype_logbuf_len(void);

/* Non-zero if any append was dropped for lack of capacity. */
int hype_logbuf_truncated(void);

/* The live capture region, header included -- boot uses this to place/
 * persist it (RT-1a physical placement, RT-1b dump). */
const hype_logbuf_t *hype_logbuf_get(void);

/*
 * Pure validity check for a candidate header: correct magic + version and
 * a len within capacity whose data checksum matches. Returns 1 if the
 * region is a well-formed hype log, 0 otherwise. Used by hype_logbuf_find()
 * and directly by a next-boot dumper. Fully testable.
 */
int hype_logbuf_validate(const hype_logbuf_t *hdr);

/*
 * Scans [base, base+size) for a valid logbuf header (magic +
 * hype_logbuf_validate), stepping by `stride` bytes. Returns a pointer to
 * the first valid one, or 0 if none. `stride` is clamped up to 8 (the
 * minimum at which the header fields are readable); pass
 * HYPE_LOGBUF_SCAN_ALIGN for a fast real-RAM sweep when `base` is
 * page-aligned (the RT-1b case), or 8 to probe every 8-byte boundary.
 * Pure -- no allocation, no dereference outside [base, base+size). This is
 * how RT-1b recovers the previous run's log from physical RAM on the
 * following boot. Fully testable.
 */
const hype_logbuf_t *hype_logbuf_find(const void *base, unsigned long size, unsigned long stride);

#endif /* HYPE_CORE_LOGBUF_H */
