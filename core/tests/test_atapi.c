#include <stdio.h>
#include "../../devices/atapi.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void make_cdb(uint8_t cdb[HYPE_ATAPI_CDB_MAX], uint8_t opcode) {
    int i;
    for (i = 0; i < HYPE_ATAPI_CDB_MAX; i++) {
        cdb[i] = 0;
    }
    cdb[0] = opcode;
}

static uint8_t g_media[4 * HYPE_ATAPI_SECTOR_SIZE];

static void init_media(void) {
    uint32_t i;
    for (i = 0; i < sizeof(g_media); i++) {
        g_media[i] = (uint8_t)(i & 0xFFu);
    }
}

static void test_test_unit_ready_with_media(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];

    hype_atapi_reset(&dev, g_media, sizeof(g_media));
    make_cdb(cdb, HYPE_ATAPI_CMD_TEST_UNIT_READY);
    hype_atapi_execute_cdb(&dev, cdb, &out);

    CHECK_HEX("status GOOD", HYPE_ATAPI_STATUS_GOOD, out.status);
}

static void test_test_unit_ready_no_media(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];

    hype_atapi_reset(&dev, 0, 0);
    make_cdb(cdb, HYPE_ATAPI_CMD_TEST_UNIT_READY);
    hype_atapi_execute_cdb(&dev, cdb, &out);

    CHECK_HEX("status CHECK_CONDITION", HYPE_ATAPI_STATUS_CHECK_CONDITION, out.status);
    CHECK_HEX("sense key NOT_READY", HYPE_ATAPI_SENSE_KEY_NOT_READY, dev.sense_key);
    CHECK_HEX("asc MEDIUM_NOT_PRESENT", HYPE_ATAPI_ASC_MEDIUM_NOT_PRESENT, dev.asc);
}

static void test_inquiry(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];

    hype_atapi_reset(&dev, g_media, sizeof(g_media));
    make_cdb(cdb, HYPE_ATAPI_CMD_INQUIRY);
    hype_atapi_execute_cdb(&dev, cdb, &out);

    CHECK_HEX("status GOOD", HYPE_ATAPI_STATUS_GOOD, out.status);
    CHECK_HEX("uses synthesized data, not media", 0, out.uses_media_data);
    CHECK_HEX("response length 36", 36, out.synth_length);
    CHECK_HEX("peripheral device type = CD-ROM (5)", 0x05, out.synth_data[0] & 0x1Fu);
    CHECK_HEX("RMB bit set (removable)", 1, (out.synth_data[1] & 0x80u) != 0);
    CHECK_HEX("vendor id starts with 'H'", 'H', out.synth_data[8]);
    CHECK_HEX("product id starts with 'V'", 'V', out.synth_data[16]);
}

static void test_read_capacity(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];
    uint32_t last_lba, block_len;

    hype_atapi_reset(&dev, g_media, sizeof(g_media));
    make_cdb(cdb, HYPE_ATAPI_CMD_READ_CAPACITY);
    hype_atapi_execute_cdb(&dev, cdb, &out);

    CHECK_HEX("status GOOD", HYPE_ATAPI_STATUS_GOOD, out.status);
    CHECK_HEX("response length 8", 8, out.synth_length);

    last_lba = ((uint32_t)out.synth_data[0] << 24) | ((uint32_t)out.synth_data[1] << 16) |
               ((uint32_t)out.synth_data[2] << 8) | out.synth_data[3];
    block_len = ((uint32_t)out.synth_data[4] << 24) | ((uint32_t)out.synth_data[5] << 16) |
                ((uint32_t)out.synth_data[6] << 8) | out.synth_data[7];

    CHECK_HEX("last LBA is (4 sectors - 1)", 3, last_lba);
    CHECK_HEX("block length is 2048", HYPE_ATAPI_SECTOR_SIZE, block_len);
}

static void test_read_capacity_no_media(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];

    hype_atapi_reset(&dev, 0, 0);
    make_cdb(cdb, HYPE_ATAPI_CMD_READ_CAPACITY);
    hype_atapi_execute_cdb(&dev, cdb, &out);

    CHECK_HEX("status CHECK_CONDITION", HYPE_ATAPI_STATUS_CHECK_CONDITION, out.status);
    CHECK_HEX("sense key NOT_READY", HYPE_ATAPI_SENSE_KEY_NOT_READY, dev.sense_key);
}

