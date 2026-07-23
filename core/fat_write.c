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
                           uint32_t first_cluster, uint32_t size) {
    unsigned int i;
    for (i = 0; i < 32u; i++) ent[i] = 0;
    for (i = 0; i < 11u; i++) ent[i] = name11[i];
    ent[11] = attr;
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
