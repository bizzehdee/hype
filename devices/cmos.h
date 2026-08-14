#ifndef HYPE_DEVICES_CMOS_H
#define HYPE_DEVICES_CMOS_H

#include <stdint.h>

/*
 * Minimal CMOS/RTC device model (FW-1) -- confirmed necessary via
 * source-level investigation of this project's own vendored OVMF
 * (edk2/OvmfPkg/Library/PlatformInitLib/MemDetect.c,
 * PlatformGetSystemMemorySizeBelow4gb()): if fw_cfg's "etc/e820" file
 * doesn't describe a nonzero low-memory size, OVMF falls back to
 * reading CMOS registers 0x34/0x35 ("system memory above 16MB, in
 * 64KB chunks, high/low byte") via the classic index/data port pair.
 * Without this, an unhandled port 0x70/0x71 access reads back all-1s
 * (this project's generic "unhandled IOIO" default), giving OVMF a
 * wildly wrong memory size to work with.
 *
 * Standard PC chipset convention: port 0x70 (write-only in this
 * project's scope) selects a register by index (bit 7 of the written
 * byte is conventionally the NMI-disable bit, not part of the index --
 * masked off here since this project has no NMI model to disable);
 * port 0x71 reads/writes the selected register's own byte in a
 * 128-byte register file. This project only ever gives registers
 * 0x34/0x35 a meaningful value (the real memory-size fallback path).
 *
 * #286: the RTC half is NOT optional after all, and "irrelevant to boot correctness" was
 * wrong. Register D's VRT bit ("RAM and time are valid") read back as 0 because every
 * register defaulted to 0, and EDK2's PcRtc treats that as fatal: RtcWaitToUpdate()
 * returns EFI_DEVICE_ERROR whenever VRT is clear, so EVERY RTC read failed. A release
 * OVMF ignores the error and carries on; a DEBUG build turns it into
 * `ASSERT_EFI_ERROR (Status = Device Error)` at PcRtcEntry.c:181 and dead-loops -- which
 * is exactly why a DEBUG guest firmware never reached its console. A day-zero,
 * month-zero date also fails PcRtc's own RtcTimeFieldsValid().
 */

#define HYPE_CMOS_SIZE 128u
#define HYPE_CMOS_INDEX_MASK 0x7Fu
#define HYPE_CMOS_REG_EXTMEM_LOW 0x34u
#define HYPE_CMOS_REG_EXTMEM_HIGH 0x35u

/* RTC time/date + status registers (standard MC146818 layout). */
#define HYPE_CMOS_REG_SECONDS 0x00u
#define HYPE_CMOS_REG_MINUTES 0x02u
#define HYPE_CMOS_REG_HOURS 0x04u
#define HYPE_CMOS_REG_DAY_OF_WEEK 0x06u
#define HYPE_CMOS_REG_DAY 0x07u
#define HYPE_CMOS_REG_MONTH 0x08u
#define HYPE_CMOS_REG_YEAR 0x09u
#define HYPE_CMOS_REG_STATUS_A 0x0Au
#define HYPE_CMOS_REG_STATUS_B 0x0Bu
#define HYPE_CMOS_REG_STATUS_C 0x0Cu

/*
 * #436: register B's interrupt-enable bits and register C's flags. hype stored
 * both registers faithfully but never RAISED anything, so a guest that enabled
 * the periodic interrupt and waited for IRQ8 waited forever -- the registers
 * described an interrupting RTC that could not interrupt.
 */
#define HYPE_CMOS_STATUS_B_PIE (1u << 6) /* periodic interrupt enable */
#define HYPE_CMOS_STATUS_C_PF (1u << 6)  /* periodic interrupt flag */
#define HYPE_CMOS_STATUS_C_IRQF (1u << 7) /* an enabled interrupt is asserted */
#define HYPE_CMOS_REG_STATUS_D 0x0Du
#define HYPE_CMOS_REG_CENTURY 0x32u

/*
 * Reset values for the status registers.
 *
 * A = 0x26: the conventional divider (010) + rate selector (0110), with UIP clear -- an
 * update is never "in progress" here because the register file is only written between
 * guest accesses, so a guest polling for UIP to clear always succeeds immediately.
 *
 * B = 0x02: 24-hour mode, BCD time (DM clear). BCD because that is what a real PC and
 * QEMU default to, so guest firmware that skips reading register B still decodes
 * correctly.
 *
 * D = 0x80: VRT set. See the header comment -- this single bit is what a DEBUG OVMF
 * dead-loops on when it is clear.
 */
#define HYPE_CMOS_STATUS_A_RESET 0x26u
#define HYPE_CMOS_STATUS_B_RESET 0x02u
#define HYPE_CMOS_STATUS_D_RESET 0x80u
#define HYPE_CMOS_STATUS_B_BINARY 0x04u /* DM: time is binary, not BCD */
#define HYPE_CMOS_STATUS_B_24HOUR 0x02u

/*
 * #304: the RTC has to TICK.
 *
 * #286 seeded the time/date registers once at VM setup and nothing updated them, so every
 * guest read returned the same instant and any guest computing `now - start` got 0 for
 * ever. FreeBSD's loader menu never reached its "Autoboot in 10 seconds" countdown --
 * 15.8M ACPI-PM-timer reads and no progress -- and Alpine's `date` reported its image's
 * build year. A frozen clock is a different defect from #286's invalid one, and fixing
 * that exposed this.
 *
 * The base snapshot plus a caller-supplied elapsed-seconds count is all this needs; the
 * calendar arithmetic is hype_rtc_advance()'s (core/rtc.h), and keeping the elapsed
 * measurement outside this file is what keeps the model free of any platform clock. The
 * whole time is recomputed and re-encoded in ONE call so a guest cannot read a rolled
 * minute against an unrolled hour -- real hardware exposes that hazard through UIP, which
 * hype deliberately holds clear (#286), so the snapshot has to be atomic here instead.
 */