static void test_read10_valid(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];

    hype_atapi_reset(&dev, g_media, sizeof(g_media));
    make_cdb(cdb, HYPE_ATAPI_CMD_READ10);
    /* LBA = 2, transfer length = 1 block */
    cdb[2] = 0;
    cdb[3] = 0;
    cdb[4] = 0;
    cdb[5] = 2;
    cdb[7] = 0;
    cdb[8] = 1;
    hype_atapi_execute_cdb(&dev, cdb, &out);

    CHECK_HEX("status GOOD", HYPE_ATAPI_STATUS_GOOD, out.status);
    CHECK_HEX("streams directly from media_data", 1, out.uses_media_data);
    CHECK_HEX("media_offset = LBA 2 * 2048", 2u * HYPE_ATAPI_SECTOR_SIZE, out.media_offset);
    CHECK_HEX("media_length = 1 * 2048", HYPE_ATAPI_SECTOR_SIZE, out.media_length);
}

static void test_read10_zero_count_is_noop(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];

    hype_atapi_reset(&dev, g_media, sizeof(g_media));
    make_cdb(cdb, HYPE_ATAPI_CMD_READ10);
    hype_atapi_execute_cdb(&dev, cdb, &out); /* count defaults to 0 via make_cdb's zeroing */

    CHECK_HEX("status GOOD (legal no-op)", HYPE_ATAPI_STATUS_GOOD, out.status);
    CHECK_HEX("no data to transfer", 0, out.uses_media_data);
}

static void test_read10_out_of_range(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];

    hype_atapi_reset(&dev, g_media, sizeof(g_media));
    make_cdb(cdb, HYPE_ATAPI_CMD_READ10);
    cdb[5] = 100; /* LBA 100, media only has 4 sectors */
    cdb[8] = 1;
    hype_atapi_execute_cdb(&dev, cdb, &out);

    CHECK_HEX("status CHECK_CONDITION", HYPE_ATAPI_STATUS_CHECK_CONDITION, out.status);
    CHECK_HEX("sense key ILLEGAL_REQUEST", HYPE_ATAPI_SENSE_KEY_ILLEGAL_REQUEST, dev.sense_key);
    CHECK_HEX("asc LBA_OUT_OF_RANGE", HYPE_ATAPI_ASC_LBA_OUT_OF_RANGE, dev.asc);
}

static void test_read10_count_spans_past_end(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];

    hype_atapi_reset(&dev, g_media, sizeof(g_media));
    make_cdb(cdb, HYPE_ATAPI_CMD_READ10);
    cdb[5] = 3;   /* last valid LBA */
    cdb[8] = 2;   /* but asks for 2 sectors -- would run past the 4-sector media */
    hype_atapi_execute_cdb(&dev, cdb, &out);

    CHECK_HEX("status CHECK_CONDITION", HYPE_ATAPI_STATUS_CHECK_CONDITION, out.status);
}

static void test_read10_no_media(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];

    hype_atapi_reset(&dev, 0, 0);
    make_cdb(cdb, HYPE_ATAPI_CMD_READ10);
    cdb[8] = 1;
    hype_atapi_execute_cdb(&dev, cdb, &out);

    CHECK_HEX("status CHECK_CONDITION", HYPE_ATAPI_STATUS_CHECK_CONDITION, out.status);
    CHECK_HEX("sense key NOT_READY", HYPE_ATAPI_SENSE_KEY_NOT_READY, dev.sense_key);
}

static void test_request_sense_reflects_last_failure(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];

    hype_atapi_reset(&dev, g_media, sizeof(g_media));
    make_cdb(cdb, HYPE_ATAPI_CMD_READ10);
    cdb[5] = 100;
    cdb[8] = 1;
    hype_atapi_execute_cdb(&dev, cdb, &out); /* fails, sets sense state */

    make_cdb(cdb, HYPE_ATAPI_CMD_REQUEST_SENSE);
    hype_atapi_execute_cdb(&dev, cdb, &out);

    CHECK_HEX("status GOOD (REQUEST SENSE itself always succeeds)", HYPE_ATAPI_STATUS_GOOD, out.status);
    CHECK_HEX("response length 18", 18, out.synth_length);
    CHECK_HEX("response code 0x70 (current, fixed format)", 0x70, out.synth_data[0]);
    CHECK_HEX("sense key reflects the prior failure", HYPE_ATAPI_SENSE_KEY_ILLEGAL_REQUEST,
              out.synth_data[2]);
    CHECK_HEX("ASC reflects the prior failure", HYPE_ATAPI_ASC_LBA_OUT_OF_RANGE, out.synth_data[12]);
}

