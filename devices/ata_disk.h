#ifndef HYPE_DEVICES_ATA_DISK_H
#define HYPE_DEVICES_ATA_DISK_H

#include <stdint.h>
#include "../core/blk_backend.h"

/*
 * M5-2: a plain ATA hard-disk device behind its own, second AHCI HBA
 * instance (devices/ahci.h) -- genuine ATA commands (IDENTIFY DEVICE,
 * READ/WRITE DMA EXT) carried directly in a SATA Register FIS, no
 * ATAPI/SCSI-CDB indirection at all (that's M4-5's own, entirely
 * separate optical-drive device, devices/atapi.h). Deliberately a
 * SECOND, independent AHCI controller/PCI function rather than a
 * second port on the existing single-port hype_ahci_t -- that struct
 * was written for exactly one port total (see its own top comment);
 * extending it to genuinely multi-port would mean touching M4-5's
 * already-tested code for no real benefit, when two independent
 * single-port controllers (a real, valid hardware topology too) get
 * the same result with zero risk to working code.
 *
 * ATA command byte values, the H2D Register FIS's own ATA-specific
 * field layout (devices/ahci.h's hype_ahci_decode_h2d_fis()), 48-bit
 * LBA/count encoding, IDENTIFY DEVICE response field offsets, and
 * status-register semantics were fetched and confirmed against the
 * Linux kernel's own include/linux/ata.h plus QEMU's hw/ide/ahci.c
 * (handle_reg_h2d_fis()), not reconstructed from memory -- same
 * discipline as this project's other wire-format structs. Backed by a
 * fixed in-memory buffer for now -- a real host-file-backed store is
 * M5-3's own job ("blk_backend"), matching M5-1's own identical
 * scope-narrowing.
 */

#define HYPE_ATA_CMD_IDENTIFY_DEVICE 0xECu
#define HYPE_ATA_CMD_READ_DMA_EXT 0x25u
/* #325: PACKET -- carries an ATAPI CDB rather than an ATA command. Used HOST-side to read a real
 * optical drive; hype's own guest ATAPI model answers these rather than issuing them. */
#define HYPE_ATA_CMD_PACKET 0xA0u
#define HYPE_ATA_CMD_WRITE_DMA_EXT 0x35u
#define HYPE_ATA_CMD_FLUSH_CACHE_EXT 0xEAu
#define HYPE_ATA_CMD_READ_DMA 0xC8u
#define HYPE_ATA_CMD_WRITE_DMA 0xCAu
#define HYPE_ATA_CMD_FLUSH_CACHE 0xE7u
#define HYPE_ATA_CMD_STANDBY_IMMEDIATE 0xE0u
#define HYPE_ATA_CMD_SET_FEATURES 0xEFu

#define HYPE_ATA_SECTOR_SIZE 512u
#define HYPE_ATA_IDENTIFY_SIZE 512u

/* Status register bits (already partly used by devices/ahci.h's own
 * ATAPI completion path; the full set, for the plain-ATA path). */
#define HYPE_ATA_STATUS_BSY 0x80u
#define HYPE_ATA_STATUS_DRDY 0x40u
/* Seek/Drive Seek Complete. Real drives assert it with DRDY on a good completion,
 * and hype's ATAPI model already returns DRDY|DSC (0x50) on the path the guest
 * firmware is known to accept. */
#define HYPE_ATA_STATUS_DSC 0x10u
#define HYPE_ATA_STATUS_DF 0x20u
#define HYPE_ATA_STATUS_DRQ 0x08u
#define HYPE_ATA_STATUS_ERR 0x01u

typedef struct {
    uint8_t *media;
    hype_blk_backend_t *be; /* #262: when set, storage comes from here, not `media` */
    const uint8_t *identify_override; /* #262 discriminator; 0 = synthesise normally */
    uint64_t media_bytes;
    uint64_t total_sectors; /* media_bytes / HYPE_ATA_SECTOR_SIZE */
} hype_ata_disk_t;

/* Resets to a fresh disk backed by `media` (media_bytes must be a
 * whole multiple of HYPE_ATA_SECTOR_SIZE). Pure struct init, no
 * guest-memory access -- mirrors hype_atapi_reset()'s own
 * media/media_size parameters. */
void hype_ata_disk_reset(hype_ata_disk_t *disk, uint8_t *media, uint64_t media_bytes);

