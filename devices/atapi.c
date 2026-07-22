#include "atapi.h"

uint32_t hype_atapi_read10_size_bucket(uint32_t block_count) {
    if (block_count <= 1u) {
        return 0u;
    }
    if (block_count <= 8u) {
        return 1u;
    }
    if (block_count <= 16u) {
        return 2u;
    }
    if (block_count <= 64u) {
        return 3u;
    }
    if (block_count <= 256u) {
        return 4u;
    }
    return 5u;
}

static void zero_synth(hype_atapi_result_t *out) {
    uint32_t i;
    for (i = 0; i < HYPE_ATAPI_MAX_SYNTH_RESPONSE; i++) {
        out->synth_data[i] = 0;
    }
}

static void set_check_condition(hype_atapi_t *dev, hype_atapi_result_t *out, uint8_t sense_key, uint8_t asc) {
    dev->sense_key = sense_key;
    dev->asc = asc;
    out->status = HYPE_ATAPI_STATUS_CHECK_CONDITION;
    out->uses_media_data = 0;
    out->synth_length = 0;
}

static void handle_test_unit_ready(hype_atapi_t *dev, hype_atapi_result_t *out) {
    zero_synth(out);
    out->uses_media_data = 0;
    out->synth_length = 0;

    if ((dev->media_data == 0 && dev->media_chunks == 0) || dev->media_size == 0) {
        set_check_condition(dev, out, HYPE_ATAPI_SENSE_KEY_NOT_READY, HYPE_ATAPI_ASC_MEDIUM_NOT_PRESENT);
        return;
    }
    dev->sense_key = HYPE_ATAPI_SENSE_KEY_NO_SENSE;
    dev->asc = 0;
    out->status = HYPE_ATAPI_STATUS_GOOD;
}

static void handle_inquiry(hype_atapi_t *dev, hype_atapi_result_t *out) {
    static const char vendor[8] = {'H', 'Y', 'P', 'E', ' ', ' ', ' ', ' '};
    static const char product[16] = {'V', 'I', 'R', 'T', 'U', 'A', 'L', ' ',
                                      'C', 'D', '-', 'R', 'O', 'M', ' ', ' '};
    static const char revision[4] = {'1', '.', '0', ' '};
    int i;

    zero_synth(out);
    out->synth_data[0] = 0x05; /* peripheral qualifier=0, device type=5 (CD-ROM) */
    out->synth_data[1] = 0x80; /* RMB=1 (removable) */
    out->synth_data[2] = 0x00; /* VERSION -- does not claim conformance to a specific spec revision */
    out->synth_data[3] = 0x02; /* response data format = 2 */
    out->synth_data[4] = 31;   /* additional length = 36 - 5 */
    for (i = 0; i < 8; i++) {
        out->synth_data[8 + i] = (uint8_t)vendor[i];
    }
    for (i = 0; i < 16; i++) {
        out->synth_data[16 + i] = (uint8_t)product[i];
    }
    for (i = 0; i < 4; i++) {
        out->synth_data[32 + i] = (uint8_t)revision[i];
    }
    out->synth_length = 36;
    out->uses_media_data = 0;
    out->status = HYPE_ATAPI_STATUS_GOOD;
    dev->sense_key = HYPE_ATAPI_SENSE_KEY_NO_SENSE;
    dev->asc = 0;
}