static void test_request_sense_no_sense_by_default(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];

    hype_atapi_reset(&dev, g_media, sizeof(g_media));
    make_cdb(cdb, HYPE_ATAPI_CMD_REQUEST_SENSE);
    hype_atapi_execute_cdb(&dev, cdb, &out);

    CHECK_HEX("sense key NO_SENSE", HYPE_ATAPI_SENSE_KEY_NO_SENSE, out.synth_data[2]);
}

static void test_unrecognized_opcode_rejected(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];

    hype_atapi_reset(&dev, g_media, sizeof(g_media));
    make_cdb(cdb, 0x7Fu); /* not one of this project's supported commands */
    hype_atapi_execute_cdb(&dev, cdb, &out);

    CHECK_HEX("status CHECK_CONDITION", HYPE_ATAPI_STATUS_CHECK_CONDITION, out.status);
    CHECK_HEX("sense key ILLEGAL_REQUEST", HYPE_ATAPI_SENSE_KEY_ILLEGAL_REQUEST, dev.sense_key);
    CHECK_HEX("asc INVALID_COMMAND_OPCODE", HYPE_ATAPI_ASC_INVALID_COMMAND_OPCODE, dev.asc);
}

static void test_build_identify_packet_device(void) {
    hype_atapi_t dev;
    uint8_t id[HYPE_ATAPI_IDENTIFY_SIZE];
    uint16_t word0;

    hype_atapi_reset(&dev, g_media, sizeof(g_media));
    hype_atapi_build_identify(&dev, id);

    word0 = (uint16_t)((uint16_t)id[0] | ((uint16_t)id[1] << 8));
    CHECK_HEX("word 0 = 0x85C0 (ATAPI CD-ROM, 12-byte packet)", 0x85C0u, word0);
    CHECK_HEX("bits 15:14 = 10b (ATAPI protocol)", 0x2u, (word0 >> 14) & 0x3u);
    CHECK_HEX("bits 12:8 = 00101b (CD-ROM device type)", 0x05u, (word0 >> 8) & 0x1Fu);
    CHECK_HEX("bits 1:0 = 00 (12-byte command packet)", 0x0u, word0 & 0x3u);
    /* Model string (words 27-46) is byte-swapped ASCII: "HYPE..." -> the
     * first stored byte is the second character ('Y'). */
    CHECK_HEX("model string byte-swapped ('Y' first)", 'Y', id[54]);
    CHECK_HEX("model string byte-swapped ('H' second)", 'H', id[55]);
}

static uint16_t id_word(const uint8_t *id, unsigned w) {
    return (uint16_t)((uint16_t)id[2u * w] | ((uint16_t)id[2u * w + 1u] << 8));
}

static void test_build_identify_dma_words(void) {
    /* task #105: the IDENTIFY must advertise DMA so libata drives the CD with
     * the DMA protocol instead of PIO. */
    hype_atapi_t dev;
    uint8_t id[HYPE_ATAPI_IDENTIFY_SIZE];

    hype_atapi_reset(&dev, g_media, sizeof(g_media));
    hype_atapi_build_identify(&dev, id);

    CHECK_HEX("word 49 = 0x0F00 (DMA+LBA+IORDY)", 0x0F00u, id_word(id, 49));
    CHECK_HEX("word 49 bit 8 (DMA supported) set", 1u, (id_word(id, 49) >> 8) & 1u);
    CHECK_HEX("word 53 = 0x0006 (w64-70 + w88 valid)", 0x0006u, id_word(id, 53));
    CHECK_HEX("word 53 bit 2 (word 88 valid) set", 1u, (id_word(id, 53) >> 2) & 1u);
    CHECK_HEX("word 63 = 0x0007 (MWDMA 0-2 supported)", 0x0007u, id_word(id, 63));
    CHECK_HEX("word 88 = 0x203F (UDMA 0-5 supported, mode 5 selected)", 0x203Fu, id_word(id, 88));
    CHECK_HEX("word 88 bit 13 (UDMA5 selected) set", 1u, (id_word(id, 88) >> 13) & 1u);
    /* word 0 must still mark an ATAPI CD-ROM -- the DMA words don't disturb it. */
    CHECK_HEX("word 0 still 0x85C0", 0x85C0u, id_word(id, 0));
}

