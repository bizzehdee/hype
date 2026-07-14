#include <stdio.h>
#include "../../devices/pflash.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static uint8_t g_backing[8192];

static void reset_backing(void) {
    unsigned i;
    for (i = 0; i < sizeof(g_backing); i++) {
        g_backing[i] = 0xABu;
    }
}

static void test_reset_defaults(void) {
    hype_pflash_t pf;
    reset_backing();
    hype_pflash_reset(&pf, g_backing, sizeof(g_backing));
    CHECK_HEX("starts in read-array mode", HYPE_PFLASH_MODE_READ_ARRAY, pf.mode);
    CHECK_HEX("status starts ready, no errors", HYPE_PFLASH_STATUS_READY, pf.status);
}

static void test_read_array_reflects_backing(void) {
    hype_pflash_t pf;
    uint32_t value;
    reset_backing();
    g_backing[0x10] = 0x11;
    g_backing[0x11] = 0x22;
    g_backing[0x12] = 0x33;
    g_backing[0x13] = 0x44;
    hype_pflash_reset(&pf, g_backing, sizeof(g_backing));

    CHECK_HEX("1-byte array read", 0x11, (hype_pflash_read(&pf, 0x10, 1, &value), value));
    CHECK_HEX("4-byte array read is little-endian assembled", 0x44332211,
              (hype_pflash_read(&pf, 0x10, 4, &value), value));
}

static void test_read_out_of_range_rejected(void) {
    hype_pflash_t pf;
    uint32_t value = 0xDEADBEEF;
    reset_backing();
    hype_pflash_reset(&pf, g_backing, sizeof(g_backing));

    CHECK_HEX("read at size is out of range", 1, hype_pflash_read(&pf, sizeof(g_backing), 1, &value) != 0);
    CHECK_HEX("4-byte read overrunning the end is rejected", 1,
              hype_pflash_read(&pf, sizeof(g_backing) - 2, 4, &value) != 0);
}

static void test_read_status_and_devid_replicate_across_width(void) {
    hype_pflash_t pf;
    uint32_t value;
    reset_backing();
    hype_pflash_reset(&pf, g_backing, sizeof(g_backing));

    hype_pflash_write(&pf, 0, 1, HYPE_PFLASH_CMD_READ_STATUS);
    hype_pflash_read(&pf, 0, 4, &value);
    CHECK_HEX("status replicated across all 4 bytes", 0x80808080u, value);

    hype_pflash_write(&pf, 0, 1, HYPE_PFLASH_CMD_READ_DEVID);
    CHECK_HEX("mode is read-devid", HYPE_PFLASH_MODE_READ_DEVID, pf.mode);
    hype_pflash_read(&pf, 0, 2, &value);
    CHECK_HEX("devid read succeeds (unconfirmed exact value, see pflash.c)", 0, value & 0xFF00u);

    hype_pflash_write(&pf, 0, 1, HYPE_PFLASH_CMD_READ_ARRAY);
    CHECK_HEX("READ_ARRAY command returns to array mode", HYPE_PFLASH_MODE_READ_ARRAY, pf.mode);
}

static void test_write_byte_programs_backing(void) {
    hype_pflash_t pf;
    uint32_t value;
    reset_backing();
    hype_pflash_reset(&pf, g_backing, sizeof(g_backing));

    CHECK_HEX("WRITE_BYTE command accepted", 0, hype_pflash_write(&pf, 0x20, 1, HYPE_PFLASH_CMD_WRITE_BYTE));
    CHECK_HEX("mode is write-pending", HYPE_PFLASH_MODE_WRITE_BYTE_PENDING, pf.mode);
    CHECK_HEX("data write accepted", 0, hype_pflash_write(&pf, 0x20, 1, 0x5A));
    CHECK_HEX("mode returns to array after programming", HYPE_PFLASH_MODE_READ_ARRAY, pf.mode);

    hype_pflash_read(&pf, 0x20, 1, &value);
    CHECK_HEX("programmed byte is readable back", 0x5A, value);
}

static void test_write_byte_out_of_range_sets_program_error(void) {
    hype_pflash_t pf;
    reset_backing();
    hype_pflash_reset(&pf, g_backing, sizeof(g_backing));

    hype_pflash_write(&pf, sizeof(g_backing) - 1, 1, HYPE_PFLASH_CMD_WRITE_BYTE);
    CHECK_HEX("4-byte program overrunning the end is rejected", 1,
              hype_pflash_write(&pf, sizeof(g_backing) - 1, 4, 0x11223344) != 0);
    CHECK_HEX("PROGRAM_ERROR status bit set", HYPE_PFLASH_STATUS_PROGRAM_ERROR,
              pf.status & HYPE_PFLASH_STATUS_PROGRAM_ERROR);
}