static void handle_read_capacity(hype_atapi_t *dev, hype_atapi_result_t *out) {
    uint32_t last_lba;

    zero_synth(out);
    out->uses_media_data = 0;

    if ((dev->media_data == 0 && dev->media_chunks == 0) || dev->media_size == 0) {
        set_check_condition(dev, out, HYPE_ATAPI_SENSE_KEY_NOT_READY, HYPE_ATAPI_ASC_MEDIUM_NOT_PRESENT);
        return;
    }

    last_lba = (dev->media_size / HYPE_ATAPI_SECTOR_SIZE) - 1u;
    out->synth_data[0] = (uint8_t)(last_lba >> 24);
    out->synth_data[1] = (uint8_t)(last_lba >> 16);
    out->synth_data[2] = (uint8_t)(last_lba >> 8);
    out->synth_data[3] = (uint8_t)last_lba;
    out->synth_data[4] = (uint8_t)(HYPE_ATAPI_SECTOR_SIZE >> 24);
    out->synth_data[5] = (uint8_t)(HYPE_ATAPI_SECTOR_SIZE >> 16);
    out->synth_data[6] = (uint8_t)(HYPE_ATAPI_SECTOR_SIZE >> 8);
    out->synth_data[7] = (uint8_t)HYPE_ATAPI_SECTOR_SIZE;
    out->synth_length = 8;
    out->status = HYPE_ATAPI_STATUS_GOOD;
    dev->sense_key = HYPE_ATAPI_SENSE_KEY_NO_SENSE;
    dev->asc = 0;
}

/* Shared body for READ(10)/READ(12): both address the media with a 32-bit LBA
 * and a block count -- READ(12) only widens the count field from 16 to 32 bits.
 * The response describes a direct media stream (uses_media_data); the caller's
 * AHCI glue does the actual PRDT copy. */
static void handle_read(hype_atapi_t *dev, uint32_t lba, uint32_t count,
                        hype_atapi_result_t *out) {
    uint32_t total_sectors;

    zero_synth(out);
    out->synth_length = 0;

    if ((dev->media_data == 0 && dev->media_chunks == 0) || dev->media_size == 0) {
        set_check_condition(dev, out, HYPE_ATAPI_SENSE_KEY_NOT_READY, HYPE_ATAPI_ASC_MEDIUM_NOT_PRESENT);
        return;
    }

    total_sectors = dev->media_size / HYPE_ATAPI_SECTOR_SIZE;
    if (count == 0) {
        /* A legal no-op per SPC; nothing to transfer, no error. */
        out->uses_media_data = 0;
        out->status = HYPE_ATAPI_STATUS_GOOD;
        dev->sense_key = HYPE_ATAPI_SENSE_KEY_NO_SENSE;
        dev->asc = 0;
        return;
    }
    if (lba >= total_sectors || count > total_sectors - lba) {
        set_check_condition(dev, out, HYPE_ATAPI_SENSE_KEY_ILLEGAL_REQUEST, HYPE_ATAPI_ASC_LBA_OUT_OF_RANGE);
        return;
    }

    /* task #105 measure-first: record the transfer-size profile of the
     * actual data reads (successful path only -- a no-op or out-of-range
     * READ transfers nothing, so excluding them keeps the profile honest). */
    dev->read10_sectors_total += count;
    if (count > dev->read10_max_count) {
        dev->read10_max_count = count;
    }
    dev->read10_size_hist[hype_atapi_read10_size_bucket(count)]++;

    out->uses_media_data = 1;
    out->media_offset = lba * HYPE_ATAPI_SECTOR_SIZE;
    out->media_length = count * HYPE_ATAPI_SECTOR_SIZE;
    out->status = HYPE_ATAPI_STATUS_GOOD;
    dev->sense_key = HYPE_ATAPI_SENSE_KEY_NO_SENSE;
    dev->asc = 0;
}

/* READ(10): 32-bit LBA at bytes 2-5, 16-bit block count at bytes 7-8. */
static void handle_read10(hype_atapi_t *dev, const uint8_t cdb[HYPE_ATAPI_CDB_MAX],
                          hype_atapi_result_t *out) {
    uint32_t lba = ((uint32_t)cdb[2] << 24) | ((uint32_t)cdb[3] << 16) | ((uint32_t)cdb[4] << 8) |
                   (uint32_t)cdb[5];
    uint32_t count = ((uint32_t)cdb[7] << 8) | (uint32_t)cdb[8];
    handle_read(dev, lba, count, out);
}

