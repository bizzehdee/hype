#include "atapi.h"

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

    if (dev->media_data == 0 || dev->media_size == 0) {
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

    if (dev->media_data == 0 || dev->media_size == 0) {
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

static void handle_read10(hype_atapi_t *dev, const uint8_t cdb[HYPE_ATAPI_CDB_MAX],
                          hype_atapi_result_t *out) {
    uint32_t lba = ((uint32_t)cdb[2] << 24) | ((uint32_t)cdb[3] << 16) | ((uint32_t)cdb[4] << 8) |
                   (uint32_t)cdb[5];
    uint32_t count = ((uint32_t)cdb[7] << 8) | (uint32_t)cdb[8];
    uint32_t total_sectors;

    zero_synth(out);
    out->synth_length = 0;

    if (dev->media_data == 0 || dev->media_size == 0) {
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

    out->uses_media_data = 1;
    out->media_offset = lba * HYPE_ATAPI_SECTOR_SIZE;
    out->media_length = count * HYPE_ATAPI_SECTOR_SIZE;
    out->status = HYPE_ATAPI_STATUS_GOOD;
    dev->sense_key = HYPE_ATAPI_SENSE_KEY_NO_SENSE;
    dev->asc = 0;
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
}

void hype_atapi_reset(hype_atapi_t *dev, const uint8_t *media_data, uint32_t media_size) {
    dev->media_data = media_data;
    dev->media_size = media_size;
    dev->sense_key = HYPE_ATAPI_SENSE_KEY_NO_SENSE;
    dev->asc = 0;
}

void hype_atapi_execute_cdb(hype_atapi_t *dev, const uint8_t cdb[HYPE_ATAPI_CDB_MAX],
                            hype_atapi_result_t *out) {
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
        case HYPE_ATAPI_CMD_REQUEST_SENSE:
            handle_request_sense(dev, out);
            return;
        default:
            set_check_condition(dev, out, HYPE_ATAPI_SENSE_KEY_ILLEGAL_REQUEST,
                                 HYPE_ATAPI_ASC_INVALID_COMMAND_OPCODE);
            zero_synth(out);
            return;
    }
}