static void test_block_erase_sets_erased_bytes_to_0xff(void) {
    hype_pflash_t pf;
    uint32_t value;
    reset_backing();
    hype_pflash_reset(&pf, g_backing, sizeof(g_backing));

    hype_pflash_write(&pf, 0x1000, 1, HYPE_PFLASH_CMD_BLOCK_ERASE);
    CHECK_HEX("mode is erase-pending", HYPE_PFLASH_MODE_ERASE_PENDING, pf.mode);
    CHECK_HEX("confirm at the same offset succeeds", 0,
              hype_pflash_write(&pf, 0x1000, 1, HYPE_PFLASH_CMD_ERASE_CONFIRM));
    CHECK_HEX("mode returns to array after erase", HYPE_PFLASH_MODE_READ_ARRAY, pf.mode);

    hype_pflash_read(&pf, 0x1000, 1, &value);
    CHECK_HEX("erased byte reads as 0xFF", 0xFF, value);
    hype_pflash_read(&pf, 0x1FFF, 1, &value);
    CHECK_HEX("erased block's last byte reads as 0xFF", 0xFF, value);

    /* Bytes outside the erased 4KB block must be untouched. */
    hype_pflash_read(&pf, 0x0FFF, 1, &value);
    CHECK_HEX("byte just before the erased block is untouched", 0xAB, value);
    hype_pflash_read(&pf, 0x2000, 1, &value);
    CHECK_HEX("byte just after the erased block is untouched", 0xAB, value);
}

static void test_block_erase_wrong_confirm_offset_rejected(void) {
    hype_pflash_t pf;
    reset_backing();
    hype_pflash_reset(&pf, g_backing, sizeof(g_backing));

    hype_pflash_write(&pf, 0x1000, 1, HYPE_PFLASH_CMD_BLOCK_ERASE);
    CHECK_HEX("confirm at a different (but still in-range) offset is rejected", 1,
              hype_pflash_write(&pf, 0x0500, 1, HYPE_PFLASH_CMD_ERASE_CONFIRM) != 0);
    CHECK_HEX("PROGRAM_ERROR status bit set", HYPE_PFLASH_STATUS_PROGRAM_ERROR,
              pf.status & HYPE_PFLASH_STATUS_PROGRAM_ERROR);
}

static void test_block_erase_wrong_confirm_byte_rejected(void) {
    hype_pflash_t pf;
    reset_backing();
    hype_pflash_reset(&pf, g_backing, sizeof(g_backing));

    hype_pflash_write(&pf, 0x1000, 1, HYPE_PFLASH_CMD_BLOCK_ERASE);
    CHECK_HEX("wrong confirm byte is rejected", 1, hype_pflash_write(&pf, 0x1000, 1, 0xAA) != 0);
}

static void test_clear_status(void) {
    hype_pflash_t pf;
    reset_backing();
    hype_pflash_reset(&pf, g_backing, sizeof(g_backing));

    pf.status |= HYPE_PFLASH_STATUS_PROGRAM_ERROR;
    hype_pflash_write(&pf, 0, 1, HYPE_PFLASH_CMD_CLEAR_STATUS);
    CHECK_HEX("CLEAR_STATUS resets to ready-only", HYPE_PFLASH_STATUS_READY, pf.status);
    CHECK_HEX("CLEAR_STATUS returns to array mode", HYPE_PFLASH_MODE_READ_ARRAY, pf.mode);
}

static void test_unrecognized_command_rejected(void) {
    hype_pflash_t pf;
    reset_backing();
    hype_pflash_reset(&pf, g_backing, sizeof(g_backing));

    CHECK_HEX("unrecognized command byte is rejected", 1, hype_pflash_write(&pf, 0, 1, 0x33) != 0);
    CHECK_HEX("mode unchanged after rejected command", HYPE_PFLASH_MODE_READ_ARRAY, pf.mode);
}