/* READ(12): same 32-bit LBA at bytes 2-5, but a 32-bit block count at bytes
 * 6-9. A DMA-capable libata may issue this instead of READ(10); semantics are
 * otherwise identical for a read-only optical device. */
static void handle_read12(hype_atapi_t *dev, const uint8_t cdb[HYPE_ATAPI_CDB_MAX],
                          hype_atapi_result_t *out) {
    uint32_t lba = ((uint32_t)cdb[2] << 24) | ((uint32_t)cdb[3] << 16) | ((uint32_t)cdb[4] << 8) |
                   (uint32_t)cdb[5];
    uint32_t count = ((uint32_t)cdb[6] << 24) | ((uint32_t)cdb[7] << 16) | ((uint32_t)cdb[8] << 8) |
                     (uint32_t)cdb[9];
    handle_read(dev, lba, count, out);
}

static void handle_request_sense(hype_atapi_t *dev, hype_atapi_result_t *out) {
    zero_synth(out);
    out->synth_data[0] = 0x70; /* current errors, fixed format */
    out->synth_data[2] = dev->sense_key;
    out->synth_data[7] = 10; /* additional sense length = 18 - 8 */
    out->synth_data[12] = dev->asc;
    out->synth_data[13] = 0; /* ASCQ -- not tracked separately in this project's minimal scope */
    out->synth_length = 18;
    out->uses_media_data = 0;
    out->status = HYPE_ATAPI_STATUS_GOOD;
    /* REQUEST SENSE reporting the sense data does not itself clear it --
     * matches real devices, which only clear sense state on specific
     * subsequent conditions (a later successful command, in this
     * project's own simplified model -- see each handler above). */
}

/* SCSI multi-byte fields are big-endian; these keep the MMC responses below
 * readable. */
static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* True while a disc is loaded (either backing). Shared by the media-detection
 * handlers, which must report NOT READY with no media exactly like the reads. */
static int media_present(const hype_atapi_t *dev) {
    return (dev->media_data != 0 || dev->media_chunks != 0) && dev->media_size != 0;
}

/* GET CONFIGURATION (0x46): advertise a data-disc drive whose CURRENT profile
 * is DVD-ROM. udev's cdrom_id reads the current-profile field of the feature
 * header to conclude a disc is present (-> ID_CDROM_MEDIA=1). Returns the
 * 8-byte Feature Header plus a Profile List feature (0x0000) listing CD-ROM
 * and DVD-ROM, the latter marked current. RT/starting-feature in the CDB are
 * ignored -- this minimal set is returned for every request type, which every
 * RT value tolerates (RT=2 asks for one feature and still accepts feature 0). */
static void handle_get_configuration(hype_atapi_t *dev, hype_atapi_result_t *out) {
    zero_synth(out);
    out->uses_media_data = 0;
    if (!media_present(dev)) {
        set_check_condition(dev, out, HYPE_ATAPI_SENSE_KEY_NOT_READY, HYPE_ATAPI_ASC_MEDIUM_NOT_PRESENT);
        return;
    }
    /* Feature header: bytes 0-3 data length (filled last), 4-5 reserved,
     * 6-7 current profile. */
    put_be16(out->synth_data + 6, (uint16_t)HYPE_ATAPI_PROFILE_DVD_ROM);
    /* Profile List feature (0x0000). */
    put_be16(out->synth_data + 8, 0x0000u); /* feature code */
    out->synth_data[10] = 0x03u;             /* version=0, persistent=1, current=1 */
    out->synth_data[11] = 8u;                /* additional length: two 4-byte descriptors */
    put_be16(out->synth_data + 12, (uint16_t)HYPE_ATAPI_PROFILE_CD_ROM);
    out->synth_data[14] = 0x00u;             /* CD-ROM present but not current */
    put_be16(out->synth_data + 16, (uint16_t)HYPE_ATAPI_PROFILE_DVD_ROM);
    out->synth_data[18] = 0x01u;             /* DVD-ROM is the current profile */
    put_be32(out->synth_data + 0, 20u - 4u); /* data length = total - 4 */
    out->synth_length = 20;
    out->status = HYPE_ATAPI_STATUS_GOOD;
    dev->sense_key = HYPE_ATAPI_SENSE_KEY_NO_SENSE;
    dev->asc = 0;
}

