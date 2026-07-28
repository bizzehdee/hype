#ifndef HYPE_CORE_FAT_EXFAT_H
#define HYPE_CORE_FAT_EXFAT_H

#include <stdint.h>

#include "rtc.h"

/*
 * #198 (STORAGE: writable FAT32/exFAT) -- pure exFAT primitives, shared by the
 * read-only resolver (core/fat.c) and the block-backed writer
 * (core/fat_exfat_fs.c). Everything here is I/O-free and side-effect-free so it
 * is unit-testable directly: the three exFAT checksums, the file-name hash, the
 * up-case table decompressor, allocation-bitmap bit twiddling, directory-entry
 * addressing arithmetic, and directory-entry encoders.
 *
 * Field offsets and the checksum/hash algorithms are from the Microsoft exFAT
 * file system specification (rev 1.00), sections 3.1 (Main Boot Sector), 6.x
 * (directory-entry definitions), 7.1 (Allocation Bitmap), 7.2 (Up-case Table),
 * and the EntrySetChecksum / NameHash / TableChecksum pseudocode. The layout
 * facts used are also cross-checked byte-for-byte against a real
 * `mkfs.exfat` (exfatprogs 1.4.2) volume -- see the ticket notes.
 *
 * 512-byte logical sectors only, matching the rest of hype's block world.
 */

#define HYPE_EXFAT_SECTOR_SIZE 512u
#define HYPE_EXFAT_ENTRY_SIZE 32u
#define HYPE_EXFAT_ENTRIES_PER_SECTOR (HYPE_EXFAT_SECTOR_SIZE / HYPE_EXFAT_ENTRY_SIZE) /* 16 */

/* Directory-entry type bytes. Bit 7 (0x80) is InUse: a deleted entry keeps its
 * type nibbles but clears that bit (a deleted File entry reads 0x05). */
#define HYPE_EXFAT_ENT_INUSE 0x80u
#define HYPE_EXFAT_ENT_BITMAP 0x81u
#define HYPE_EXFAT_ENT_UPCASE 0x82u
#define HYPE_EXFAT_ENT_LABEL 0x83u
#define HYPE_EXFAT_ENT_FILE 0x85u
#define HYPE_EXFAT_ENT_STREAM 0xC0u
#define HYPE_EXFAT_ENT_NAME 0xC1u

/* GeneralSecondaryFlags bits in a Stream Extension / Allocation Bitmap entry. */
#define HYPE_EXFAT_FLAG_ALLOC_POSSIBLE 0x01u
#define HYPE_EXFAT_FLAG_NO_FAT_CHAIN 0x02u

/* FileAttributes bits (same encoding as FAT's attribute byte). */
#define HYPE_EXFAT_ATTR_READ_ONLY 0x0001u
#define HYPE_EXFAT_ATTR_DIRECTORY 0x0010u
#define HYPE_EXFAT_ATTR_ARCHIVE 0x0020u

/* A FAT entry >= this marks end-of-chain; 0xFFFFFFF7 is the bad-cluster mark. */
#define HYPE_EXFAT_EOC 0xFFFFFFF8u
#define HYPE_EXFAT_BAD_CLUSTER 0xFFFFFFF7u

/* 15 UTF-16 code units per File Name entry; a name is at most 255 units, so at
 * most 17 name entries -> SecondaryCount is at most 18 (1 stream + 17 name). */
#define HYPE_EXFAT_NAME_CHARS_PER_ENTRY 15u
#define HYPE_EXFAT_MAX_NAME 255u
#define HYPE_EXFAT_MAX_SECONDARY 18u

/* 1980-01-01 00:00:00, the earliest timestamp exFAT can encode (month and day
 * fields are 1-based, so an all-zero timestamp is out of spec and trips
 * consistency checkers). Used for every entry hype creates -- hype has no
 * wall-clock source it trusts post-ExitBootServices, and a fixed valid stamp is
 * preferable to an invalid or fabricated one. */
#define HYPE_EXFAT_TIMESTAMP_EPOCH 0x00210000u

/*
 * ---- checksums and hashes ----
 *
 * All three exFAT checksums are "rotate right one bit, then add the next byte".
 * Each is exposed incrementally so a caller can feed sector-sized chunks of a
 * structure that spans sectors without buffering the whole thing.
 */

/*
 * Directory-entry-set checksum (spec: EntrySetChecksum). Feed the bytes of the
 * entry set in order, starting from the File entry's byte 0; `byte_index` is the
 * index of `bytes[0]` within the set, so the checksum field itself (set bytes 2
 * and 3) is skipped automatically. Start from sum == 0.
 */