static void test_read12_valid(void) {
    /* READ(12): 32-bit LBA at bytes 2-5, 32-bit block count at bytes 6-9. */
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];

    hype_atapi_reset(&dev, g_media, sizeof(g_media)); /* 4-sector media */
    make_cdb(cdb, HYPE_ATAPI_CMD_READ12);
    cdb[5] = 1;  /* LBA = 1 */
    cdb[9] = 3;  /* count = 3 blocks (bytes 6-9, big-endian) */
    hype_atapi_execute_cdb(&dev, cdb, &out);

    CHECK_HEX("READ(12) status GOOD", HYPE_ATAPI_STATUS_GOOD, out.status);
    CHECK_HEX("READ(12) streams from media", 1, out.uses_media_data);
    CHECK_HEX("READ(12) media_offset = LBA 1 * 2048", 1u * HYPE_ATAPI_SECTOR_SIZE, out.media_offset);
    CHECK_HEX("READ(12) media_length = 3 * 2048", 3u * HYPE_ATAPI_SECTOR_SIZE, out.media_length);
    CHECK_HEX("read12_count incremented", 1, dev.read12_count);
    CHECK_HEX("read10_count untouched by READ(12)", 0, dev.read10_count);
    /* size profile shared with READ(10): 3 blocks -> bucket 1 (2-8). */
    CHECK_HEX("READ(12) recorded in size profile", 3, dev.read10_sectors_total);
    CHECK_HEX("READ(12) size bucket 1", 1, dev.read10_size_hist[1]);
}

static void test_read12_out_of_range(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];

    hype_atapi_reset(&dev, g_media, sizeof(g_media));
    make_cdb(cdb, HYPE_ATAPI_CMD_READ12);
    cdb[5] = 100; /* LBA 100, past 4-sector media */
    cdb[9] = 1;
    hype_atapi_execute_cdb(&dev, cdb, &out);

    CHECK_HEX("READ(12) OOR -> CHECK_CONDITION", HYPE_ATAPI_STATUS_CHECK_CONDITION, out.status);
    CHECK_HEX("READ(12) OOR sense ILLEGAL_REQUEST", HYPE_ATAPI_SENSE_KEY_ILLEGAL_REQUEST, dev.sense_key);
    CHECK_HEX("READ(12) OOR not in size profile", 0, dev.read10_sectors_total);
}

static void test_diagnostic_counters(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];

    hype_atapi_reset(&dev, g_media, sizeof(g_media));
    CHECK_HEX("reset zeroes command_count", 0, dev.command_count);
    CHECK_HEX("reset zeroes read10_count", 0, dev.read10_count);

    make_cdb(cdb, HYPE_ATAPI_CMD_INQUIRY);
    hype_atapi_execute_cdb(&dev, cdb, &out);
    CHECK_HEX("command_count after 1 cmd", 1, dev.command_count);
    CHECK_HEX("last_cdb = INQUIRY", HYPE_ATAPI_CMD_INQUIRY, dev.last_cdb);
    CHECK_HEX("read10_count still 0", 0, dev.read10_count);

    make_cdb(cdb, HYPE_ATAPI_CMD_READ10);
    cdb[8] = 1; /* 1 sector */
    hype_atapi_execute_cdb(&dev, cdb, &out);
    hype_atapi_execute_cdb(&dev, cdb, &out);
    CHECK_HEX("command_count after 3 cmds", 3, dev.command_count);
    CHECK_HEX("read10_count after 2 reads", 2, dev.read10_count);
    CHECK_HEX("last_cdb = READ10", HYPE_ATAPI_CMD_READ10, dev.last_cdb);
}

