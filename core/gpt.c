#include "gpt.h"

/* GPT header field offsets (UEFI spec §5.3.2), within the LBA-1 sector. */
#define GPT_SIG_OFF 0x00u          /* 8 bytes: "EFI PART" */
#define GPT_DISK_GUID_OFF 0x38u    /* 16 bytes: the disk's GUID */
#define GPT_PART_ENTRY_LBA_OFF 0x48u /* u64: first LBA of the partition-entry array */
#define GPT_NUM_ENTRIES_OFF 0x50u  /* u32: number of partition entries */
#define GPT_ENTRY_SIZE_OFF 0x54u   /* u32: size of each entry (>= 128) */

/* Partition entry field offsets (§5.3.3). */
#define GPT_ENT_TYPE_GUID_OFF 0x00u /* 16 bytes; all-zero => unused entry */
#define GPT_ENT_FIRST_LBA_OFF 0x20u /* u64 */
#define GPT_ENT_LAST_LBA_OFF 0x28u  /* u64 */

/* "EFI PART" as a little-endian u64. */
#define GPT_SIGNATURE 0x5452415020494645ull

/* Reject absurd geometries (defends the entry-walk loop against a corrupt/hostile
 * header) while comfortably covering any real table (128 entries is standard). */
#define GPT_MAX_ENTRIES 4096u
#define GPT_MIN_ENTRY_SIZE 128u

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_le64(const uint8_t *p) {
    return (uint64_t)rd_le32(p) | ((uint64_t)rd_le32(p + 4) << 32);
}

static int entry_is_used(const uint8_t *ent) {
    unsigned i;
    for (i = 0; i < 16u; i++) {
        if (ent[GPT_ENT_TYPE_GUID_OFF + i] != 0u) {
            return 1;
        }
    }
    return 0;
}

int hype_gpt_find_partition(hype_gpt_read_lba_fn read, void *ctx, unsigned index,
                            hype_gpt_partition_t *out) {
    uint8_t sector[HYPE_GPT_SECTOR_SIZE];
    uint64_t entry_lba;
    uint32_t num_entries;
    uint32_t entry_size;
    uint32_t per_sector;
    unsigned used_seen = 0;
    uint32_t scanned = 0;

    if (index == 0u) {
        return -1; /* 1-based */
    }
    if (read(ctx, HYPE_GPT_HEADER_LBA, 1u, sector) != 0) {
        return -1;
    }
    if (rd_le64(sector + GPT_SIG_OFF) != GPT_SIGNATURE) {
        return -1;
    }
    entry_lba = rd_le64(sector + GPT_PART_ENTRY_LBA_OFF);
    num_entries = rd_le32(sector + GPT_NUM_ENTRIES_OFF);
    entry_size = rd_le32(sector + GPT_ENTRY_SIZE_OFF);
    if (entry_size < GPT_MIN_ENTRY_SIZE || num_entries == 0u || num_entries > GPT_MAX_ENTRIES) {
        return -1;
    }
    /* Only 128-byte entries are walked (the ubiquitous size); a larger entry_size
     * would need a multi-sector-per-entry walk this minimal parser doesn't do. */
    if (entry_size != GPT_MIN_ENTRY_SIZE) {
        return -1;
    }
    per_sector = HYPE_GPT_SECTOR_SIZE / entry_size; /* 4 */

    while (scanned < num_entries) {
        uint32_t i;
        if (read(ctx, entry_lba + (scanned / per_sector), 1u, sector) != 0) {
            return -1;
        }
        for (i = 0; i < per_sector && scanned < num_entries; i++, scanned++) {
            const uint8_t *ent = sector + (uint32_t)i * entry_size;
            if (!entry_is_used(ent)) {
                continue;
            }
            used_seen++;
            if (used_seen == index) {
                out->first_lba = rd_le64(ent + GPT_ENT_FIRST_LBA_OFF);
                out->last_lba = rd_le64(ent + GPT_ENT_LAST_LBA_OFF);
                if (out->last_lba < out->first_lba) {
                    return -1;
                }
                out->size_bytes = (out->last_lba - out->first_lba + 1u) * HYPE_GPT_SECTOR_SIZE;
                return 0;
            }
        }
    }
    return -1; /* fewer than `index` used partitions */
}

int hype_gpt_disk_guid(hype_gpt_read_lba_fn read, void *ctx, uint8_t out_guid[16]) {
    uint8_t sector[HYPE_GPT_SECTOR_SIZE];
    unsigned i;

    if (read(ctx, HYPE_GPT_HEADER_LBA, 1u, sector) != 0) {
        return -1;
    }
    if (rd_le64(sector + GPT_SIG_OFF) != GPT_SIGNATURE) {
        return -1;
    }
    for (i = 0; i < 16u; i++) {
        out_guid[i] = sector[GPT_DISK_GUID_OFF + i];
    }
    return 0;
}
