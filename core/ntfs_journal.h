#ifndef HYPE_CORE_NTFS_JOURNAL_H
#define HYPE_CORE_NTFS_JOURNAL_H

#include <stdint.h>
#include "ntfs.h"
#include "fs_owner_guard.h"

/*
 * #416 (STORAGE: NTFS $LogFile/USN journal and dirty-volume replay), descoped
 * per plan.md §10 decision 64: $LogFile replay is refused, never attempted --
 * see the decision for why (ntfs-3g, this project's own reference, doesn't
 * implement it either, and there is no accessible spec to validate a
 * from-scratch engine against). What this module actually provides:
 *
 *  - A dirty-flag BRACKET around a writable session (hype_ntfs_txn_open/
 *    close): sets $VOLUME_INFORMATION's dirty bit before the first mutation,
 *    clears it only on a clean close. This is the interop contract with any
 *    real NTFS driver -- an interrupted hype session leaves the bit set, so
 *    Windows/ntfs-3g correctly demand a chkdsk rather than trusting metadata
 *    a crash may have left inconsistent. No $LogFile content is written.
 *  - USN change-journal maintenance (hype_ntfs_usn_append): a real,
 *    well-documented USN_RECORD_V2 append to an EXISTING, active
 *    $Extend\$UsnJrnl -- hype never creates or enables one itself.
 *  - The #596/decision-57 single-owner-core lesson applied from the start:
 *    every mutating entry point here takes a hype_fs_owner_guard_t and
 *    refuses (-1) rather than silently proceeding when the executing core
 *    is not the bound owner. See fs_owner_guard.h for the full rationale.
 */

typedef struct {
    hype_fs_owner_guard_t guard;
    int open;          /* 1 between a successful txn_open and its txn_close */
    uint16_t next_usn; /* next fixup USN this session will stamp; cycles 1..0xFFFE */
} hype_ntfs_txn_t;

/* Binds `owner_apic_id` as the only core allowed to drive this session, and
 * marks the session not-yet-open. Call once, before hype_ntfs_txn_open(). */
void hype_ntfs_txn_init(hype_ntfs_txn_t *txn, uint32_t owner_apic_id);

/*
 * Opens a writable session: refuses (-1) if the volume is already dirty
 * (matches ntfs-3g's own refusal -- a prior session ended uncleanly, or
 * another driver has it open; hype never guesses which), if the executing
 * core is not the bound owner, or if setting the dirty bit itself fails.
 * On success the dirty bit is now set on the medium and every later mutation
 * in this session is covered by it.
 */
int hype_ntfs_txn_open(hype_ntfs_t *fs, hype_blk_write_fn write, hype_ntfs_txn_t *txn,
                       uint32_t executing_apic_id);

/*
 * Closes a writable session: refuses (-1, and leaves the dirty bit SET) if
 * the executing core is not the bound owner, or if the session was never
 * opened. Clears the dirty bit only when called -- the caller must not call
 * this until every pending write from the session has already reached the
 * medium (the same durability-barrier discipline decision 56 established
 * elsewhere); this function does not flush anything itself.
 */
int hype_ntfs_txn_close(hype_ntfs_t *fs, hype_blk_write_fn write, hype_ntfs_txn_t *txn,
                        uint32_t executing_apic_id);

/* Next fixup USN this session should stamp (1..0xFFFE, wrapping, skipping 0
 * and 0xFFFF -- see hype_ntfs_fixup_stamp's contract). Pure; does not touch
 * the medium. Advances `txn`'s internal counter each call. */
uint16_t hype_ntfs_txn_next_usn(hype_ntfs_txn_t *txn);

/* --- USN change journal (well-documented USN_RECORD_V2, see research/README.md) --- */

#define HYPE_USN_REASON_DATA_OVERWRITE 0x00000001u
#define HYPE_USN_REASON_DATA_EXTEND 0x00000002u
#define HYPE_USN_REASON_DATA_TRUNCATION 0x00000004u
#define HYPE_USN_REASON_FILE_CREATE 0x00000100u
#define HYPE_USN_REASON_FILE_DELETE 0x00000200u
#define HYPE_USN_REASON_RENAME_OLD_NAME 0x00001000u
#define HYPE_USN_REASON_RENAME_NEW_NAME 0x00002000u

/*
 * Encodes one USN_RECORD_V2 into `out` (caller-sized, at least
 * hype_usn_record_size(name_utf16_len) bytes) and returns its total byte
 * length (already DWORD-aligned per the format's own requirement), or 0 if
 * `out_cap` is too small. `name_utf16` is the file's name in UTF-16LE,
 * `name_utf16_len` its length in BYTES (not code units). `usn` is this
 * record's own journal-relative byte offset -- the caller (the mutating
 * slice appending it) supplies it, since only the caller knows $J's current
 * end-of-stream offset.
 */
uint32_t hype_ntfs_usn_encode(uint8_t *out, uint32_t out_cap, uint64_t file_ref,
                              uint64_t parent_ref, uint64_t usn, uint64_t timestamp_filetime,
                              uint32_t reason, uint32_t file_attributes,
                              const uint8_t *name_utf16, uint16_t name_utf16_len);

/* Total encoded size (DWORD-aligned) for a name of `name_utf16_len` bytes --
 * what a caller should size its buffer to before calling hype_ntfs_usn_encode. */
uint32_t hype_ntfs_usn_record_size(uint16_t name_utf16_len);

/*
 * Writes one USN_RECORD_V2 (already encoded by hype_ntfs_usn_encode) into
 * `usnjrnl_stream` at `offset` -- a plain #337-style IN-PLACE write through
 * the resolved $J stream's range map, guarded the same way as
 * hype_ntfs_txn_open/close (refuses if the executing core is not `txn`'s
 * bound owner). Returns 0 on success, -1 on refusal or a failed write.
 *
 * `offset` is the caller's job, not this function's: a real $UsnJrnl's
 * current end (DataSize/real_size) is exactly hype's HYPE_RANGE_DATA/
 * UNWRITTEN boundary in the resolved map, and #416 does not implement the
 * valid-length advance needed to write past it (that is #419's territory) --
 * hype_file_rmap_write_at refuses that on its own, correctly. So this only
 * ever succeeds writing into a region the map already reports as DATA
 * end-to-end; the caller supplies `offset` from whatever the mutating slice
 * that triggered this record already knows about the journal's writable
 * range, rather than this function guessing "the end" from size_bytes.
 *
 * hype never creates $UsnJrnl itself: if `usnjrnl_stream` is NULL (the
 * caller found no active journal), this is correctly a silent no-op success
 * (0) -- maintaining an absent journal is not a failure.
 */
int hype_ntfs_usn_append(hype_ntfs_txn_t *txn, uint32_t executing_apic_id,
                         hype_file_rmap_t *usnjrnl_stream, hype_blk_read_fn read,
                         hype_blk_write_fn write, void *ctx, uint64_t offset,
                         const uint8_t *record, uint32_t record_len);

#endif /* HYPE_CORE_NTFS_JOURNAL_H */
