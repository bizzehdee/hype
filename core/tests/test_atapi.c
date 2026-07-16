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
    test_diagnostic_counters();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
