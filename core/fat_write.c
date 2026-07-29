#include "fat_write.h"

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }

uint32_t hype_fat32_entry_get(const uint8_t *fat_sector, unsigned int idx_in_sector) {
    return rd32(fat_sector + idx_in_sector * 4u) & 0x0FFFFFFFu;
}

void hype_fat32_entry_set(uint8_t *fat_sector, unsigned int idx_in_sector, uint32_t value) {
    uint8_t *p = fat_sector + idx_in_sector * 4u;
    uint32_t cur = rd32(p);
    wr32(p, (cur & 0xF0000000u) | (value & 0x0FFFFFFFu)); /* preserve reserved top nibble */
}

void hype_fat32_fat_location(uint32_t cluster, uint64_t fat_start_lba, uint64_t *out_sector_lba,
                             unsigned int *out_idx) {
    if (out_sector_lba) *out_sector_lba = fat_start_lba + (uint64_t)(cluster / HYPE_FAT32_ENTRIES_PER_SECTOR);
    if (out_idx) *out_idx = cluster % HYPE_FAT32_ENTRIES_PER_SECTOR;
}

int hype_fat32_find_free_in_sector(const uint8_t *fat_sector, unsigned int entries,
                                   unsigned int *out_idx) {
    unsigned int i;
    if (entries > HYPE_FAT32_ENTRIES_PER_SECTOR) entries = HYPE_FAT32_ENTRIES_PER_SECTOR;
    for (i = 0; i < entries; i++) {
        if (hype_fat32_entry_get(fat_sector, i) == 0u) {
            if (out_idx) *out_idx = i;
            return 0;
        }
    }
    return -1;
}

void hype_fat_shortname_83(const char *name, uint8_t out11[11]) {
    unsigned int i, dot = 0, have_dot = 0, n = 0;
    unsigned int bi, ei;

    for (i = 0; i < 11u; i++) out11[i] = ' ';
    /* find length + the LAST dot */
    while (name[n] != '\0') {
        if (name[n] == '.') { dot = n; have_dot = 1; }
        n++;
    }
    if (!have_dot) dot = n; /* no extension */

    for (bi = 0, i = 0; i < dot && bi < 8u; i++) {
        if (name[i] == '.') continue;
        out11[bi++] = (uint8_t)up(name[i]);
    }
    if (have_dot) {
        for (ei = 0, i = dot + 1u; name[i] != '\0' && ei < 3u; i++) {
            out11[8 + ei++] = (uint8_t)up(name[i]);
        }
    }
}

void hype_fat_dirent_build(uint8_t ent[32], const uint8_t name11[11], uint8_t attr,
                           uint32_t first_cluster, uint32_t size, const hype_rtc_time_t *now) {
    unsigned int i;
    for (i = 0; i < 32u; i++) ent[i] = 0;
    for (i = 0; i < 11u; i++) ent[i] = name11[i];
    ent[11] = attr;
    /* Timestamps (FAT spec sec 6). The encoders return 0 for an absent or
     * invalid time, so this reproduces the old all-zero behaviour rather than
     * writing a confidently wrong date when the clock is unreadable. */
    ent[13] = hype_fat_encode_time_tenths(now);      /* CrtTimeTenth */
    wr16(ent + 14, hype_fat_encode_time(now));       /* CrtTime */
    wr16(ent + 16, hype_fat_encode_date(now));       /* CrtDate */
    wr16(ent + 18, hype_fat_encode_date(now));       /* LstAccDate (date only) */
    wr16(ent + 22, hype_fat_encode_time(now));       /* WrtTime */
    wr16(ent + 24, hype_fat_encode_date(now));       /* WrtDate */
    wr16(ent + 20, (uint16_t)(first_cluster >> 16)); /* first cluster high */
    wr16(ent + 26, (uint16_t)(first_cluster & 0xFFFFu)); /* first cluster low */
    wr32(ent + 28, size);
}

uint32_t hype_fat_dirent_cluster(const uint8_t ent[32]) {
    uint32_t hi = (uint32_t)ent[20] | ((uint32_t)ent[21] << 8);
    uint32_t lo = (uint32_t)ent[26] | ((uint32_t)ent[27] << 8);
    return (hi << 16) | lo;
}

uint32_t hype_fat_dirent_size(const uint8_t ent[32]) {
    return rd32(ent + 28);
}

int hype_fat32_fsinfo_set(uint8_t *fsinfo_sector, uint32_t free_count, uint32_t next_free) {
    if (rd32(fsinfo_sector + 0) != 0x41615252u) return -1; /* FSInfo lead signature */
    wr32(fsinfo_sector + 0x1E8, free_count);
    wr32(fsinfo_sector + 0x1EC, next_free);
    return 0;
}

void hype_fat_dirent_set_cluster(uint8_t ent[32], uint32_t first_cluster) {
    wr16(ent + 20, (uint16_t)(first_cluster >> 16));
    wr16(ent + 26, (uint16_t)(first_cluster & 0xFFFFu));
}

/* ---- Long File Names (#247) ---- */

uint8_t hype_fat_shortname_checksum(const uint8_t name11[11]) {
    uint8_t sum = 0;
    unsigned int i;
    for (i = 0; i < 11u; i++) {
        sum = (uint8_t)((uint8_t)((sum & 1u) << 7) + (uint8_t)(sum >> 1) + name11[i]);
    }
    return sum;
}

/* Characters FAT forbids in a LONG name. Control characters are checked
 * separately; the short-name set is stricter (see shortchar_ok). */