static void test_write_and_read_out_of_range_offset_rejected(void) {
    hype_pflash_t pf;
    uint32_t value;
    reset_backing();
    hype_pflash_reset(&pf, g_backing, sizeof(g_backing));

    CHECK_HEX("write at/beyond size is rejected", 1,
              hype_pflash_write(&pf, sizeof(g_backing), 1, HYPE_PFLASH_CMD_READ_ARRAY) != 0);
    CHECK_HEX("read at/beyond size is rejected", 1, hype_pflash_read(&pf, sizeof(g_backing), 1, &value) != 0);
}

static void test_buffered_write_round_trip(void) {
    hype_pflash_t pf;
    uint32_t value;
    reset_backing();
    hype_pflash_reset(&pf, g_backing, sizeof(g_backing));

    CHECK_HEX("WRITE_TO_BUFFER command accepted", 0,
              hype_pflash_write(&pf, 0x40, 1, HYPE_PFLASH_CMD_WRITE_TO_BUFFER));
    CHECK_HEX("mode is buffer-count-pending", HYPE_PFLASH_MODE_BUFFER_COUNT_PENDING, pf.mode);

    CHECK_HEX("count byte accepted", 0, hype_pflash_write(&pf, 0x40, 1, 3));
    CHECK_HEX("mode is buffer-data-pending", HYPE_PFLASH_MODE_BUFFER_DATA_PENDING, pf.mode);

    CHECK_HEX("data byte 0 at buffer_offset+0", 0, hype_pflash_write(&pf, 0x40, 1, 0x11));
    CHECK_HEX("data byte 1 at buffer_offset+1", 0, hype_pflash_write(&pf, 0x41, 1, 0x22));
    CHECK_HEX("data byte 2 at buffer_offset+2", 0, hype_pflash_write(&pf, 0x42, 1, 0x33));
    CHECK_HEX("mode is confirm-pending once count reached", HYPE_PFLASH_MODE_BUFFER_CONFIRM_PENDING,
              pf.mode);

    CHECK_HEX("confirm accepted", 0, hype_pflash_write(&pf, 0x40, 1, HYPE_PFLASH_CMD_ERASE_CONFIRM));
    CHECK_HEX("mode returns to array after buffered write", HYPE_PFLASH_MODE_READ_ARRAY, pf.mode);

    hype_pflash_read(&pf, 0x40, 1, &value);
    CHECK_HEX("buffered byte 0 committed", 0x11, value);
    hype_pflash_read(&pf, 0x41, 1, &value);
    CHECK_HEX("buffered byte 1 committed", 0x22, value);
    hype_pflash_read(&pf, 0x42, 1, &value);
    CHECK_HEX("buffered byte 2 committed", 0x33, value);
}

static void test_buffered_write_non_sequential_offset_rejected(void) {
    hype_pflash_t pf;
    reset_backing();
    hype_pflash_reset(&pf, g_backing, sizeof(g_backing));

    hype_pflash_write(&pf, 0x40, 1, HYPE_PFLASH_CMD_WRITE_TO_BUFFER);
    hype_pflash_write(&pf, 0x40, 1, 2);
    CHECK_HEX("non-sequential data offset is rejected", 1, hype_pflash_write(&pf, 0x50, 1, 0x11) != 0);
}

static void test_buffered_write_zero_count_rejected(void) {
    hype_pflash_t pf;
    reset_backing();
    hype_pflash_reset(&pf, g_backing, sizeof(g_backing));

    hype_pflash_write(&pf, 0x40, 1, HYPE_PFLASH_CMD_WRITE_TO_BUFFER);
    CHECK_HEX("zero-length buffered write is rejected", 1, hype_pflash_write(&pf, 0x40, 1, 0) != 0);
    CHECK_HEX("mode returns to array after rejection", HYPE_PFLASH_MODE_READ_ARRAY, pf.mode);
}

int main(void) {
    test_reset_defaults();
    test_read_array_reflects_backing();
    test_read_out_of_range_rejected();
    test_read_status_and_devid_replicate_across_width();
    test_write_byte_programs_backing();
    test_write_byte_out_of_range_sets_program_error();
    test_block_erase_sets_erased_bytes_to_0xff();
    test_block_erase_wrong_confirm_offset_rejected();
    test_block_erase_wrong_confirm_byte_rejected();
    test_clear_status();
    test_unrecognized_command_rejected();
    test_write_and_read_out_of_range_offset_rejected();
    test_buffered_write_round_trip();
    test_buffered_write_non_sequential_offset_rejected();
    test_buffered_write_zero_count_rejected();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