uint16_t hype_exfat_set_checksum_update(uint16_t sum, unsigned int byte_index, const uint8_t *bytes,
                                        unsigned int n);

/*
 * File-name hash (spec: NameHash), over the UP-CASED name as little-endian
 * UTF-16 code units. Start from hash == 0.
 */
uint16_t hype_exfat_name_hash_update(uint16_t hash, const uint16_t *upcased, unsigned int count);

/* Up-case table checksum (spec: TableChecksum), over the raw table bytes.
 * Start from sum == 0. */
uint32_t hype_exfat_upcase_checksum_update(uint32_t sum, const uint8_t *bytes, unsigned int n);

/*
 * ---- up-case table ----
 *
 * The on-disk table is a stream of little-endian 16-bit words. Read in order
 * against a running character index, a word of 0xFFFF means "the next word is a
 * count N of characters that map to themselves"; any other word is the literal
 * mapping for the current character. A 0xFFFF as the very last word of the
 * table (the reference table produced by mkfs.exfat ends that way) has no count
 * word and simply terminates the table.
 *
 * A full decompressed table is 65536 entries (128 KiB), which hype has no
 * business holding. We cache the first HYPE_EXFAT_UPCASE_CACHE characters --
 * enough for ASCII plus Latin-1, which is every name hype itself creates -- and
 * treat anything above that as mapping to itself. hype_exfat_upcase_exact()
 * reports whether a given character's mapping is actually known, so the writer
 * can refuse a name whose hash it cannot compute correctly rather than writing a
 * hash that other exFAT implementations would disagree with.
 */
#define HYPE_EXFAT_UPCASE_CACHE 256u

typedef struct {
    uint16_t map[HYPE_EXFAT_UPCASE_CACHE]; /* map[c] == up-case of character c */
    uint32_t chars;                        /* characters described by the table so far */
    uint32_t checksum;                     /* running TableChecksum of the fed bytes */
    uint16_t pending_count;                /* held word when a 0xFFFF marker split a chunk */
    uint8_t awaiting_count;                /* 1 == the next word is a run length */
    uint8_t half_valid;                    /* 1 == half_byte holds the low byte of a word */
    uint8_t half_byte;
    uint8_t malformed; /* 1 == the table over-ran 0x10000 characters */
} hype_exfat_upcase_t;

/* Resets to the identity mapping (also the correct state for a volume whose
 * table has not been read yet). */
void hype_exfat_upcase_reset(hype_exfat_upcase_t *u);

/* Feeds the next `n` raw table bytes, in order. Any number of bytes per call. */
void hype_exfat_upcase_feed(hype_exfat_upcase_t *u, const uint8_t *bytes, unsigned int n);

/* Up-cases one UTF-16 code unit (identity outside the cached range). */
uint16_t hype_exfat_upcase(const hype_exfat_upcase_t *u, uint16_t ch);

/* 1 if this character's mapping came from the table (so a name hash built from
 * it is exact), 0 if hype_exfat_upcase() would be guessing at identity. */
int hype_exfat_upcase_exact(const hype_exfat_upcase_t *u, uint16_t ch);

/*
 * ---- allocation bitmap ----
 *
 * One bit per cluster, LSB first, cluster 2 == bit 0. 1 == allocated.
 */

/* Locates cluster `cluster`'s bit: the sector holding it (relative to the
 * bitmap's first LBA) and the bit index within that sector. */
void hype_exfat_bitmap_location(uint32_t cluster, uint64_t bitmap_start_lba, uint64_t *out_lba,
                                unsigned int *out_bit);

int hype_exfat_bitmap_get(const uint8_t *sector, unsigned int bit_in_sector);
void hype_exfat_bitmap_set(uint8_t *sector, unsigned int bit_in_sector, int used);

/*
 * Finds the first clear bit in [from_bit, bits) of a 512-byte bitmap sector.
 * Returns 0 and the index in *out_bit, or -1 if every candidate bit is set.
 */
int hype_exfat_bitmap_find_free(const uint8_t *sector, unsigned int from_bit, unsigned int bits,
                                unsigned int *out_bit);

/* Counts the set bits in [0, bits) of a 512-byte bitmap sector. */
unsigned int hype_exfat_bitmap_count(const uint8_t *sector, unsigned int bits);

/*
 * ---- addressing ----
 */

/* Volume-relative first LBA of cluster `cluster` (>= 2). */
uint64_t hype_exfat_cluster_lba(uint32_t heap_lba, uint32_t sec_per_cluster, uint32_t cluster);

