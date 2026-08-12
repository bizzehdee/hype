#include "jbd2.h"
#include "lebytes.h"

/* On-disk formats from the jbd2 layout (all BIG-endian, unlike ext itself). */

#define SECSZ HYPE_BLK_SECTOR_SIZE

#define JBD2_MAGIC 0xC03B3998u
#define BT_DESCRIPTOR 1u
#define BT_COMMIT 2u
#define BT_SB_V1 3u
#define BT_SB_V2 4u

/* journal superblock offsets (after the 12-byte header) */
#define JSB_BLOCKSIZE 12u
#define JSB_MAXLEN 16u
#define JSB_FIRST 20u
#define JSB_SEQUENCE 24u
#define JSB_START 28u
#define JSB_ERRNO 32u
#define JSB_FEAT_COMPAT 36u
#define JSB_FEAT_INCOMPAT 40u
#define JSB_FEAT_ROCOMPAT 44u
#define JSB_UUID 48u

/* descriptor tags (the classic, no-csum, 32-bit form) */
#define TAG_FLAG_ESCAPE 1u
#define TAG_FLAG_SAME_UUID 2u
#define TAG_FLAG_LAST 8u

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void bcopy8(uint8_t *dst, const uint8_t *src, unsigned n) {
    unsigned i;
    for (i = 0; i < n; i++) dst[i] = src[i];
}
static void bzero8(uint8_t *p, unsigned n) {
    unsigned i;
    for (i = 0; i < n; i++) p[i] = 0;
}

/* Read/write journal-file block `jb` through the journal inode's map. */
static int jblock_read(hype_jbd2_t *j, uint32_t jb, uint8_t *out) {
    return hype_file_rmap_read_at(&j->map, j->read, j->ctx, (uint64_t)jb * j->block_size, out,
                                  j->block_size);
}

static int jblock_write(hype_jbd2_t *j, uint32_t jb, const uint8_t *data) {
    hype_range_kind_t kind;
    uint64_t lba, run;
    uint32_t head;
    uint64_t off = (uint64_t)jb * j->block_size;
    uint32_t s;

    /* the journal file is fully allocated by mkfs: every block must be DATA */
    for (s = 0; s < j->spb; s++) {
        if (hype_file_rmap_locate(&j->map, off + (uint64_t)s * SECSZ, &kind, &lba, &head, &run) !=
                0 ||
            kind != HYPE_RANGE_DATA || head != 0u) {
            return -1;
        }
        if (j->write(j->ctx, lba, 1u, data + (uint64_t)s * SECSZ) != 0) {
            return -1;
        }
    }
    return 0;
}

int hype_jbd2_open(hype_jbd2_t *j, hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                   uint32_t block_size, const hype_file_rmap_t *map) {
    static uint8_t sb[4096];
    unsigned i;

    if (read == 0 || write == 0 || block_size < 1024u || block_size > 4096u) return -1;
    j->read = read;
    j->write = write;
    j->ctx = ctx;
    j->block_size = block_size;
    j->spb = block_size / SECSZ;
    /* copy the map field-by-field (freestanding: no struct assignment) */
    hype_file_rmap_init(&j->map, map->size_bytes);
    for (i = 0; i < map->count; i++) {
        j->map.ranges[i].kind = map->ranges[i].kind;
        j->map.ranges[i].start_lba = map->ranges[i].start_lba;
        j->map.ranges[i].sector_count = map->ranges[i].sector_count;
        if (map->ranges[i].kind != HYPE_RANGE_DATA) {
            return -1; /* a sparse journal is corruption */
        }
    }
    j->map.count = map->count;

    if (jblock_read(j, 0u, sb) != 0) return -1;
    if (rd32be(sb + 0) != JBD2_MAGIC) return -1;
    if (rd32be(sb + 4) != BT_SB_V2) return -1; /* V1 journals predate ext3 itself */
    if (rd32be(sb + JSB_BLOCKSIZE) != block_size) return -1;
    j->maxlen = rd32be(sb + JSB_MAXLEN);
    j->first = rd32be(sb + JSB_FIRST);
    j->sequence = rd32be(sb + JSB_SEQUENCE);
    if (j->maxlen < 8u || j->first == 0u || j->first >= j->maxlen) return -1;
    if ((uint64_t)j->maxlen * block_size > j->map.size_bytes) return -1;
    if (rd32be(sb + JSB_ERRNO) != 0u) return -1; /* the journal recorded an error */
    /* feature gates: hype understands the plain 32-bit no-csum journal only */
    if (rd32be(sb + JSB_FEAT_INCOMPAT) != 0u) return -1; /* 64BIT/CSUM/ASYNC/FAST_COMMIT */
    if (rd32be(sb + JSB_FEAT_ROCOMPAT) != 0u) return -1;
    /* non-empty == a crashed writer's transactions await replay: refuse */
    if (rd32be(sb + JSB_START) != 0u) return -1;
    if (j->sequence == 0u) j->sequence = 1u;
    bcopy8(j->uuid, sb + JSB_UUID, 16u);
    return 0;
}

