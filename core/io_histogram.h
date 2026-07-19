#ifndef HYPE_CORE_IO_HISTOGRAM_H
#define HYPE_CORE_IO_HISTOGRAM_H

#include <stdint.h>

/*
 * PERF-1: a bounded per-port I/O-exit histogram. Every guest IN/OUT that
 * #VMEXITs (SVM IOIO intercept) is recorded by its 16-bit port number, so the
 * dominant source of the ~350k+ IOIO exits an Alpine boot generates can be
 * named exactly (serial 0x3F8/0x2F8 vs PCI-config 0xCF8/0xCFC vs PIT/PIC/CMOS
 * vs the 0x80 delay port vs AHCI legacy ports) instead of guessed. The current
 * dispatcher only counts port 0x80 and AHCI-NPF separately, which cannot split
 * the remaining IOIO load -- this closes that measurement gap before any
 * optimisation is chosen.
 *
 * Pure logic (plain array math, no CPU/VMCB/UEFI state) so it is unit-testable
 * and the array can be sized down in tests. The full port space is 2^16, so a
 * direct-indexed uint32 count array is exactly HYPE_IO_HIST_PORTS*4 bytes --
 * fixed and bounded, no eviction/rehash to get wrong.
 */

#define HYPE_IO_HIST_PORTS 0x10000u /* one counter per 16-bit I/O port */

typedef struct {
    uint16_t port;
    uint32_t count;
} hype_io_hist_entry_t;

/*
 * Record one I/O exit on `port`. `counts` is a caller-owned array of at least
 * `nports` uint32 counters (HYPE_IO_HIST_PORTS in production; smaller in tests).
 * Saturates at UINT32_MAX rather than wrapping, so a hot port never rolls back
 * to a small value mid-measurement. A port >= nports is ignored (defensive; the
 * production array covers the whole 16-bit space so this cannot happen there).
 */
void hype_io_hist_record(uint32_t *counts, unsigned nports, uint16_t port);

/*
 * Fill `out` (capacity `max_out`) with the highest-count ports in descending
 * count order, skipping zero-count ports. Returns the number of entries written
 * (<= max_out, fewer if fewer than max_out ports were ever hit). Ties break by
 * lower port number first (stable, deterministic output). Pure read -- does not
 * modify `counts`.
 */
unsigned hype_io_hist_top(const uint32_t *counts, unsigned nports, hype_io_hist_entry_t *out,
                          unsigned max_out);

/* Sum of all counters -- a sanity cross-check against the dispatcher's own IOIO
 * exit total (they should match within any concurrent-record slop). */
uint64_t hype_io_hist_total(const uint32_t *counts, unsigned nports);

#endif /* HYPE_CORE_IO_HISTOGRAM_H */