static void test_read10_size_bucket_boundaries(void) {
    /* Pure classifier: 1 / 2-8 / 9-16 / 17-64 / 65-256 / >256 blocks. */
    CHECK_HEX("bucket(0) -> 0 (no-op)", 0, hype_atapi_read10_size_bucket(0));
    CHECK_HEX("bucket(1) -> 0", 0, hype_atapi_read10_size_bucket(1));
    CHECK_HEX("bucket(2) -> 1", 1, hype_atapi_read10_size_bucket(2));
    CHECK_HEX("bucket(8) -> 1", 1, hype_atapi_read10_size_bucket(8));
    CHECK_HEX("bucket(9) -> 2", 2, hype_atapi_read10_size_bucket(9));
    CHECK_HEX("bucket(16) -> 2", 2, hype_atapi_read10_size_bucket(16));
    CHECK_HEX("bucket(17) -> 3", 3, hype_atapi_read10_size_bucket(17));
    CHECK_HEX("bucket(64) -> 3", 3, hype_atapi_read10_size_bucket(64));
    CHECK_HEX("bucket(65) -> 4", 4, hype_atapi_read10_size_bucket(65));
    CHECK_HEX("bucket(256) -> 4", 4, hype_atapi_read10_size_bucket(256));
    CHECK_HEX("bucket(257) -> 5", 5, hype_atapi_read10_size_bucket(257));
    CHECK_HEX("bucket(65535) -> 5", 5, hype_atapi_read10_size_bucket(65535));
}

static void test_read10_size_profile_accumulates(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];

    hype_atapi_reset(&dev, g_media, sizeof(g_media)); /* 4-sector media */
    CHECK_HEX("reset zeroes sectors_total", 0, dev.read10_sectors_total);
    CHECK_HEX("reset zeroes max_count", 0, dev.read10_max_count);
    CHECK_HEX("reset zeroes hist[0]", 0, dev.read10_size_hist[0]);
    CHECK_HEX("reset zeroes hist[1]", 0, dev.read10_size_hist[1]);

    /* 1-sector read at LBA 0 -> bucket 0, total 1, max 1. */
    make_cdb(cdb, HYPE_ATAPI_CMD_READ10);
    cdb[8] = 1;
    hype_atapi_execute_cdb(&dev, cdb, &out);
    CHECK_HEX("sectors_total after 1-blk", 1, dev.read10_sectors_total);
    CHECK_HEX("max_count after 1-blk", 1, dev.read10_max_count);
    CHECK_HEX("hist[0] after 1-blk", 1, dev.read10_size_hist[0]);

    /* 4-sector read at LBA 0 -> bucket 1, total 5, max 4. */
    make_cdb(cdb, HYPE_ATAPI_CMD_READ10);
    cdb[8] = 4;
    hype_atapi_execute_cdb(&dev, cdb, &out);
    CHECK_HEX("sectors_total after 4-blk", 5, dev.read10_sectors_total);
    CHECK_HEX("max_count tracks largest", 4, dev.read10_max_count);
    CHECK_HEX("hist[1] after 4-blk", 1, dev.read10_size_hist[1]);

    /* A no-op (count 0) and an out-of-range read must NOT be counted --
     * they transfer no data, so including them would skew the profile. */
    make_cdb(cdb, HYPE_ATAPI_CMD_READ10); /* count 0 */
    hype_atapi_execute_cdb(&dev, cdb, &out);
    make_cdb(cdb, HYPE_ATAPI_CMD_READ10);
    cdb[5] = 100; /* LBA 100, past 4-sector media */
    cdb[8] = 1;
    hype_atapi_execute_cdb(&dev, cdb, &out);
    CHECK_HEX("sectors_total unchanged by no-op/OOR", 5, dev.read10_sectors_total);
    CHECK_HEX("max_count unchanged by no-op/OOR", 4, dev.read10_max_count);
    CHECK_HEX("hist[0] unchanged by no-op/OOR", 1, dev.read10_size_hist[0]);
}

static void test_get_configuration_reports_media(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];
    unsigned cur_profile;

    hype_atapi_reset(&dev, g_media, sizeof(g_media));
    make_cdb(cdb, HYPE_ATAPI_CMD_GET_CONFIGURATION);
    hype_atapi_execute_cdb(&dev, cdb, &out);

    CHECK_HEX("status GOOD", HYPE_ATAPI_STATUS_GOOD, out.status);
    CHECK_HEX("uses synthesized data", 0, out.uses_media_data);
    CHECK_HEX("response length 20", 20, out.synth_length);
    /* Feature-header data length = total - 4. */
    CHECK_HEX("data length field = 16", 16u, ((unsigned)out.synth_data[2] << 8) | out.synth_data[3]);
    cur_profile = ((unsigned)out.synth_data[6] << 8) | out.synth_data[7];
    CHECK_HEX("current profile = DVD-ROM", HYPE_ATAPI_PROFILE_DVD_ROM, cur_profile);
    CHECK_HEX("profile-list feature code 0x0000", 0x0000u,
              ((unsigned)out.synth_data[8] << 8) | out.synth_data[9]);
    CHECK_HEX("DVD-ROM descriptor marked current", 1u, out.synth_data[18] & 0x01u);
}

