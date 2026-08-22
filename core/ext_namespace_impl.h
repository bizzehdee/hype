#ifndef HYPE_CORE_EXT_NAMESPACE_IMPL_H
#define HYPE_CORE_EXT_NAMESPACE_IMPL_H

#include <stdint.h>

#include "blk_io.h"

/*
 * #498: the two backends' actual entry points, split the same way #384/#385
 * are (core/ext2_namespace.c: no journal; core/extj_namespace.c: journaled).
 * NOT part of the public contract -- core/ext_namespace.h's hype_ext_ns_*
 * functions are what every other caller (core/fs_ops.c) uses; this header
 * exists only so core/ext_namespace.c's dispatcher and the two backend .c
 * files agree on one exact signature instead of three independent copies.
 */

int hype_ext2_ns_create(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                        const char *path, uint32_t mtime);
int hype_ext2_ns_unlink(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                        const char *path, uint32_t mtime);
int hype_ext2_ns_mkdir(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                       const char *path, uint32_t mtime);
int hype_ext2_ns_rmdir(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                       const char *path, uint32_t mtime);
int hype_ext2_ns_rename(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                        const char *from, const char *to, uint32_t mtime);

int hype_extj_ns_create(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                        const char *path, uint32_t mtime);
int hype_extj_ns_unlink(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                        const char *path, uint32_t mtime);
int hype_extj_ns_mkdir(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                       const char *path, uint32_t mtime);
int hype_extj_ns_rmdir(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                       const char *path, uint32_t mtime);
int hype_extj_ns_rename(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                        const char *from, const char *to, uint32_t mtime);

#endif /* HYPE_CORE_EXT_NAMESPACE_IMPL_H */
