#include "fat_exfat.h"

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

/* ---- checksums and hashes ---- */

/* All three are the same primitive: rotate the accumulator right one bit, then
 * add the byte. Spelled out per width because the accumulator widths differ. */
static uint16_t ror16_add(uint16_t sum, uint8_t b) {
    return (uint16_t)((uint16_t)((sum & 1u) ? 0x8000u : 0u) + (uint16_t)(sum >> 1) + (uint16_t)b);
}

uint16_t hype_exfat_set_checksum_update(uint16_t sum, unsigned int byte_index, const uint8_t *bytes,
                                        unsigned int n) {
    unsigned int i;
    for (i = 0; i < n; i++) {
        unsigned int idx = byte_index + i;
        if (idx == 2u || idx == 3u) {
            continue; /* the SetChecksum field itself is excluded */
        }
        sum = ror16_add(sum, bytes[i]);
    }
    return sum;
}

uint16_t hype_exfat_name_hash_update(uint16_t hash, const uint16_t *upcased, unsigned int count) {
    unsigned int i;
    for (i = 0; i < count; i++) {
        hash = ror16_add(hash, (uint8_t)(upcased[i] & 0xFFu)); /* little-endian: low byte first */
        hash = ror16_add(hash, (uint8_t)(upcased[i] >> 8));
    }
    return hash;
}

uint32_t hype_exfat_upcase_checksum_update(uint32_t sum, const uint8_t *bytes, unsigned int n) {
    unsigned int i;
    for (i = 0; i < n; i++) {
        sum = ((sum & 1u) ? 0x80000000u : 0u) + (sum >> 1) + (uint32_t)bytes[i];
    }
    return sum;
}

/* ---- up-case table ---- */

void hype_exfat_upcase_reset(hype_exfat_upcase_t *u) {
    unsigned int i;
    for (i = 0; i < HYPE_EXFAT_UPCASE_CACHE; i++) {
        u->map[i] = (uint16_t)i; /* identity until the table says otherwise */
    }
    u->chars = 0u;
    u->checksum = 0u;
    u->pending_count = 0u;
    u->awaiting_count = 0u;
    u->half_valid = 0u;
    u->half_byte = 0u;
    u->malformed = 0u;
}

/* Consume one decoded 16-bit table word. */
static void upcase_word(hype_exfat_upcase_t *u, uint16_t word) {
    if (u->awaiting_count) {
        u->awaiting_count = 0u;
        /* An identity run: `word` characters map to themselves, which the reset
         * state already says, so only the character cursor moves. */
        if ((uint32_t)word > 0x10000u - u->chars) {
            u->malformed = 1u;
            return;
        }
        u->chars += (uint32_t)word;
        return;
    }
    if (word == 0xFFFFu) {
        u->awaiting_count = 1u;
        return;
    }
    if (u->chars >= 0x10000u) {
        u->malformed = 1u;
        return;
    }
    if (u->chars < HYPE_EXFAT_UPCASE_CACHE) {
        u->map[u->chars] = word;
    }
    u->chars++;
}

void hype_exfat_upcase_feed(hype_exfat_upcase_t *u, const uint8_t *bytes, unsigned int n) {
    unsigned int i;
    u->checksum = hype_exfat_upcase_checksum_update(u->checksum, bytes, n);
    for (i = 0; i < n; i++) {
        if (!u->half_valid) {
            u->half_byte = bytes[i];
            u->half_valid = 1u;
            continue;
        }
        u->half_valid = 0u;
        upcase_word(u, (uint16_t)((uint16_t)u->half_byte | ((uint16_t)bytes[i] << 8)));
    }
    /* A trailing 0xFFFF marker with no count word behind it simply ends the
     * table -- the reference table mkfs.exfat writes does exactly that -- so it
     * is deliberately not treated as malformed. */
}

uint16_t hype_exfat_upcase(const hype_exfat_upcase_t *u, uint16_t ch) {
    if ((uint32_t)ch < HYPE_EXFAT_UPCASE_CACHE && (uint32_t)ch < u->chars) {
        return u->map[ch];
    }
    return ch;
}

int hype_exfat_upcase_exact(const hype_exfat_upcase_t *u, uint16_t ch) {
    return ((uint32_t)ch < HYPE_EXFAT_UPCASE_CACHE && (uint32_t)ch < u->chars) ? 1 : 0;
}

/* ---- allocation bitmap ---- */

#define BITS_PER_SECTOR (HYPE_EXFAT_SECTOR_SIZE * 8u)

void hype_exfat_bitmap_location(uint32_t cluster, uint64_t bitmap_start_lba, uint64_t *out_lba,
                                unsigned int *out_bit) {
    uint32_t bit = cluster - 2u; /* cluster 2 is bit 0 */
    if (out_lba) {
        *out_lba = bitmap_start_lba + (uint64_t)(bit / BITS_PER_SECTOR);
    }
    if (out_bit) {
        *out_bit = bit % BITS_PER_SECTOR;
    }
}