static void test_get_configuration_no_media(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];

    hype_atapi_reset(&dev, 0, 0);
    make_cdb(cdb, HYPE_ATAPI_CMD_GET_CONFIGURATION);
    hype_atapi_execute_cdb(&dev, cdb, &out);

    CHECK_HEX("status CHECK_CONDITION", HYPE_ATAPI_STATUS_CHECK_CONDITION, out.status);
    CHECK_HEX("sense key NOT_READY", HYPE_ATAPI_SENSE_KEY_NOT_READY, dev.sense_key);
}

static void test_get_event_status_media_present(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];

    hype_atapi_reset(&dev, g_media, sizeof(g_media));
    make_cdb(cdb, HYPE_ATAPI_CMD_GET_EVENT_STATUS);
    hype_atapi_execute_cdb(&dev, cdb, &out);

    CHECK_HEX("status GOOD", HYPE_ATAPI_STATUS_GOOD, out.status);
    CHECK_HEX("response length 8", 8, out.synth_length);
    CHECK_HEX("notification class = media (4)", 0x04u, out.synth_data[2] & 0x07u);
    CHECK_HEX("media-present bit set", 0x02u, out.synth_data[5] & 0x02u);
}

static void test_read_toc_formatted(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];
    unsigned leadout_lba;

    hype_atapi_reset(&dev, g_media, sizeof(g_media)); /* 4-sector media */
    make_cdb(cdb, HYPE_ATAPI_CMD_READ_TOC);
    cdb[2] = 0x00; /* format 0: formatted TOC */
    hype_atapi_execute_cdb(&dev, cdb, &out);

    CHECK_HEX("status GOOD", HYPE_ATAPI_STATUS_GOOD, out.status);
    CHECK_HEX("response length 20", 20, out.synth_length);
    CHECK_HEX("first track = 1", 1u, out.synth_data[2]);
    CHECK_HEX("last track = 1", 1u, out.synth_data[3]);
    CHECK_HEX("track 1 is a data track (control nibble 0x4)", 0x14u, out.synth_data[5]);
    CHECK_HEX("lead-out track number 0xAA", 0xAAu, out.synth_data[14]);
    leadout_lba = ((unsigned)out.synth_data[16] << 24) | ((unsigned)out.synth_data[17] << 16) |
                  ((unsigned)out.synth_data[18] << 8) | out.synth_data[19];
    CHECK_HEX("lead-out LBA = total sectors (4)", 4u, leadout_lba);
}

static void test_read_toc_multisession(void) {
    hype_atapi_t dev;
    hype_atapi_result_t out;
    uint8_t cdb[HYPE_ATAPI_CDB_MAX];

    hype_atapi_reset(&dev, g_media, sizeof(g_media));
    make_cdb(cdb, HYPE_ATAPI_CMD_READ_TOC);
    cdb[2] = 0x01; /* format 1: multisession info */
    hype_atapi_execute_cdb(&dev, cdb, &out);

    CHECK_HEX("status GOOD", HYPE_ATAPI_STATUS_GOOD, out.status);
    CHECK_HEX("response length 12", 12, out.synth_length);
    CHECK_HEX("first session = 1", 1u, out.synth_data[2]);
    CHECK_HEX("last session = 1", 1u, out.synth_data[3]);
}

int main(void) {
    init_media();

    test_test_unit_ready_with_media();
    test_test_unit_ready_no_media();
    test_inquiry();
    test_read_capacity();
    test_read_capacity_no_media();
    test_read10_valid();
    test_read10_zero_count_is_noop();
    test_read10_out_of_range();
    test_read10_count_spans_past_end();
    test_read10_no_media();
    test_request_sense_reflects_last_failure();
    test_request_sense_no_sense_by_default();
    test_unrecognized_opcode_rejected();
    test_build_identify_packet_device();
    test_build_identify_dma_words();
    test_read12_valid();
    test_read12_out_of_range();
    test_diagnostic_counters();
    test_read10_size_bucket_boundaries();
    test_read10_size_profile_accumulates();
    test_get_configuration_reports_media();
    test_get_configuration_no_media();
    test_get_event_status_media_present();
    test_read_toc_formatted();
    test_read_toc_multisession();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