/* Rewrite the journal superblock with the given start/sequence. */
static int jsb_update(hype_jbd2_t *j, uint32_t start, uint32_t sequence) {
    static uint8_t sb[4096];
    if (jblock_read(j, 0u, sb) != 0) return -1;
    wr32be(sb + JSB_START, start);
    wr32be(sb + JSB_SEQUENCE, sequence);
    return jblock_write(j, 0u, sb);
}

int hype_jbd2_commit(hype_jbd2_t *j, const hype_jbd2_block_t *blocks, unsigned count) {
    static uint8_t desc[4096];
    static uint8_t esc[4096];
    static uint8_t cmt[4096];
    uint32_t jb = j->first;
    unsigned i;
    uint32_t doff;

    if (count == 0u || count > HYPE_JBD2_MAX_BLOCKS) return -1;
    if (2u + count > j->maxlen - j->first) return -1; /* must fit without wrapping */

    /*
     * One descriptor block covers this bounded transaction comfortably: the
     * classic tag is 8 bytes (+16 UUID on the first), so 24 tags need at
     * most 8 + 24*8 + 16 = 216 bytes of a >= 1024-byte block.
     */
    bzero8(desc, j->block_size);
    wr32be(desc + 0, JBD2_MAGIC);
    wr32be(desc + 4, BT_DESCRIPTOR);
    wr32be(desc + 8, j->sequence);
    doff = 12u;
    for (i = 0; i < count; i++) {
        uint32_t flags = 0;
        if (rd32be(blocks[i].data) == JBD2_MAGIC) flags |= TAG_FLAG_ESCAPE;
        if (i != 0u) flags |= TAG_FLAG_SAME_UUID;
        if (i == count - 1u) flags |= TAG_FLAG_LAST;
        if (blocks[i].blocknr > 0xFFFFFFFFull) return -1; /* 32-bit journals only */
        wr32be(desc + doff, (uint32_t)blocks[i].blocknr);
        wr32be(desc + doff + 4, flags);
        doff += 8u;
        if (i == 0u) {
            bcopy8(desc + doff, j->uuid, 16u);
            doff += 16u;
        }
        if (doff + 8u > j->block_size) return -1;
    }
    if (jblock_write(j, jb, desc) != 0) return -1;
    jb++;

    /* the block images, escaped where needed */
    for (i = 0; i < count; i++) {
        const uint8_t *img = blocks[i].data;
        if (rd32be(img) == JBD2_MAGIC) {
            bcopy8(esc, img, j->block_size);
            bzero8(esc, 4u); /* the replayer restores the magic from the flag */
            img = esc;
        }
        if (jblock_write(j, jb, img) != 0) return -1;
        jb++;
    }

    /* the commit block makes the transaction real */
    bzero8(cmt, j->block_size);
    wr32be(cmt + 0, JBD2_MAGIC);
    wr32be(cmt + 4, BT_COMMIT);
    wr32be(cmt + 8, j->sequence);
    if (jblock_write(j, jb, cmt) != 0) return -1;

    /* expose it: from here a crash replays these images */
    return jsb_update(j, j->first, j->sequence);
}

int hype_jbd2_checkpoint(hype_jbd2_t *j) {
    if (jsb_update(j, 0u, j->sequence + 1u) != 0) return -1;
    j->sequence++;
    return 0;
}
