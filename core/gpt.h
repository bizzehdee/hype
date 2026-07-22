#ifndef HYPE_CORE_GPT_H
#define HYPE_CORE_GPT_H

#include <stdint.h>

/*
 * GLADDER-10 (streaming ISO backend): a minimal, read-only GPT partition-table
 * parser. The streaming source stores the installer ISO on its OWN raw
 * partition (alongside the FAT ESP that holds hype.efi); this locates that
 * partition's LBA range so a guest CD read at ISO offset O maps to disk LBA
 * `first_lba + O/512`. No filesystem parsing -- that (FAT32/exFAT, for a
 * file-on-ESP layout) is the follow-up (#181).
 *
 * Pure logic driven by an injected sector-read callback (the host_pci.c /
 * ahci_host.c dependency-injection pattern), unit-tested against a synthetic
 * GPT. Field offsets are the stable UEFI GPT spec layout (§5.3).
 */

#define HYPE_GPT_HEADER_LBA 1u  /* the primary GPT header always lives at LBA 1 */
#define HYPE_GPT_SECTOR_SIZE 512u

/*
 * Reads `count` 512-byte sectors starting at `lba` into `dst`. Returns 0 on
 * success, non-zero on error. (`ctx` carries whatever the caller needs -- e.g.
 * the ABAR + port for hype_ahci_host_read().)
 */
typedef int (*hype_gpt_read_lba_fn)(void *ctx, uint64_t lba, uint32_t count, void *dst);

typedef struct {
    uint64_t first_lba;  /* partition's first LBA (inclusive) */
    uint64_t last_lba;   /* partition's last LBA (inclusive) */
    uint64_t size_bytes; /* (last_lba - first_lba + 1) * 512 */
} hype_gpt_partition_t;

/*
 * Finds the `index`-th (1-based) USED partition entry (one whose 16-byte type
 * GUID is not all-zero) and fills *out. Returns 0 on success, -1 if the GPT
 * header is invalid ("EFI PART" signature / sane geometry), the index exceeds
 * the used-partition count, or a sector read fails. Read-only.
 */
int hype_gpt_find_partition(hype_gpt_read_lba_fn read, void *ctx, unsigned index,
                            hype_gpt_partition_t *out);

/*
 * Reads the 16-byte Disk GUID from the primary GPT header (LBA 1, offset 0x38,
 * §5.3.2) into `out_guid`. Returns 0 on success, -1 if the header is invalid
 * ("EFI PART" signature) or the sector read fails. Read-only. This is the
 * partition-table-level disk identity (M10-2 "serial/GUID"), complementing the
 * hardware serial from hype_ahci_host_parse_identify().
 */
int hype_gpt_disk_guid(hype_gpt_read_lba_fn read, void *ctx, uint8_t out_guid[16]);

#endif /* HYPE_CORE_GPT_H */