typedef struct {
    uint8_t index;
    uint8_t registers[HYPE_CMOS_SIZE];
    /* Seeded wall clock, and whether it is usable. Kept alongside the register file so a
     * re-encode never has to go back to the host. */
    uint16_t base_year;
    uint8_t base_month, base_day, base_hour, base_minute, base_second;
    int base_valid;
    /* #436: nanoseconds accumulated toward the next periodic interrupt. */
    uint64_t periodic_ns;
} hype_cmos_t;

/*
 * Resets to index 0 and every register 0, EXCEPT the RTC status registers, which get the
 * values above. Call on every (re)start, same convention as every other device model here.
 *
 * The status registers are not zeroed because zero is not a legal power-on state for them:
 * VRT clear means "the time is not valid", and guest firmware is entitled to treat that as
 * a broken RTC (EDK2 does, fatally, in a DEBUG build).
 */
void hype_cmos_reset(hype_cmos_t *cmos);

/*
 * Seed the RTC time/date registers, honouring register B's DM bit (BCD by default) and
 * 24-hour bit. `year` is the full year; register 0x09 gets the two low digits and the
 * century register 0x32 the rest, which is what firmware that reads a century register
 * expects.
 *
 * Values are range-checked and a nonsensical date is REFUSED (returns -1) rather than
 * written: month 0 or day 0 fails EDK2's own RtcTimeFieldsValid(), so writing one would
 * reproduce the very failure this exists to prevent. Returns 0 on success.
 */
int hype_cmos_set_time(hype_cmos_t *cmos, unsigned int year, unsigned int month,
                       unsigned int day, unsigned int hour, unsigned int minute,
                       unsigned int second);

/*
 * #304: re-encode the time/date registers as the seeded base advanced by `elapsed_seconds`.
 *
 * Call before answering a guest read of a time/date register; the caller owns the elapsed
 * measurement (a TSC delta), which is what keeps this module free of a platform clock, the
 * same split hype_blk_wstats_set_clock() uses. A no-op if no valid base was seeded -- a
 * guest reading a clock hype never set should see the unchanging zeros it had before,
 * not an invented date that drifts.
 *
 * Idempotent for a given `elapsed_seconds`, so calling it on every register read of a
 * multi-register sequence yields a self-consistent time as long as the caller passes the
 * same value for that sequence.
 */
void hype_cmos_advance_to(hype_cmos_t *cmos, uint64_t elapsed_seconds);

/*
 * Populates registers 0x34/0x35 with `size_64kb_units` (the standard
 * "memory above 16MB, in 64KB chunks" encoding
 * PlatformGetSystemMemorySizeBelow4gb() itself decodes) -- the
 * caller's job to compute this from the actual amount of memory this
 * guest should believe it has. Pure struct mutation.
 */
void hype_cmos_set_extended_memory_above_16mb(hype_cmos_t *cmos, uint16_t size_64kb_units);

/* Port 0x70 write: selects a register (bits 0-6; bit 7, conventionally
 * NMI-disable, is masked off -- not modeled). Pure struct mutation. */
void hype_cmos_index_write(hype_cmos_t *cmos, uint8_t value);

/*
 * Port 0x71 read: returns the currently-selected register's byte. Reading
 * status register C acknowledges its latched interrupt flags, as an MC146818
 * does, so this accessor may update the register file.
 */
uint8_t hype_cmos_data_read(hype_cmos_t *cmos);

/*
 * Port 0x71 write: stores into the currently-selected register, EXCEPT for the read-only
 * status bits -- register D's VRT is preserved, register A's UIP is held clear, and
 * register C is not writable at all. Pure struct mutation.
 *
 * #286: this is not pedantry. EDK2's PcRtcInit() writes 0 to register D and then requires
 * VRT to still be set; a plain read/write model let the firmware destroy the bit it was
 * about to check, which dead-looped a DEBUG OVMF and left a release one running on a clock
 * it had been told was invalid.
 */
void hype_cmos_data_write(hype_cmos_t *cmos, uint8_t value);

/*
 * #436: the periodic-interrupt rate register A's rate selector encodes, in Hz.
 * Rate 0 means "no periodic interrupt"; 1 and 2 are the 256 Hz aliases the
 * hardware defines, and 3..15 halve from 8192 Hz down. Returns 0 when no rate
 * is selected.
 */
uint32_t hype_cmos_periodic_hz(const hype_cmos_t *cmos);

/*
 * Advance the RTC by `elapsed_ns` and report whether the periodic interrupt is
 * now asserted. When it fires, register C's PF and IRQF are latched exactly as
 * the hardware latches them -- and, as on hardware, reading register C clears
 * them and drops the line. Returns 1 the moment the line is newly asserted so
 * the caller can raise IRQ8, 0 otherwise.
 */
int hype_cmos_advance(hype_cmos_t *cmos, uint64_t elapsed_ns);

#endif /* HYPE_DEVICES_CMOS_H */
