#ifndef HYPE_CORE_FAT_WRITE_H
#define HYPE_CORE_FAT_WRITE_H

#include <stdint.h>

#include "rtc.h"

/*
 * #198 (STORAGE: writable FAT32) -- pure write primitives. The read-only reader
 * (core/fat.c) resolves paths to extents; this adds the encode/locate pieces a
 * writer needs: FAT32 table-entry get/set + free scan, 8.3 short-name encoding,
 * directory-entry construction, and FSInfo counter updates. All operate on
 * caller-supplied 512-byte sector buffers (read-modify-write by the block-backed
 * orchestration in a later slice), so they are fully unit-testable with no I/O.
 * 512-byte logical sectors; little-endian on disk.
 */

#define HYPE_FAT32_ENTRIES_PER_SECTOR 128u /* 512 / 4 */
#define HYPE_FAT32_EOC 0x0FFFFFFFu          /* end-of-chain marker */
#define HYPE_FAT_ATTR_ARCHIVE 0x20u
#define HYPE_FAT_ATTR_DIRECTORY 0x10u
#define HYPE_FAT_ATTR_VOLUME_ID 0x08u
#define HYPE_FAT_ATTR_LFN 0x0Fu /* the four low attribute bits together mark an LFN entry */

/* Long File Name geometry (FAT spec: "FAT Long Directory Entries"). */
#define HYPE_FAT_LFN_CHARS 13u /* UCS-2 characters carried per LFN entry */
#define HYPE_FAT_MAX_LFN 255u
#define HYPE_FAT_LFN_LAST 0x40u /* ORed into the sequence number of the run's first
                                 * PHYSICAL entry (the logically last piece) */

/* FAT32 entries are 28-bit; the top 4 bits of the 32-bit slot are reserved and
 * must be preserved across a write. */
uint32_t hype_fat32_entry_get(const uint8_t *fat_sector, unsigned int idx_in_sector);
void hype_fat32_entry_set(uint8_t *fat_sector, unsigned int idx_in_sector, uint32_t value);

/*
 * Locates cluster N's FAT entry: the FAT sector (relative to fat_start_lba) that
 * holds it, and its index within that sector.
 */
void hype_fat32_fat_location(uint32_t cluster, uint64_t fat_start_lba, uint64_t *out_sector_lba,
                             unsigned int *out_idx);

/*
 * Scans a 512-byte FAT sector for the first free (==0) entry among the first
 * `entries` slots. Returns 0 and the index in *out_idx if found, else -1.
 */
int hype_fat32_find_free_in_sector(const uint8_t *fat_sector, unsigned int entries,
                                   unsigned int *out_idx);

/*
 * Encodes `name` (e.g. "HYPELOG.TXT") into an 11-byte 8.3 field: uppercased,
 * split on the last '.', base left-justified in 8 and extension in 3, both
 * space-padded. Over-long components are truncated. Pure.
 */
void hype_fat_shortname_83(const char *name, uint8_t out11[11]);

/*
 * Builds a 32-byte directory entry: 8.3 name, attribute byte, first cluster
 * (split hi/lo per FAT32) and file size.
 *
 * `now` fills the creation/write/access timestamps. Pass 0 (or an invalid time)
 * to leave them zeroed, which is what this did unconditionally before -- and is
 * why hype's own log showed up as the Unix epoch in a file manager. A zero FAT
 * date decodes as year 1980, month 0, day 0: not a real date, so readers render
 * it however they please. Pure.
 */
void hype_fat_dirent_build(uint8_t ent[32], const uint8_t name11[11], uint8_t attr,
                           uint32_t first_cluster, uint32_t size, const hype_rtc_time_t *now);

/* Reads the first cluster + size back out of a 32-byte directory entry. */
uint32_t hype_fat_dirent_cluster(const uint8_t ent[32]);
uint32_t hype_fat_dirent_size(const uint8_t ent[32]);

/* Writes only the first-cluster words of an existing directory entry (used to
 * re-point a moved directory's ".." at its new parent). Pure. */
void hype_fat_dirent_set_cluster(uint8_t ent[32], uint32_t first_cluster);

/*
 * ---- Long File Names (#247) ----
 */

/* The short-name checksum every LFN entry of a run carries (FAT spec ChkSum). */
uint8_t hype_fat_shortname_checksum(const uint8_t name11[11]);

/*
 * 1 if `name` contains only characters FAT allows in a long name (no control
 * characters, none of  \ / : * ? " < > | ), is 1..255 bytes, and is neither
 * "." nor "..". 0 otherwise.
 */
int hype_fat_name_valid(const char *name);

/*
 * 1 if `name` is already a strictly valid 8.3 short name -- uppercase base of
 * 1..8 valid short-name characters, optional '.' + 1..3 character extension --
 * that hype_fat_shortname_83() encodes losslessly, so it needs no LFN run at
 * all. Lowercase or over-long names return 0 (they need LFN entries to keep
 * their real name).
 */
int hype_fat_name_is_83(const char *name);

/*
 * Builds the 11-byte short name for LFN generation attempt `n`: up to six
 * usable characters of the (uppercased, invalid-chars-mapped-to-'_') base,
 * then '~' and `n` in decimal, then up to three extension characters -- the
 * classic "LONGFI~1.TXT" scheme. Returns 0, or -1 when `n` no longer fits in
 * the 8-character base field. `n` starts at 1.
 */
int hype_fat_shortname_tail(const char *name, unsigned int n, uint8_t out11[11]);

/*
 * Encodes physical LFN entry `seq` (1-based; carries characters
 * (seq-1)*13 .. seq*13-1 of `name`). `last` marks the run's first physical
 * entry (HYPE_FAT_LFN_LAST). Characters past the end of the name are filled
 * per spec: one 0x0000 terminator, then 0xFFFF. Pure.
 */
void hype_fat_lfn_entry_build(uint8_t ent[32], const char *name, unsigned int name_len,
                              unsigned int seq, int last, uint8_t checksum);

/*
 * Extracts the 13 characters of one LFN entry into `out` at their run position
 * ((seq-1)*13, from the entry's own sequence byte). Characters above 0xFF --
 * which hype never writes and cannot compare -- are replaced with 0x7F so they
 * match no ASCII name. Returns the entry's sequence number (without the LAST
 * bit), or 0 for an out-of-range one. `out` must hold HYPE_FAT_MAX_LFN + 1.
 */
unsigned int hype_fat_lfn_entry_chars(const uint8_t ent[32], char *out);

/*
 * Updates the free-cluster count + next-free hint in a 512-byte FSInfo sector,
 * leaving the signatures intact. Returns 0 on success, -1 if the sector's lead
 * signature is not a valid FSInfo (0x41615252).
 */
int hype_fat32_fsinfo_set(uint8_t *fsinfo_sector, uint32_t free_count, uint32_t next_free);

#endif /* HYPE_CORE_FAT_WRITE_H */