static int longchar_ok(char c) {
    switch (c) {
    case '\\': case '/': case ':': case '*': case '?': case '"': case '<': case '>': case '|':
        return 0;
    default:
        return (unsigned char)c >= 0x20u;
    }
}

/* Characters valid in an 8.3 short name (FAT spec: upper-case letters, digits,
 * and this punctuation set; no spaces, no lower case). */
static int shortchar_ok(char c) {
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    switch (c) {
    case '$': case '%': case '\'': case '-': case '_': case '@': case '~': case '`':
    case '!': case '(': case ')': case '{': case '}': case '^': case '#': case '&':
        return 1;
    default:
        return 0;
    }
}

int hype_fat_name_valid(const char *name) {
    unsigned int n = 0;
    int all_dots = 1;
    while (name[n] != '\0') {
        if (!longchar_ok(name[n])) return 0;
        if (name[n] != '.') all_dots = 0;
        n++;
        if (n > HYPE_FAT_MAX_LFN) return 0;
    }
    return (n > 0u && !all_dots) ? 1 : 0;
}

int hype_fat_name_is_83(const char *name) {
    unsigned int i = 0, base = 0, ext = 0;
    int have_dot = 0;
    for (i = 0; name[i] != '\0'; i++) {
        if (name[i] == '.') {
            if (have_dot) return 0; /* a second dot cannot be encoded */
            have_dot = 1;
            continue;
        }
        if (!shortchar_ok(name[i])) return 0;
        if (have_dot) ext++; else base++;
    }
    return (base >= 1u && base <= 8u && ext <= 3u && (!have_dot || ext >= 1u)) ? 1 : 0;
}

static char shortmap(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return shortchar_ok(c) ? c : '_';
}

int hype_fat_shortname_tail(const char *name, unsigned int n, uint8_t out11[11]) {
    unsigned int i, len = 0, dot = 0, bi = 0, ei, digits = 0, v = n, base_cap;
    int have_dot = 0;
    char num[8];

    for (i = 0; i < 11u; i++) out11[i] = ' ';
    while (name[len] != '\0') {
        if (name[len] == '.') { dot = len; have_dot = 1; }
        len++;
    }
    if (!have_dot) dot = len;

    if (n == 0u) return -1;
    while (v > 0u && digits < sizeof num) { num[digits++] = (char)('0' + v % 10u); v /= 10u; }
    if (digits + 1u >= 8u) return -1; /* '~' + digits must leave a base character */
    base_cap = 8u - (digits + 1u);
    if (base_cap > 6u) base_cap = 6u; /* the classic scheme keeps at most six */

    for (i = 0; i < dot && bi < base_cap; i++) {
        if (name[i] == '.' || name[i] == ' ') continue; /* dots/spaces are dropped */
        out11[bi++] = (uint8_t)shortmap(name[i]);
    }
    if (bi == 0u) out11[bi++] = '_'; /* a base of nothing usable still gets a stem */
    out11[bi++] = '~';
    for (i = 0; i < digits; i++) out11[bi++] = (uint8_t)num[digits - 1u - i];
    if (have_dot) {
        for (ei = 0, i = dot + 1u; name[i] != '\0' && ei < 3u; i++) {
            if (name[i] == ' ') continue;
            out11[8u + ei++] = (uint8_t)shortmap(name[i]);
        }
    }
    return 0;
}

/* Byte offsets of the 13 UCS-2 character slots within an LFN entry. */
static const uint8_t lfn_off[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

void hype_fat_lfn_entry_build(uint8_t ent[32], const char *name, unsigned int name_len,
                              unsigned int seq, int last, uint8_t checksum) {
    unsigned int k;
    unsigned int i;
    for (i = 0; i < 32u; i++) ent[i] = 0;
    ent[0] = (uint8_t)(seq | (last ? HYPE_FAT_LFN_LAST : 0u));
    ent[11] = HYPE_FAT_ATTR_LFN;
    ent[12] = 0;        /* type: name entry */
    ent[13] = checksum; /* ties the run to its short name */
    /* first-cluster field (bytes 26..27) must be zero: already is */
    for (k = 0; k < HYPE_FAT_LFN_CHARS; k++) {
        unsigned int ci = (seq - 1u) * HYPE_FAT_LFN_CHARS + k;
        uint16_t u;
        if (ci < name_len) {
            u = (uint16_t)(unsigned char)name[ci];
        } else if (ci == name_len) {
            u = 0x0000u; /* the spec's single NUL terminator */
        } else {
            u = 0xFFFFu; /* fill */
        }
        wr16(ent + lfn_off[k], u);
    }
}

unsigned int hype_fat_lfn_entry_chars(const uint8_t ent[32], char *out) {
    unsigned int seq = (unsigned int)(ent[0] & 0x3Fu);
    unsigned int k;
    if (seq == 0u || (seq - 1u) * HYPE_FAT_LFN_CHARS >= HYPE_FAT_MAX_LFN) return 0;
    for (k = 0; k < HYPE_FAT_LFN_CHARS; k++) {
        unsigned int ci = (seq - 1u) * HYPE_FAT_LFN_CHARS + k;
        uint16_t u = (uint16_t)((uint16_t)ent[lfn_off[k]] | ((uint16_t)ent[lfn_off[k] + 1u] << 8));
        if (ci >= HYPE_FAT_MAX_LFN) break;
        if (u == 0x0000u || u == 0xFFFFu) {
            out[ci] = '\0';
        } else if (u > 0xFFu) {
            out[ci] = 0x7F; /* not representable: never matches an ASCII query */
        } else {
            out[ci] = (char)u;
        }
    }
    return seq;
}