/*
 * #262 slice 1: back this disk with a hype_blk_backend_t instead of a fixed RAM
 * buffer, so an ATA disk can serve a real file or physical target -- the same shape
 * virtio-blk was given in #205, where the device model takes the backend rather than
 * owning storage.
 *
 * WHY THIS EXISTS. hype installs a guest to a real disk and cannot then boot it,
 * because the guest firmware will not boot hype's virtio-blk (#262). The firmware DOES
 * already boot this controller class -- every guest today boots the ATAPI optical
 * drive on AHCI -- so presenting the installed disk as SATA is the shorter route to a
 * bootable guest. That needs the ATA path to reach a real backend, which is this.
 *
 * `media` stays supported and takes precedence when no backend is set, so M5-2's
 * microtest (a fixed RAM buffer) is untouched.
 */
void hype_ata_disk_set_backend(hype_ata_disk_t *disk, hype_blk_backend_t *be);

/*
 * #262 discriminator hook: serve `id` (512 bytes) verbatim as this disk's IDENTIFY
 * DEVICE response instead of the synthesised one. Exists to answer one question --
 * is the guest firmware rejecting the disk because of the IDENTIFY CONTENT, or for
 * some other reason -- by handing it a response a real disk gave and the same
 * firmware demonstrably accepted. Pass 0 to go back to the synthesised response.
 * Not a production path: nothing sets this unless HYPE_262_USE_ACCEPTED_ID is on.
 */
void hype_ata_disk_set_identify_override(hype_ata_disk_t *disk, const uint8_t *id);

/*
 * Convert a PRD's byte range within a transfer into whole sectors.
 *
 * Returns 0 and fills *lba_off (sectors from the command's starting LBA) and *nsec on
 * success; -1 if the range is not sector-aligned in both offset and length. Refusing
 * is deliberate: ATA DMA transfers whole sectors and real PRD tables are aligned, so a
 * split sector means either a guest bug or an assumption of ours being wrong, and
 * silently read-modify-writing around it would hide that. Pure: unit-tested.
 */
int hype_ata_prd_sector_range(uint64_t byte_off, uint32_t byte_len, uint64_t *lba_off,
                              uint32_t *nsec);

/*
 * Synthesizes a 512-byte IDENTIFY DEVICE response into `out`.
 * Deliberately minimal -- only the fields a real driver actually
 * checks to accept the device and learn its capacity: word 0 (general
 * config, ATA not ATAPI), word 49 (LBA supported), words 60-61 (28-bit
 * LBA capacity, capped at 0x0FFFFFFF if the real capacity exceeds it,
 * the standard convention signaling "see the 48-bit fields instead"),
 * words 83/86 (48-bit Address feature set supported/enabled, plus the
 * words-82-84 validity marker bits 15:14 = 0:1), words 100-103 (48-bit
 * LBA capacity), and a fixed model-string/serial/firmware-revision
 * (ASCII, byte-swapped per word -- the real ATA convention: each
 * word's first character in the high byte, second in the low byte).
 * Pure, deterministic given `disk`'s own state.
 */
void hype_ata_disk_build_identify(const hype_ata_disk_t *disk, uint8_t out[HYPE_ATA_IDENTIFY_SIZE]);

/*
 * Resolves a raw 16-bit Count field (H2D FIS bytes 12-13) into an
 * actual sector count, applying the real ATA "0 means the maximum"
 * convention for 48-bit EXT commands (0 -> 65536, not 0 sectors).
 */
uint32_t hype_ata_disk_resolve_sector_count(uint16_t raw_count);

/*
 * The 28-bit counterparts. libata does NOT use the EXT commands for everything:
 * ata_build_rw_tf() prefers 28-bit READ/WRITE DMA whenever the request fits, so a
 * model that only handles 0x25/0x35 sees the probe succeed and then every real
 * block read arrive as an unmodelled 0xC8.
 *
 * A 28-bit command splits its LBA differently -- bits 0-23 in the three LBA bytes,
 * bits 24-27 in the low nibble of the device register (the high LBA bytes are not
 * used) -- and its count is 8-bit with 0 meaning 256, not the 48-bit rule of 65536.
 * Decoding one as the other silently reads the wrong sector.
 */
uint64_t hype_ata_lba28_from_fis(uint64_t fis_lba, uint8_t device);
uint32_t hype_ata_resolve_sector_count28(uint16_t raw_count);
int hype_ata_cmd_is_lba48(uint8_t command);

/*
 * True if [lba, lba+sector_count) lies entirely within the disk's own
 * total_sectors -- the bounds check a real READ/WRITE DMA EXT command
 * must pass before this project's exempt AHCI glue touches the
 * backing buffer at all.
 */
int hype_ata_disk_range_in_bounds(const hype_ata_disk_t *disk, uint64_t lba, uint32_t sector_count);

#endif /* HYPE_DEVICES_ATA_DISK_H */
