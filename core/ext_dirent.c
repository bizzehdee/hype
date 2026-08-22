#include "ext_dirent.h"
#include "ext_csum.h"
#include "lebytes.h"

/* See ext_dirent.h. */

#define DE_INODE 0u
#define DE_REC_LEN 4u
#define DE_NAME_LEN 6u
#define DE_FILE_TYPE 7u
#define DE_NAME 8u

#define TAIL_LEN 12u
#define TAIL_FT 0xDEu

static uint32_t usable_size(uint32_t block_size, int has_tail) {
    return has_tail ? block_size - TAIL_LEN : block_size;
}

uint32_t hype_extd_reclen(unsigned int name_len) {
    return (DE_NAME + (uint32_t)name_len + 3u) & ~3u;
}

static int name_eq(const uint8_t *block, uint32_t off, const char *name, unsigned int name_len) {
    unsigned int i;
    for (i = 0; i < name_len; i++) {
        if (block[off + DE_NAME + i] != (uint8_t)name[i]) {
            return 0;
        }
    }
    return 1;
}

static void tail_write(uint8_t *block, uint32_t block_size) {
    uint32_t off = block_size - TAIL_LEN;
    hype_wr32(block + off + 0, 0u);
    hype_wr16(block + off + 4, (uint16_t)TAIL_LEN);
    block[off + 6] = 0u;
    block[off + 7] = (uint8_t)TAIL_FT;
    hype_wr32(block + off + 8, 0u);
}

void hype_extd_block_init(uint8_t *block, uint32_t block_size, int has_tail) {
    uint32_t usable = usable_size(block_size, has_tail);
    hype_wr32(block + DE_INODE, 0u);
    hype_wr16(block + DE_REC_LEN, (uint16_t)usable);
    block[DE_NAME_LEN] = 0u;
    block[DE_FILE_TYPE] = 0u;
    if (has_tail) {
        tail_write(block, block_size);
    }
}

int hype_extd_validate(const uint8_t *block, uint32_t block_size, int has_tail) {
    uint32_t usable = usable_size(block_size, has_tail);
    uint32_t off = 0;

    if (block_size < DE_NAME || (has_tail && block_size < TAIL_LEN + DE_NAME)) {
        return -1;
    }
    while (off < usable) {
        uint32_t rec_len = hype_rd16(block + off + DE_REC_LEN);
        uint8_t nl = block[off + DE_NAME_LEN];
        if (rec_len < DE_NAME || (rec_len & 3u) != 0u || off + rec_len > usable) {
            return -1;
        }
        if (hype_rd32(block + off + DE_INODE) != 0u && off + DE_NAME + nl > usable) {
            return -1;
        }
        off += rec_len;
    }
    /* the loop's own per-entry bound (`off + rec_len > usable` above) already
     * guarantees off never overshoots usable, so exiting the loop always
     * means off == usable exactly -- nothing further to check here. */
    if (has_tail) {
        uint32_t t = block_size - TAIL_LEN;
        if (hype_rd32(block + t + 0) != 0u || hype_rd16(block + t + 4) != (uint16_t)TAIL_LEN ||
            block[t + 6] != 0u || block[t + 7] != (uint8_t)TAIL_FT) {
            return -1; /* not the shape a metadata_csum tail must have */
        }
    }
    return 0;
}

int hype_extd_find(const uint8_t *block, uint32_t block_size, int has_tail, const char *name,
                   unsigned int name_len, uint32_t *out_off, uint32_t *out_ino) {
    uint32_t usable = usable_size(block_size, has_tail);
    uint32_t off = 0;

    while (off < usable) {
        uint32_t ino = hype_rd32(block + off + DE_INODE);
        uint32_t rec_len = hype_rd16(block + off + DE_REC_LEN);
        uint8_t nl = block[off + DE_NAME_LEN];
        if (ino != 0u && nl == name_len && name_eq(block, off, name, name_len)) {
            *out_off = off;
            *out_ino = ino;
            return 1;
        }
        off += rec_len;
    }
    return 0;
}