int hype_exfat_bitmap_get(const uint8_t *sector, unsigned int bit_in_sector) {
    return (sector[bit_in_sector / 8u] & (uint8_t)(1u << (bit_in_sector % 8u))) ? 1 : 0;
}

void hype_exfat_bitmap_set(uint8_t *sector, unsigned int bit_in_sector, int used) {
    uint8_t mask = (uint8_t)(1u << (bit_in_sector % 8u));
    if (used) {
        sector[bit_in_sector / 8u] |= mask;
    } else {
        sector[bit_in_sector / 8u] &= (uint8_t)~mask;
    }
}

int hype_exfat_bitmap_find_free(const uint8_t *sector, unsigned int from_bit, unsigned int bits,
                                unsigned int *out_bit) {
    unsigned int i;
    if (bits > BITS_PER_SECTOR) {
        bits = BITS_PER_SECTOR;
    }
    for (i = from_bit; i < bits; i++) {
        /* Skip a whole byte of allocated clusters at a time -- a nearly-full
         * volume otherwise costs 4096 single-bit tests per bitmap sector. */
        if ((i % 8u) == 0u && sector[i / 8u] == 0xFFu) {
            i += 7u;
            continue;
        }
        if (!hype_exfat_bitmap_get(sector, i)) {
            if (out_bit) {
                *out_bit = i;
            }
            return 0;
        }
    }
    return -1;
}

unsigned int hype_exfat_bitmap_count(const uint8_t *sector, unsigned int bits) {
    unsigned int i, n = 0u;
    if (bits > BITS_PER_SECTOR) {
        bits = BITS_PER_SECTOR;
    }
    for (i = 0; i < bits; i++) {
        if (hype_exfat_bitmap_get(sector, i)) {
            n++;
        }
    }
    return n;
}

/* ---- addressing ---- */

uint64_t hype_exfat_cluster_lba(uint32_t heap_lba, uint32_t sec_per_cluster, uint32_t cluster) {
    return (uint64_t)heap_lba + (uint64_t)(cluster - 2u) * sec_per_cluster;
}

void hype_exfat_entry_pos(uint32_t ei, uint32_t sec_per_cluster, uint32_t *out_cluster_index,
                          uint32_t *out_sec_in_cluster, unsigned int *out_off_in_sector) {
    uint32_t per_cluster = sec_per_cluster * HYPE_EXFAT_ENTRIES_PER_SECTOR;
    uint32_t in_cluster = ei % per_cluster;
    if (out_cluster_index) {
        *out_cluster_index = ei / per_cluster;
    }
    if (out_sec_in_cluster) {
        *out_sec_in_cluster = in_cluster / HYPE_EXFAT_ENTRIES_PER_SECTOR;
    }
    if (out_off_in_sector) {
        *out_off_in_sector = (in_cluster % HYPE_EXFAT_ENTRIES_PER_SECTOR) * HYPE_EXFAT_ENTRY_SIZE;
    }
}

/* ---- directory-entry encoders ---- */

static void ent_zero(uint8_t ent[32]) {
    unsigned int i;
    for (i = 0; i < HYPE_EXFAT_ENTRY_SIZE; i++) {
        ent[i] = 0u;
    }
}

void hype_exfat_file_entry(uint8_t ent[32], uint16_t attributes, uint8_t secondary_count,
                           const hype_rtc_time_t *now) {
    ent_zero(ent);
    ent[0] = HYPE_EXFAT_ENT_FILE;
    ent[1] = secondary_count;
    /* ent[2..3] (SetChecksum) stays zero so a checksum taken over the built set
     * matches -- hype_exfat_set_checksum_update skips those bytes anyway. */
    wr16(ent + 4, attributes);
    {
        /* hype_exfat_encode_timestamp() returns the 1980-01-01 epoch for an
         * absent/invalid time, so this keeps the previous behaviour when there
         * is no clock. */
        uint32_t ts = hype_exfat_encode_timestamp(now);
        wr32(ent + 8, ts);  /* CreateTimestamp */
        wr32(ent + 12, ts); /* LastModifiedTimestamp */
        wr32(ent + 16, ts); /* LastAccessedTimestamp */
    }
}

void hype_exfat_stream_entry(uint8_t ent[32], unsigned int name_length, uint16_t name_hash,
                            uint64_t valid_data_length, uint32_t first_cluster,
                            uint64_t data_length, int no_fat_chain) {
    ent_zero(ent);
    ent[0] = HYPE_EXFAT_ENT_STREAM;
    ent[1] = (uint8_t)(HYPE_EXFAT_FLAG_ALLOC_POSSIBLE |
                       (no_fat_chain ? HYPE_EXFAT_FLAG_NO_FAT_CHAIN : 0u));
    ent[3] = (uint8_t)name_length;
    wr16(ent + 4, name_hash);
    wr64(ent + 8, valid_data_length);
    wr32(ent + 20, first_cluster);
    wr64(ent + 24, data_length);
}

