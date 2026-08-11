#ifndef HYPE_CORE_BLK_IO_H
#define HYPE_CORE_BLK_IO_H

#include <stdint.h>

/*
 * #292: the shared block/file I/O contract, extracted from core/fat.h.
 *
 * These types were born in the FAT reader (#181) and then adopted -- by
 * including fat.h -- everywhere a module needed "read/write N sectors at an
 * LBA" or "a file as a list of on-disk runs": the exFAT and ext drivers, the
 * raw-image backend (blk_image), the USB log sink, iso_stream and boot/main.
 * None of that is FAT-specific, so the FAT header had become the accidental
 * owner of hype's block-I/O vocabulary, and every non-FAT consumer compiled
 * FAT declarations it never used. This header is the neutral owner.
 *
 * The LBA SPACE (volume-relative vs disk-absolute) is deliberately NOT part of
 * these types: it is a property of what the injected callback is wired to.
 * A filesystem resolver's callback is volume-relative (sector 0 = the boot
 * sector); blk_phys hands out disk-absolute I/O; a partition wrapper converts
 * between them by adding the partition's first LBA. Every field of this shape
 * must say which space it is in, as core/blk_image.h and core/iso_stream.h do.
 */

/* One logical sector, everywhere in hype's block world. A medium or volume
 * with a different logical sector size is rejected at mount/probe, never
 * mis-parsed. (Moved here from core/blk_backend.h so the filesystem layer does
 * not need the backend vtable header just for the constant.) */
#define HYPE_BLK_SECTOR_SIZE 512u

/*
 * Reads `count` 512-byte sectors starting at `lba` into `dst`. Returns 0 on
 * success, non-zero on error. `ctx` carries whatever the backend needs (e.g.
 * ABAR+port+partition base for hype_ahci_host_read()).
 */
typedef int (*hype_blk_read_fn)(void *ctx, uint64_t lba, uint32_t count, void *dst);

/* Writes `count` 512-byte sectors from `src` at `lba`. Returns 0 on success,
 * non-zero on error. Mirror of hype_blk_read_fn. */
typedef int (*hype_blk_write_fn)(void *ctx, uint64_t lba, uint32_t count, const void *src);

/* Optional persistence barrier: force everything previously written through
 * the paired hype_blk_write_fn to be durable on the medium. */
typedef int (*hype_blk_sync_fn)(void *ctx);

/* A single contiguous run of a file's data. A contiguous file is one extent;
 * fragmentation adds more, capped by HYPE_FILE_MAX_EXTENTS. */
typedef struct {
    uint64_t start_lba;    /* first LBA of this run (space per the map's producer) */
    uint64_t sector_count; /* length of the run, in 512-byte sectors */
} hype_file_extent_t;

/*
 * #366: 64 was too small to be a limit on hype rather than a limit on the operator's stick.
 *
 * The builders COALESCE adjacent clusters, so this counts real discontiguities, not clusters --
 * 64 of them is a mildly fragmented file, not a pathological one. A 3-5 GB Windows ISO copied
 * onto a volume that was not freshly formatted routinely exceeds it, and core/ext.h notes large
 * indirect-mapped files on ext are STRUCTURALLY fragmented, so staying under 64 there was close to
 * unachievable. The observable effect was that whether an ISO streamed depended on how the stick
 * happened to be laid out, not on the ISO -- the operator's complaint, and the right one.
 *
 * 256 is 4x the headroom at 4 KiB per extent array (16 bytes each). The cost is bounded and known:
 * one array in hype_iso_stream_t per VM, one in hype_blk_image_t per disk, and a 4 KiB frame in
 * the three functions that resolve into a local. Every one of those runs on the BSP during setup,
 * before hype_ap_start(), so none of them lands on the 16 KiB AP stacks.
 *
 * This is a ceiling, not a target. It is still finite, and the resolvers still report hitting it
 * through hype_file_map_t::too_fragmented, so an operator who exceeds even 256 gets the message
 * naming the cap rather than silence.
 */
#define HYPE_FILE_MAX_EXTENTS 256u

/* A file resolved to its on-disk runs -- the contract every host-FS resolver
 * (FAT32, exFAT, ext) produces and every consumer (iso streaming, blk_image,
 * in-place writers) maps offsets through. */
typedef struct {
    hype_file_extent_t extents[HYPE_FILE_MAX_EXTENTS];
    unsigned count;      /* number of extents used */
    uint64_t size_bytes; /* exact file length in bytes */
    /*
     * #366: set when resolution stopped because the file needs MORE than HYPE_FILE_MAX_EXTENTS
     * runs, as opposed to any other failure.
     *
     * Every failure used to collapse into -1, so "this ISO is too fragmented for hype to map" was
     * indistinguishable from "no such file" and "this is not a FAT32 volume". boot/main.c even
     * had a diagnostic written for the fragmentation case -- and it was unreachable, because it
     * sat behind `else if (have_file)` and have_file only gets set when resolve SUCCEEDS.
     *
     * The operator's complaint is the point: whether an ISO streams should not depend on how
     * their stick happens to be laid out. It does today, and until this flag existed it did so
     * without saying why.
     */
    int too_fragmented;
} hype_file_map_t;

#endif /* HYPE_CORE_BLK_IO_H */