int hype_extd_insert(uint8_t *block, uint32_t block_size, int has_tail, uint32_t ino,
                     const char *name, unsigned int name_len, uint8_t file_type) {
    uint32_t usable = usable_size(block_size, has_tail);
    uint32_t needed = hype_extd_reclen(name_len);
    uint32_t off = 0;
    unsigned int i;

    while (off < usable) {
        uint32_t e_ino = hype_rd32(block + off + DE_INODE);
        uint32_t rec_len = hype_rd16(block + off + DE_REC_LEN);
        uint8_t nl = block[off + DE_NAME_LEN];
        uint32_t real_len = (e_ino != 0u) ? hype_extd_reclen(nl) : 0u;

        if (e_ino == 0u && rec_len >= needed) {
            hype_wr32(block + off + DE_INODE, ino);
            /* rec_len stays: reusing the whole freed/free slot is always
             * valid on disk, and avoids leaving yet another tiny free record
             * behind for no benefit this writer needs. */
            block[off + DE_NAME_LEN] = (uint8_t)name_len;
            block[off + DE_FILE_TYPE] = file_type;
            for (i = 0; i < name_len; i++) {
                block[off + DE_NAME + i] = (uint8_t)name[i];
            }
            return 0;
        }
        if (e_ino != 0u && rec_len - real_len >= needed) {
            uint32_t new_off = off + real_len;
            uint32_t new_len = rec_len - real_len;
            hype_wr16(block + off + DE_REC_LEN, (uint16_t)real_len);
            hype_wr32(block + new_off + DE_INODE, ino);
            hype_wr16(block + new_off + DE_REC_LEN, (uint16_t)new_len);
            block[new_off + DE_NAME_LEN] = (uint8_t)name_len;
            block[new_off + DE_FILE_TYPE] = file_type;
            for (i = 0; i < name_len; i++) {
                block[new_off + DE_NAME + i] = (uint8_t)name[i];
            }
            return 0;
        }
        off += rec_len;
    }
    return -1;
}

int hype_extd_remove(uint8_t *block, uint32_t block_size, int has_tail, const char *name,
                     unsigned int name_len) {
    uint32_t usable = usable_size(block_size, has_tail);
    uint32_t off = 0;
    uint32_t prev_off = usable; /* usable == "no predecessor in this block" */

    while (off < usable) {
        uint32_t ino = hype_rd32(block + off + DE_INODE);
        uint32_t rec_len = hype_rd16(block + off + DE_REC_LEN);
        uint8_t nl = block[off + DE_NAME_LEN];
        if (ino != 0u && nl == name_len && name_eq(block, off, name, name_len)) {
            if (prev_off == usable) {
                hype_wr32(block + off + DE_INODE, 0u); /* first in block: stays, freed */
            } else {
                uint32_t prev_len = hype_rd16(block + prev_off + DE_REC_LEN);
                hype_wr16(block + prev_off + DE_REC_LEN, (uint16_t)(prev_len + rec_len));
            }
            return 1;
        }
        prev_off = off;
        off += rec_len;
    }
    return 0;
}

int hype_extd_only_dots(const uint8_t *block, uint32_t block_size, int has_tail) {
    uint32_t usable = usable_size(block_size, has_tail);
    uint32_t off = 0;

    while (off < usable) {
        uint32_t ino = hype_rd32(block + off + DE_INODE);
        uint32_t rec_len = hype_rd16(block + off + DE_REC_LEN);
        uint8_t nl = block[off + DE_NAME_LEN];
        if (ino != 0u) {
            int is_dot = (nl == 1u && block[off + DE_NAME] == '.');
            int is_dotdot = (nl == 2u && block[off + DE_NAME] == '.' && block[off + DE_NAME + 1] == '.');
            if (!is_dot && !is_dotdot) {
                return 0;
            }
        }
        off += rec_len;
    }
    return 1;
}

void hype_extd_csum_finalize(uint8_t *block, uint32_t block_size, int has_tail,
                             uint32_t i_csum_seed) {
    uint32_t crc;
    if (!has_tail) {
        return;
    }
    crc = hype_ext_crc32c(i_csum_seed, block, block_size - TAIL_LEN);
    hype_wr32(block + (block_size - TAIL_LEN) + 8, crc);
}
