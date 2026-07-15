#ifndef HYPE_DEVICES_ATA_DISK_H
#define HYPE_DEVICES_ATA_DISK_H

#include <stdint.h>

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
#define HYPE_ATA_CMD_WRITE_DMA_EXT 0x35u
#define HYPE_ATA_CMD_FLUSH_CACHE_EXT 0xEAu

#define HYPE_ATA_SECTOR_SIZE 512u
#define HYPE_ATA_IDENTIFY_SIZE 512u

/* Status register bits (already partly used by devices/ahci.h's own
 * ATAPI completion path; the full set, for the plain-ATA path). */
#define HYPE_ATA_STATUS_BSY 0x80u
#define HYPE_ATA_STATUS_DRDY 0x40u
#define HYPE_ATA_STATUS_DF 0x20u
#define HYPE_ATA_STATUS_DRQ 0x08u
#define HYPE_ATA_STATUS_ERR 0x01u

typedef struct {
    uint8_t *media;
    uint64_t media_bytes;
    uint64_t total_sectors; /* media_bytes / HYPE_ATA_SECTOR_SIZE */
} hype_ata_disk_t;

/* Resets to a fresh disk backed by `media` (media_bytes must be a
 * whole multiple of HYPE_ATA_SECTOR_SIZE). Pure struct init, no
 * guest-memory access -- mirrors hype_atapi_reset()'s own
 * media/media_size parameters. */
void hype_ata_disk_reset(hype_ata_disk_t *disk, uint8_t *media, uint64_t media_bytes);

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
 * True if [lba, lba+sector_count) lies entirely within the disk's own
 * total_sectors -- the bounds check a real READ/WRITE DMA EXT command
 * must pass before this project's exempt AHCI glue touches the
 * backing buffer at all.
 */
int hype_ata_disk_range_in_bounds(const hype_ata_disk_t *disk, uint64_t lba, uint32_t sector_count);

#endif /* HYPE_DEVICES_ATA_DISK_H */