/* GET EVENT STATUS NOTIFICATION (0x4A): report the Media event class with the
 * disc present. cdrom_id polls this for media presence alongside GET
 * CONFIGURATION. Always returns a media descriptor (NEA=0) rather than "no
 * event available", so a steady-state poll still reflects the loaded disc. */
static void handle_get_event_status(hype_atapi_t *dev, hype_atapi_result_t *out) {
    zero_synth(out);
    out->uses_media_data = 0;
    /* Event header: descriptor length (bytes 0-1) = 6, notification class
     * (byte 2) = 4 (Media), supported classes bitmap (byte 3) = Media (bit 4). */
    put_be16(out->synth_data + 0, 6u);
    out->synth_data[2] = 0x04u;
    out->synth_data[3] = 0x10u;
    /* Media event descriptor: event code (no change), media status. */
    out->synth_data[4] = 0x00u;
    out->synth_data[5] = (uint8_t)(media_present(dev) ? 0x02u : 0x00u); /* bit1 = media present */
    out->synth_data[6] = 0x00u; /* start slot */
    out->synth_data[7] = 0x00u; /* end slot */
    out->synth_length = 8;
    out->status = HYPE_ATAPI_STATUS_GOOD;
    dev->sense_key = HYPE_ATAPI_SENSE_KEY_NO_SENSE;
    dev->asc = 0;
}

/* READ TOC/PMA/ATIP (0x43): a single data track starting at LBA 0 with the
 * lead-out at the media end. Answers the Formatted-TOC (format 0) and
 * multisession (format 1) queries the Linux sr driver issues at probe time;
 * without the latter sr logs "doesn't support multisession CD's" and cdrom_id
 * loses a media-detection fallback. Formats other than 0/1 fall back to the
 * formatted TOC (a valid response the drivers accept). */
static void handle_read_toc(hype_atapi_t *dev, const uint8_t cdb[HYPE_ATAPI_CDB_MAX],
                            hype_atapi_result_t *out) {
    uint32_t total_sectors;
    uint8_t format = (uint8_t)(cdb[2] & 0x0Fu);

    zero_synth(out);
    out->uses_media_data = 0;
    if (!media_present(dev)) {
        set_check_condition(dev, out, HYPE_ATAPI_SENSE_KEY_NOT_READY, HYPE_ATAPI_ASC_MEDIUM_NOT_PRESENT);
        return;
    }
    total_sectors = dev->media_size / HYPE_ATAPI_SECTOR_SIZE;
    if (format == 1u) {
        /* Multisession: one session whose first track (1, data) is at LBA 0. */
        put_be16(out->synth_data + 0, 10u); /* data length = total - 2 */
        out->synth_data[2] = 1u;            /* first complete session */
        out->synth_data[3] = 1u;            /* last complete session */
        out->synth_data[5] = 0x14u;         /* ADR=1, control=4 (data track) */
        out->synth_data[6] = 1u;            /* first track in last session */
        put_be32(out->synth_data + 8, 0u);  /* track start LBA */
        out->synth_length = 12;
    } else {
        /* Formatted TOC (format 0): track 1 (data) + lead-out (track 0xAA). */
        put_be16(out->synth_data + 0, 18u); /* data length = total - 2 */
        out->synth_data[2] = 1u;            /* first track */
        out->synth_data[3] = 1u;            /* last track */
        out->synth_data[5] = 0x14u;         /* track 1 ADR/control: data */
        out->synth_data[6] = 1u;            /* track number 1 */
        put_be32(out->synth_data + 8, 0u);  /* track 1 start LBA */
        out->synth_data[13] = 0x14u;        /* lead-out ADR/control */
        out->synth_data[14] = 0xAAu;        /* lead-out "track" number */
        put_be32(out->synth_data + 16, total_sectors); /* lead-out start LBA */
        out->synth_length = 20;
    }
    out->status = HYPE_ATAPI_STATUS_GOOD;
    dev->sense_key = HYPE_ATAPI_SENSE_KEY_NO_SENSE;
    dev->asc = 0;
}