/*
 * Decomposes directory-entry index `ei` into the index of the cluster within the
 * directory's chain, the sector within that cluster, and the byte offset within
 * that sector. `sec_per_cluster` must be non-zero.
 */
void hype_exfat_entry_pos(uint32_t ei, uint32_t sec_per_cluster, uint32_t *out_cluster_index,
                          uint32_t *out_sec_in_cluster, unsigned int *out_off_in_sector);

/*
 * ---- directory-entry encoders ----
 *
 * Each fills a caller-supplied 32-byte buffer. The set checksum is NOT filled in
 * (the caller computes it once the whole set is built) -- hype_exfat_file_entry
 * zeroes the field so a checksum computed over the built bytes is correct.
 */

/*
 * Primary File entry (0x85). `now` fills Create/LastModified/LastAccessed; pass
 * 0 (or an invalid time) for HYPE_EXFAT_TIMESTAMP_EPOCH, which is what this did
 * unconditionally before. Note the epoch, NOT zero, is the fallback: exFAT's
 * month and day fields are 1-based, so an all-zero timestamp is out of spec and
 * trips fsck.
 */
void hype_exfat_file_entry(uint8_t ent[32], uint16_t attributes, uint8_t secondary_count,
                           const hype_rtc_time_t *now);

/* Stream Extension entry (0xC0). */
void hype_exfat_stream_entry(uint8_t ent[32], unsigned int name_length, uint16_t name_hash,
                            uint64_t valid_data_length, uint32_t first_cluster, uint64_t data_length,
                            int no_fat_chain);

/* File Name entry (0xC1) carrying up to 15 UTF-16 code units. */
void hype_exfat_name_entry(uint8_t ent[32], const uint16_t *chars, unsigned int count);

/* Writes the 16-bit set checksum into a built File entry (set bytes 2..3). */
void hype_exfat_file_entry_set_checksum(uint8_t ent[32], uint16_t checksum);

/*
 * Converts an ASCII name to UTF-16 code units. Returns the length in code units,
 * or -1 if the name is empty, longer than `cap`/HYPE_EXFAT_MAX_NAME, or contains
 * a character exFAT forbids in a file name (control characters and
 * " * / : < > ? \ |).
 */
int hype_exfat_name_to_utf16(const char *name, uint16_t *out, unsigned int cap);

/*
 * ---- directory entry sets ----
 *
 * A file or directory is described by a *set* of consecutive 32-byte entries: one
 * primary File entry, one Stream Extension entry, and one or more File Name
 * entries, protected as a whole by the File entry's SetChecksum. Reading one is
 * the same job for the read-only resolver and for the writer, so it lives here,
 * parameterised by a callback that fetches entry `ei` of whatever directory the
 * caller is walking.
 *
 * NOTE (freestanding, no libc): hype_exfat_set_t contains an array, so it must
 * never be assigned or passed by value -- that emits a memcpy call which does not
 * exist at EFI link time.
 */
typedef struct {
    uint8_t secondary;      /* SecondaryCount: entries after the File entry */
    uint16_t attributes;    /* FileAttributes */
    uint32_t first_cluster; /* FirstCluster (0 == no allocation) */
    uint64_t data_length;   /* DataLength */
    uint64_t valid_length;  /* ValidDataLength */
    uint8_t contiguous;     /* 1 == NoFatChain: the allocation has no FAT chain */
    uint8_t name_length;    /* NameLength, in UTF-16 code units */
    uint16_t name_hash;     /* NameHash, as stored */
    uint16_t name[HYPE_EXFAT_MAX_NAME]; /* the name as stored (NOT up-cased) */
} hype_exfat_set_t;

/* Fetches the 32 bytes of directory entry `ei`. Returns 0 on success, non-zero
 * at the end of the directory's allocation or on an I/O error. */
typedef int (*hype_exfat_entry_read_fn)(void *ctx, uint32_t ei, uint8_t ent[32]);

/*
 * Reads and validates the entry set whose File entry sits at index `ei`. Returns
 * 0 on a well-formed set, -1 if an entry cannot be read, `ei` is not an in-use
 * File entry, SecondaryCount is out of range, the Stream Extension entry is
 * missing, the SetChecksum does not match, NameLength disagrees with the File
 * Name entries actually present, or ValidDataLength exceeds DataLength. It does
 * NOT range-check the allocation against the volume -- only the caller knows the
 * cluster count.
 */
int hype_exfat_set_read(hype_exfat_entry_read_fn read_entry, void *ctx, uint32_t ei,
                        hype_exfat_set_t *set);

#endif /* HYPE_CORE_FAT_EXFAT_H */