void hype_exfat_name_entry(uint8_t ent[32], const uint16_t *chars, unsigned int count) {
    unsigned int i;
    ent_zero(ent);
    ent[0] = HYPE_EXFAT_ENT_NAME;
    for (i = 0; i < count && i < HYPE_EXFAT_NAME_CHARS_PER_ENTRY; i++) {
        wr16(ent + 2u + i * 2u, chars[i]);
    }
}

void hype_exfat_file_entry_set_checksum(uint8_t ent[32], uint16_t checksum) {
    wr16(ent + 2, checksum);
}

int hype_exfat_name_to_utf16(const char *name, uint16_t *out, unsigned int cap) {
    unsigned int n = 0;
    if (cap > HYPE_EXFAT_MAX_NAME) {
        cap = HYPE_EXFAT_MAX_NAME;
    }
    while (name[n] != '\0') {
        unsigned char c = (unsigned char)name[n];
        if (n >= cap) {
            return -1; /* too long for the caller's buffer / for exFAT */
        }
        if (c < 0x20u) {
            return -1; /* control characters are not permitted in a name */
        }
        if (c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' ||
            c == '\\' || c == '|') {
            return -1; /* exFAT's invalid-filename-character set */
        }
        out[n] = (uint16_t)c;
        n++;
    }
    if (n == 0u) {
        return -1; /* an empty name is not a name */
    }
    return (int)n;
}

/* ---- directory entry sets ---- */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

int hype_exfat_set_read(hype_exfat_entry_read_fn read_entry, void *ctx, uint32_t ei,
                        hype_exfat_set_t *set) {
    uint8_t ent[HYPE_EXFAT_ENTRY_SIZE];
    uint16_t sum = 0u;
    uint16_t stored;
    unsigned int k;
    unsigned int nlen = 0u;
    int have_stream = 0;

    if (read_entry(ctx, ei, ent) != 0) {
        return -1;
    }
    if (ent[0] != HYPE_EXFAT_ENT_FILE) {
        return -1;
    }
    set->secondary = ent[1];
    set->attributes = rd16(ent + 4);
    set->first_cluster = 0u;
    set->data_length = 0u;
    set->valid_length = 0u;
    set->contiguous = 0u;
    set->name_length = 0u;
    set->name_hash = 0u;
    stored = rd16(ent + 2);
    if (set->secondary < 1u || set->secondary > HYPE_EXFAT_MAX_SECONDARY) {
        return -1; /* every File set carries at least a Stream Extension entry */
    }
    sum = hype_exfat_set_checksum_update(sum, 0u, ent, HYPE_EXFAT_ENTRY_SIZE);

    for (k = 1u; k <= set->secondary; k++) {
        if (read_entry(ctx, ei + k, ent) != 0) {
            return -1; /* the set is cut short by the end of the directory */
        }
        sum = hype_exfat_set_checksum_update(sum, k * HYPE_EXFAT_ENTRY_SIZE, ent,
                                             HYPE_EXFAT_ENTRY_SIZE);
        if (k == 1u) {
            if (ent[0] != HYPE_EXFAT_ENT_STREAM) {
                return -1;
            }
            set->contiguous = (uint8_t)((ent[1] & HYPE_EXFAT_FLAG_NO_FAT_CHAIN) ? 1u : 0u);
            set->name_length = ent[3];
            set->name_hash = rd16(ent + 4);
            set->valid_length = rd64(ent + 8);
            set->first_cluster = rd32(ent + 20);
            set->data_length = rd64(ent + 24);
            have_stream = 1;
            continue;
        }
        if (ent[0] != HYPE_EXFAT_ENT_NAME) {
            continue; /* a secondary entry type hype does not know is skipped, not
                       * fatal -- it still counts towards the set checksum */
        }
        {
            unsigned int c;
            for (c = 0; c < HYPE_EXFAT_NAME_CHARS_PER_ENTRY && nlen < HYPE_EXFAT_MAX_NAME; c++) {
                set->name[nlen++] = rd16(ent + 2u + c * 2u);
            }
        }
    }
    if (!have_stream || sum != stored) {
        return -1; /* corrupt set: refuse it rather than trust any of its fields */
    }
    if (set->name_length == 0u || set->name_length > nlen) {
        return -1; /* NameLength disagrees with the File Name entries present */
    }
    if (set->valid_length > set->data_length) {
        return -1;
    }
    return 0;
}
