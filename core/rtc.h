#ifndef HYPE_CORE_RTC_H
#define HYPE_CORE_RTC_H

#include <stdint.h>

/*
 * Host wall-clock time, and the filesystem timestamp encodings that need it.
 *
 * hype had no wall-clock source at all before this: the EFI system table's
 * RuntimeServices.GetTime is declared as an untyped `void *` and never called,
 * so every directory entry hype wrote carried a zero (FAT32) or 1980-01-01
 * (exFAT) timestamp. That is why hype's own log on a FAT32 volume shows up as
 * the Unix epoch in a file manager.
 *
 * The source is the CMOS RTC (ports 0x70/0x71) rather than UEFI's GetTime,
 * because the writers that need it run POST-ExitBootServices, where Boot
 * Services are gone and calling a Runtime Service means having retained the
 * pointer and accepting whatever state the firmware left it in. hype already
 * owns the hardware at that point and does raw port I/O elsewhere, so reading
 * the RTC directly is both simpler and less dependent on firmware behaviour.
 *
 * Split pure/hw as the rest of the tree is (serial.c/serial_hw.c,
 * ahci_host.c/ahci_host_hw.c): everything here is pure and unit-tested, and
 * rtc_hw.c holds the port I/O.
 */

typedef struct {
    uint16_t year;  /* full year, e.g. 2026 */
    uint8_t month;  /* 1-12 */
    uint8_t day;    /* 1-31 */
    uint8_t hour;   /* 0-23 */
    uint8_t minute; /* 0-59 */
    uint8_t second; /* 0-59 */
} hype_rtc_time_t;

/* CMOS register indices this reads. */
#define HYPE_RTC_REG_SECOND 0x00u
#define HYPE_RTC_REG_MINUTE 0x02u
#define HYPE_RTC_REG_HOUR 0x04u
#define HYPE_RTC_REG_DAY 0x07u
#define HYPE_RTC_REG_MONTH 0x08u
#define HYPE_RTC_REG_YEAR 0x09u
#define HYPE_RTC_REG_STATUS_A 0x0Au
#define HYPE_RTC_REG_STATUS_B 0x0Bu
#define HYPE_RTC_REG_CENTURY 0x32u

/* Status A bit 7: an update is in progress and the time registers are in flux. */
#define HYPE_RTC_STATUS_A_UIP 0x80u
/* Status B bit 1: hours are 24-hour. Bit 2: values are binary, not BCD. */
#define HYPE_RTC_STATUS_B_24H 0x02u
#define HYPE_RTC_STATUS_B_BINARY 0x04u

/* Packed BCD (0x59 == 59) to binary. Pure. */
uint8_t hype_rtc_bcd_to_bin(uint8_t v);

/*
 * Decodes six raw RTC register values into a hype_rtc_time_t, honouring
 * Status B: BCD-vs-binary, and 12-hour mode (where bit 7 of the hour register
 * is the PM flag, 12am is encoded as 12 and must become 0, and 12pm stays 12).
 *
 * `century` is register 0x32 when the FADT says it exists, else 0 -- in which
 * case a two-digit year is windowed to 2000..2099. That window is a deliberate
 * choice, not an oversight: this project's timestamps only need to be plausible
 * and monotonic for log files, and a machine whose RTC lacks a century register
 * and is set before 2000 is not a case worth carrying complexity for.
 *
 * Returns 0 on success, -1 if the decoded fields are not a valid date/time
 * (which is the interesting failure -- a dead or unset RTC often reads as all
 * zeroes, and month 0 / day 0 must NOT be passed on to a filesystem encoder).
 * Pure.
 */
int hype_rtc_decode(uint8_t sec, uint8_t min, uint8_t hour, uint8_t day, uint8_t month,
                    uint8_t year, uint8_t century, uint8_t status_b, hype_rtc_time_t *out);

/* Is this a date/time a filesystem can legally encode? Both FAT and exFAT use
 * 1-based month/day and a 1980 epoch, so 1980-01-01 is the floor. Pure. */
int hype_rtc_time_valid(const hype_rtc_time_t *t);

/*
 * FAT32 directory-entry date/time encodings (FAT spec, sec 6):
 *   date = ((year - 1980) << 9) | (month << 5) | day
 *   time = (hour << 11) | (minute << 5) | (second / 2)   [2-second resolution]
 *   tenths = tenths of a second, 0-199, spanning the odd second the time field
 *            cannot represent.
 * Return 0 for a time outside the encodable range rather than a wrapped value,
 * so a bad clock produces the same "unset" entry the code produced before
 * rather than a confidently wrong date. Pure.
 */
uint16_t hype_fat_encode_date(const hype_rtc_time_t *t);
uint16_t hype_fat_encode_time(const hype_rtc_time_t *t);
uint8_t hype_fat_encode_time_tenths(const hype_rtc_time_t *t);

/*
 * exFAT timestamp (spec sec 7.4): one 32-bit field,
 *   bits 0-4 second/2, 5-10 minute, 11-15 hour, 16-20 day, 21-24 month,
 *   bits 25-31 year - 1980.
 * Returns HYPE_EXFAT_TIMESTAMP_EPOCH's value (1980-01-01 00:00:00) for an
 * invalid time -- NOT zero, because exFAT's month and day are 1-based and an
 * all-zero timestamp is out of spec and trips fsck. Pure.
 */
uint32_t hype_exfat_encode_timestamp(const hype_rtc_time_t *t);

/*
 * exFAT's Create/LastModified10msIncrement byte: the timestamp field holds
 * second/2, and this carries the odd second back as 100 x 10ms (the RTC has no
 * sub-second resolution). 0 for an invalid time. Pure.
 */
uint8_t hype_exfat_encode_10ms(const hype_rtc_time_t *t);

/*
 * Advances a wall-clock snapshot by `seconds` of real, calendar-aware time
 * (leap years included). This is how long-lived writers keep timestamps
 * moving without re-touching hardware: the RTC is read ONCE at boot on the
 * BSP, and later stamps derive from that snapshot plus TSC-measured elapsed
 * seconds -- pure arithmetic, safe from any context, where re-reading the
 * CMOS from an AP mid-flight would repeat the #229/#239 class of mistake.
 * An invalid base yields an invalid (zeroed) result. Pure.
 */
void hype_rtc_advance(const hype_rtc_time_t *base, uint64_t seconds, hype_rtc_time_t *out);

/*
 * Reads the host CMOS RTC. Retries across an update-in-progress window and
 * re-reads to guard against a rollover mid-read (the classic RTC hazard:
 * reading 01:59:59 then 02:00:00's minute field yields 01:00:59). Returns 0 and
 * fills *out on success, -1 if the RTC could not be read as a valid time.
 *
 * Not pure and not unit-tested (port I/O); the decode it delegates to is.
 * Lives in rtc_hw.c.
 */
int hype_rtc_read(hype_rtc_time_t *out);

#endif /* HYPE_CORE_RTC_H */
