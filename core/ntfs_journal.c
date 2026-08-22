#include "ntfs_journal.h"
#include "lebytes.h"
#include "file_range.h"

void hype_ntfs_txn_init(hype_ntfs_txn_t *txn, uint32_t owner_apic_id) {
    if (txn == 0) {
        return;
    }
    hype_fs_owner_guard_init(&txn->guard);
    hype_fs_owner_guard_bind(&txn->guard, owner_apic_id);
    txn->open = 0;
    txn->next_usn = 1u;
}

int hype_ntfs_txn_open(hype_ntfs_t *fs, hype_blk_write_fn write, hype_ntfs_txn_t *txn,
                       uint32_t executing_apic_id) {
    int dirty;

    if (fs == 0 || write == 0 || txn == 0) {
        return -1;
    }
    if (!hype_fs_owner_guard_check(&txn->guard, executing_apic_id)) {
        return -1;
    }

    dirty = hype_ntfs_volume_dirty_get(fs);
    if (dirty != 0) {
        /* either genuinely dirty (a prior session ended uncleanly) or the
         * read itself failed (-1) -- both are refused identically, matching
         * ntfs-3g's own "refuse, don't guess" behaviour on a dirty volume. */
        return -1;
    }

    if (hype_ntfs_volume_dirty_set(fs, write, 1, hype_ntfs_txn_next_usn(txn)) != 0) {
        return -1;
    }
    txn->open = 1;
    return 0;
}

int hype_ntfs_txn_close(hype_ntfs_t *fs, hype_blk_write_fn write, hype_ntfs_txn_t *txn,
                        uint32_t executing_apic_id) {
    if (fs == 0 || write == 0 || txn == 0) {
        return -1;
    }
    if (!hype_fs_owner_guard_check(&txn->guard, executing_apic_id)) {
        return -1;
    }
    if (!txn->open) {
        return -1;
    }
    if (hype_ntfs_volume_dirty_set(fs, write, 0, hype_ntfs_txn_next_usn(txn)) != 0) {
        return -1; /* dirty bit stays set on the medium -- the honest state */
    }
    txn->open = 0;
    return 0;
}

uint16_t hype_ntfs_txn_next_usn(hype_ntfs_txn_t *txn) {
    uint16_t v;
    if (txn == 0) {
        return 1u;
    }
    v = txn->next_usn;
    txn->next_usn = (uint16_t)(txn->next_usn + 1u);
    if (txn->next_usn == 0u || txn->next_usn == 0xFFFFu) {
        txn->next_usn = 1u;
    }
    return v;
}

/* USN_RECORD_V2 fixed header is 0x3C (60) bytes before the variable-length
 * UTF-16LE file name (research/README.md's "NTFS $LogFile / USN journal"
 * entry has the full field table). */
#define USN_V2_HEADER_BYTES 0x3Cu

uint32_t hype_ntfs_usn_record_size(uint16_t name_utf16_len) {
    uint32_t total = USN_V2_HEADER_BYTES + name_utf16_len;
    return (total + 7u) & ~((uint32_t)7u); /* DWORD- (in practice QWORD-) aligned */
}

uint32_t hype_ntfs_usn_encode(uint8_t *out, uint32_t out_cap, uint64_t file_ref,
                              uint64_t parent_ref, uint64_t usn, uint64_t timestamp_filetime,
                              uint32_t reason, uint32_t file_attributes,
                              const uint8_t *name_utf16, uint16_t name_utf16_len) {
    uint32_t total = hype_ntfs_usn_record_size(name_utf16_len);
    uint32_t i;

    if (out == 0 || total > out_cap) {
        return 0u;
    }

    hype_wr32(out + 0x00, total);       /* RecordLength */
    hype_wr16(out + 0x04, 2u);          /* MajorVersion */
    hype_wr16(out + 0x06, 0u);          /* MinorVersion */
    hype_wr64(out + 0x08, file_ref);    /* FileReferenceNumber */
    hype_wr64(out + 0x10, parent_ref);  /* ParentFileReferenceNumber */
    hype_wr64(out + 0x18, usn);         /* Usn */
    hype_wr64(out + 0x20, timestamp_filetime); /* TimeStamp */
    hype_wr32(out + 0x28, reason);      /* Reason */
    hype_wr32(out + 0x2C, 0u);          /* SourceInfo */
    hype_wr32(out + 0x30, 0u);          /* SecurityId */
    hype_wr32(out + 0x34, file_attributes); /* FileAttributes */
    hype_wr16(out + 0x38, name_utf16_len);  /* FileNameLength (bytes) */
    hype_wr16(out + 0x3A, (uint16_t)USN_V2_HEADER_BYTES); /* FileNameOffset */

    for (i = 0; i < name_utf16_len; i++) {
        out[USN_V2_HEADER_BYTES + i] = name_utf16[i];
    }
    for (i = USN_V2_HEADER_BYTES + name_utf16_len; i < total; i++) {
        out[i] = 0u; /* DWORD-alignment padding */
    }
    return total;
}

int hype_ntfs_usn_append(hype_ntfs_txn_t *txn, uint32_t executing_apic_id,
                         hype_file_rmap_t *usnjrnl_stream, hype_blk_read_fn read,
                         hype_blk_write_fn write, void *ctx, uint64_t offset,
                         const uint8_t *record, uint32_t record_len) {
    if (txn == 0 || read == 0 || write == 0 || record == 0) {
        return -1;
    }
    if (!hype_fs_owner_guard_check(&txn->guard, executing_apic_id)) {
        return -1;
    }
    if (!txn->open) {
        return -1; /* USN maintenance only happens inside a bracketed session */
    }
    if (usnjrnl_stream == 0) {
        return 0; /* no active $UsnJrnl -- maintaining an absent journal is not a failure */
    }
    /* #416 does not grow $DATA streams (that's #418/#419) -- hype_file_rmap_write_at
     * correctly refuses if `offset` falls outside the stream's current DATA
     * coverage, rather than faking a grow or a valid-length advance it cannot do. */
    return hype_file_rmap_write_at(usnjrnl_stream, read, write, ctx, offset, record, record_len);
}
