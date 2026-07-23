#ifndef HYPE_CORE_FAT_WRITE_H
#define HYPE_CORE_FAT_WRITE_H

#include <stdint.h>

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
 * (split hi/lo per FAT32) and file size. Timestamps are zeroed. Pure.
 */
void hype_fat_dirent_build(uint8_t ent[32], const uint8_t name11[11], uint8_t attr,
                           uint32_t first_cluster, uint32_t size);

/* Reads the first cluster + size back out of a 32-byte directory entry. */
uint32_t hype_fat_dirent_cluster(const uint8_t ent[32]);
uint32_t hype_fat_dirent_size(const uint8_t ent[32]);

/*
 * Updates the free-cluster count + next-free hint in a 512-byte FSInfo sector,
 * leaving the signatures intact. Returns 0 on success, -1 if the sector's lead
 * signature is not a valid FSInfo (0x41615252).
 */
int hype_fat32_fsinfo_set(uint8_t *fsinfo_sector, uint32_t free_count, uint32_t next_free);

#endif /* HYPE_CORE_FAT_WRITE_H */