static void write_swapped_ascii(uint8_t *out, const char *str, uint32_t field_bytes) {
    uint32_t len = 0;
    uint32_t i;

    while (str[len] != '\0') {
        len++;
    }

    /* ATA identify strings are stored with each 16-bit word byte-
     * swapped -- the same convention hype_ata_disk_build_identify()
     * uses for the plain-ATA disk. */
    for (i = 0; i < field_bytes; i += 2u) {
        uint8_t c0 = (i < len) ? (uint8_t)str[i] : (uint8_t)' ';
        uint8_t c1 = (i + 1u < len) ? (uint8_t)str[i + 1u] : (uint8_t)' ';
        out[i] = c1;
        out[i + 1u] = c0;
    }
}

void hype_atapi_build_identify(const hype_atapi_t *dev, uint8_t out[HYPE_ATAPI_IDENTIFY_SIZE]) {
    uint32_t i;

    (void)dev; /* the identify block is fixed for this read-only CD-ROM model */

    for (i = 0; i < HYPE_ATAPI_IDENTIFY_SIZE; i++) {
        out[i] = 0;
    }

    /* Word 0 (general configuration) = 0x85C0: bits 15:14=10b (ATAPI
     * device), bits 12:8=00101b (CD-ROM device type), bits 6:5=10b
     * (DRQ within 50us of receiving PACKET), bits 1:0=00 (12-byte
     * command packet). The standard value a real CD-ROM and QEMU both
     * report, stored little-endian. */
    out[0] = 0xC0u;
    out[1] = 0x85u;

    write_swapped_ascii(out + 20, "HYPE0000000000000001", 20u); /* words 10-19: serial number */
    write_swapped_ascii(out + 46, "1.0", 8u);                   /* words 23-26: firmware revision */
    write_swapped_ascii(out + 54, "HYPE VIRTUAL CD-ROM", 40u);  /* words 27-46: model number */

    /* task #105: advertise DMA so libata drives the ATAPI PACKET reads with the
     * DMA protocol (ATAPI_PROT_DMA) instead of PIO -- matching how a real SATA
     * optical drive is driven and how QEMU's ATAPI model reports itself. The
     * AHCI glue completes DMA and PIO reads identically (a PRDT copy), so this
     * is a correctness/realism fix, not a throughput change (measured: reads
     * already ran DMA-style at tens of MB/s). Words are little-endian: byte
     * [2N] low, [2N+1] high. Every value below mirrors a DMA-capable drive.
     *   w49  = 0x0F00: DMA(8)+LBA(9)+IORDYdis(10)+IORDY(11) supported.
     *   w53  = 0x0006: words 64-70 valid(1) + word 88 valid(2).
     *   w63  = 0x0007: MultiWord DMA modes 0-2 supported (none selected --
     *          UDMA is the active mode below).
     *   w88  = 0x203F: UDMA modes 0-5 supported (0-5), mode 5 selected (13),
     *          so a post-SET-FEATURES re-IDENTIFY sees a consistent state.
     * SET FEATURES (transfer mode) is ack'd no-data by the AHCI glue, so
     * libata's mode selection succeeds and it proceeds in DMA. */
    out[98] = 0x00u;  out[99] = 0x0Fu;  /* word 49 = 0x0F00 */
    out[106] = 0x06u; out[107] = 0x00u; /* word 53 = 0x0006 */
    out[126] = 0x07u; out[127] = 0x00u; /* word 63 = 0x0007 */
    out[176] = 0x3Fu; out[177] = 0x20u; /* word 88 = 0x203F */
}

