#ifndef HYPE_CORE_EXT_CSUM_H
#define HYPE_CORE_EXT_CSUM_H

#include <stdint.h>

/*
 * #495: the two metadata-checksum algorithms ext4 refused without --
 * METADATA_CSUM (crc32c, seeded from the filesystem UUID) and the older
 * GDT_CSUM (crc16, "uninit_bg"). Both are RAW, chainable CRC updates: the
 * caller supplies the initial state (0xFFFFFFFFu / 0xFFFFu to start a fresh
 * checksum) and neither function complements its result. ext4 threads a crc
 * across several buffers (the FS UUID, then a group number, then a
 * structure) by feeding one call's return value in as the next call's seed;
 * the final chained value is stored on disk exactly as returned -- unlike a
 * "textbook" CRC-32/CRC-16, there is no XOR-out. Verified against e2fsprogs
 * lib/ext2fs/{crc32c,csum}.c and the Linux kernel's
 * fs/ext4/{super,extents,namei,inode}.c + lib/crc/crc16.c -- both e2fsck's
 * and the kernel's own reference implementations -- not from memory.
 */

uint32_t hype_ext_crc32c(uint32_t seed, const void *data, unsigned int len);
uint16_t hype_ext_crc16(uint16_t seed, const void *data, unsigned int len);

#endif /* HYPE_CORE_EXT_CSUM_H */
