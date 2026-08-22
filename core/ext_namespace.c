#include "ext_namespace.h"
#include "ext_namespace_impl.h"
#include "lebytes.h"

/*
 * #498: the public dispatcher. ext has no persistent mount state (see
 * core/fs_ops.c's ext_mount), so every call here revalidates from scratch --
 * same as every other ext writer entry point. The only decision made HERE,
 * rather than inside a backend's own open gate, is which backend even
 * applies: COMPAT_HAS_JOURNAL, read directly off the superblock. Everything
 * else (feature gates, checksums, htree, ...) is each backend's own job.
 */

#define SB_MAGIC 0x38u
#define SB_FEATURE_COMPAT 0x5Cu
#define EXT_MAGIC 0xEF53u
#define COMPAT_HAS_JOURNAL 0x0004u

/* 1 == journaled (ext3/4), 0 == ext2, -1 == not a recognisable ext volume at
 * all (the caller's real open call will fail with its own -1 either way). */
static int has_journal(hype_blk_read_fn read, void *ctx) {
    uint8_t sb[1024];
    if (read == 0) return -1;
    if (read(ctx, 2u, 2u, sb) != 0) return -1;
    if (hype_rd16(sb + SB_MAGIC) != EXT_MAGIC) return -1;
    return (hype_rd32(sb + SB_FEATURE_COMPAT) & COMPAT_HAS_JOURNAL) ? 1 : 0;
}

int hype_ext_ns_create(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                       const char *path, uint32_t mtime) {
    int hj = has_journal(read, ctx);
    if (hj < 0) return -1;
    return hj ? hype_extj_ns_create(read, write, ctx, path, mtime)
              : hype_ext2_ns_create(read, write, ctx, path, mtime);
}

int hype_ext_ns_unlink(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                       const char *path, uint32_t mtime) {
    int hj = has_journal(read, ctx);
    if (hj < 0) return -1;
    return hj ? hype_extj_ns_unlink(read, write, ctx, path, mtime)
              : hype_ext2_ns_unlink(read, write, ctx, path, mtime);
}

int hype_ext_ns_mkdir(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                      const char *path, uint32_t mtime) {
    int hj = has_journal(read, ctx);
    if (hj < 0) return -1;
    return hj ? hype_extj_ns_mkdir(read, write, ctx, path, mtime)
              : hype_ext2_ns_mkdir(read, write, ctx, path, mtime);
}

int hype_ext_ns_rmdir(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                      const char *path, uint32_t mtime) {
    int hj = has_journal(read, ctx);
    if (hj < 0) return -1;
    return hj ? hype_extj_ns_rmdir(read, write, ctx, path, mtime)
              : hype_ext2_ns_rmdir(read, write, ctx, path, mtime);
}

int hype_ext_ns_rename(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                       const char *from, const char *to, uint32_t mtime) {
    int hj = has_journal(read, ctx);
    if (hj < 0) return -1;
    return hj ? hype_extj_ns_rename(read, write, ctx, from, to, mtime)
              : hype_ext2_ns_rename(read, write, ctx, from, to, mtime);
}