/* Zero the sense state + all diagnostic/measurement counters. Shared by both
 * reset entry points so the flat and chunked backings behave identically (the
 * chunked path is what FW-1's real guest uses). */
static void reset_state(hype_atapi_t *dev) {
    uint32_t b;
    dev->sense_key = HYPE_ATAPI_SENSE_KEY_NO_SENSE;
    dev->asc = 0;
    dev->command_count = 0;
    dev->read10_count = 0;
    dev->read12_count = 0;
    dev->last_cdb = 0;
    dev->read10_sectors_total = 0;
    dev->read10_max_count = 0;
    for (b = 0; b < HYPE_ATAPI_READ10_HIST_BUCKETS; b++) {
        dev->read10_size_hist[b] = 0;
    }
}

void hype_atapi_reset(hype_atapi_t *dev, const uint8_t *media_data, uint32_t media_size) {
    dev->media_data = media_data;
    dev->media_chunks = 0;
    dev->media_size = media_size;
    reset_state(dev);
}

void hype_atapi_reset_chunked(hype_atapi_t *dev, const hype_chunked_iso_t *iso) {
    dev->media_data = 0;
    dev->media_chunks = iso;
    /* media_size is a uint32; a >4GB ISO would truncate here. Both current
     * server ISOs are < 4GB (Fedora 3.64GB, Ubuntu 2.72GB) so this is fine,
     * but a >=4GB ISO needs media_size widened (tracked with GLADDER-10). */
    dev->media_size = (uint32_t)(iso ? iso->total_bytes : 0);
    reset_state(dev);
}

void hype_atapi_execute_cdb(hype_atapi_t *dev, const uint8_t cdb[HYPE_ATAPI_CDB_MAX],
                            hype_atapi_result_t *out) {
    /* Diagnostic bookkeeping (see hype_atapi_t): every CDB counts, so a
     * caller can report CD progress without tracing each command. */
    dev->command_count++;
    dev->last_cdb = cdb[0];
    if (cdb[0] == HYPE_ATAPI_CMD_READ10) {
        dev->read10_count++;
    } else if (cdb[0] == HYPE_ATAPI_CMD_READ12) {
        dev->read12_count++;
    }
    switch (cdb[0]) {
        case HYPE_ATAPI_CMD_TEST_UNIT_READY:
            handle_test_unit_ready(dev, out);
            return;
        case HYPE_ATAPI_CMD_INQUIRY:
            handle_inquiry(dev, out);
            return;
        case HYPE_ATAPI_CMD_READ_CAPACITY:
            handle_read_capacity(dev, out);
            return;
        case HYPE_ATAPI_CMD_READ10:
            handle_read10(dev, cdb, out);
            return;
        case HYPE_ATAPI_CMD_READ12:
            handle_read12(dev, cdb, out);
            return;
        case HYPE_ATAPI_CMD_REQUEST_SENSE:
            handle_request_sense(dev, out);
            return;
        case HYPE_ATAPI_CMD_READ_TOC:
            handle_read_toc(dev, cdb, out);
            return;
        case HYPE_ATAPI_CMD_GET_CONFIGURATION:
            handle_get_configuration(dev, out);
            return;
        case HYPE_ATAPI_CMD_GET_EVENT_STATUS:
            handle_get_event_status(dev, out);
            return;
        default:
            set_check_condition(dev, out, HYPE_ATAPI_SENSE_KEY_ILLEGAL_REQUEST,
                                 HYPE_ATAPI_ASC_INVALID_COMMAND_OPCODE);
            zero_synth(out);
            return;
    }
}
