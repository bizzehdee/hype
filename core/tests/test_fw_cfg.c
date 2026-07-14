#include <stdio.h>
#include <string.h>
#include "../../devices/fw_cfg.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_signature_via_classic_interface(void) {
    hype_fw_cfg_t fw;
    hype_fw_cfg_reset(&fw);

    hype_fw_cfg_select(&fw, HYPE_FW_CFG_KEY_SIGNATURE);
    CHECK_HEX("Q", 'Q', hype_fw_cfg_read_byte(&fw));
    CHECK_HEX("E", 'E', hype_fw_cfg_read_byte(&fw));
    CHECK_HEX("M", 'M', hype_fw_cfg_read_byte(&fw));
    CHECK_HEX("U", 'U', hype_fw_cfg_read_byte(&fw));
}

static void test_id_advertises_dma_support(void) {
    hype_fw_cfg_t fw;
    uint32_t value;
    hype_fw_cfg_reset(&fw);

    hype_fw_cfg_select(&fw, HYPE_FW_CFG_KEY_ID);
    value = (uint32_t)hype_fw_cfg_read_byte(&fw);
    value |= (uint32_t)hype_fw_cfg_read_byte(&fw) << 8;
    value |= (uint32_t)hype_fw_cfg_read_byte(&fw) << 16;
    value |= (uint32_t)hype_fw_cfg_read_byte(&fw) << 24;

    CHECK_HEX("FW_CFG_VERSION bit set", 1, (value & 0x01u) != 0);
    CHECK_HEX("FW_CFG_VERSION_DMA bit set", 1, (value & 0x02u) != 0);
}

static void test_add_file_and_read_back(void) {
    hype_fw_cfg_t fw;
    static const uint8_t payload[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
    int key;

    hype_fw_cfg_reset(&fw);
    key = hype_fw_cfg_add_file(&fw, "etc/acpi/rsdp", payload, sizeof(payload));
    CHECK_HEX("first file gets key FW_CFG_FILE_FIRST", HYPE_FW_CFG_KEY_FILE_FIRST, key);

    hype_fw_cfg_select(&fw, (uint16_t)key);
    CHECK_HEX("byte 0", 0xDE, hype_fw_cfg_read_byte(&fw));
    CHECK_HEX("byte 1", 0xAD, hype_fw_cfg_read_byte(&fw));
    CHECK_HEX("byte 2", 0xBE, hype_fw_cfg_read_byte(&fw));
    CHECK_HEX("byte 3", 0xEF, hype_fw_cfg_read_byte(&fw));
    CHECK_HEX("byte 4", 0x01, hype_fw_cfg_read_byte(&fw));
    CHECK_HEX("read past end returns 0", 0, hype_fw_cfg_read_byte(&fw));
}

static void test_add_file_rejects_when_full(void) {
    hype_fw_cfg_t fw;
    static const uint8_t data = 0;
    int i;
    int rc = 0;

    hype_fw_cfg_reset(&fw);
    for (i = 0; i < HYPE_FW_CFG_MAX_FILES; i++) {
        char name[8];
        name[0] = 'f';
        name[1] = (char)('0' + i);
        name[2] = '\0';
        rc = hype_fw_cfg_add_file(&fw, name, &data, 1);
        if (rc < 0) {
            printf("FAIL: file %d should have been accepted\n", i);
            failures++;
        }
    }
    rc = hype_fw_cfg_add_file(&fw, "one-too-many", &data, 1);
    if (rc >= 0) {
        printf("FAIL: registry should be full\n");
        failures++;
    }
}

static void test_add_file_rejects_name_too_long(void) {
    hype_fw_cfg_t fw;
    static const uint8_t data = 0;
    char too_long[HYPE_FW_CFG_MAX_FILE_PATH + 1];
    int i;

    hype_fw_cfg_reset(&fw);
    for (i = 0; i < HYPE_FW_CFG_MAX_FILE_PATH; i++) {
        too_long[i] = 'a';
    }
    too_long[HYPE_FW_CFG_MAX_FILE_PATH] = '\0';

    if (hype_fw_cfg_add_file(&fw, too_long, &data, 1) >= 0) {
        printf("FAIL: name exactly filling the 56-byte field (no room for NUL) should be rejected\n");
        failures++;
    }
}

static void test_file_directory_content(void) {
    hype_fw_cfg_t fw;
    static const uint8_t payload[3] = {1, 2, 3};
    uint32_t count;

    hype_fw_cfg_reset(&fw);
    hype_fw_cfg_add_file(&fw, "etc/acpi/rsdp", payload, sizeof(payload));

    hype_fw_cfg_select(&fw, HYPE_FW_CFG_KEY_FILE_DIR);
    count = ((uint32_t)hype_fw_cfg_read_byte(&fw) << 24) | ((uint32_t)hype_fw_cfg_read_byte(&fw) << 16) |
            ((uint32_t)hype_fw_cfg_read_byte(&fw) << 8) | (uint32_t)hype_fw_cfg_read_byte(&fw);
    CHECK_HEX("directory count (big-endian)", 1, count);

    {
        uint32_t size = ((uint32_t)hype_fw_cfg_read_byte(&fw) << 24) |
                        ((uint32_t)hype_fw_cfg_read_byte(&fw) << 16) |
                        ((uint32_t)hype_fw_cfg_read_byte(&fw) << 8) | (uint32_t)hype_fw_cfg_read_byte(&fw);
        uint16_t select = (uint16_t)(((uint32_t)hype_fw_cfg_read_byte(&fw) << 8) |
                                     (uint32_t)hype_fw_cfg_read_byte(&fw));
        int i;
        char name[HYPE_FW_CFG_MAX_FILE_PATH];

        hype_fw_cfg_read_byte(&fw); /* reserved high byte */
        hype_fw_cfg_read_byte(&fw); /* reserved low byte */
        for (i = 0; i < HYPE_FW_CFG_MAX_FILE_PATH; i++) {
            name[i] = (char)hype_fw_cfg_read_byte(&fw);
        }

        CHECK_HEX("entry size (big-endian)", 3, size);
        CHECK_HEX("entry select key (big-endian)", HYPE_FW_CFG_KEY_FILE_FIRST, select);
        if (strcmp(name, "etc/acpi/rsdp") != 0) {
            printf("FAIL: directory entry name mismatch: \"%s\"\n", name);
            failures++;
        }
    }
}

static void test_dma_addr_high_low_roundtrip(void) {
    hype_fw_cfg_t fw;
    uint64_t addr;
    /* Native address 0x1122334455667788; OVMF writes each 32-bit half
     * byte-swapped to the wire (SwapBytes32 before IoWrite32) -- so the
     * "wire_value" this device receives for the high half
     * (0x11223344) is 0x44332211, and for the low half (0x55667788) is
     * 0x88776655. */
    hype_fw_cfg_reset(&fw);

    hype_fw_cfg_dma_addr_high(&fw, 0x44332211u);
    addr = hype_fw_cfg_dma_addr_low(&fw, 0x88776655u);

    CHECK_HEX("reconstructed native access-struct address", 0x1122334455667788ULL, addr);
}

static void test_dma_decode(void) {
    /* control = READ(0x02) | SELECT(0x08) with select_key=0x0020 in the
     * upper 16 bits -> 0x00200000 | 0x0000000A = 0x0020000A, sent
     * big-endian: 00 20 00 0A. length = 5 -> 00 00 00 05. address =
     * 0x00000000DEADBEEF -> 00 00 00 00 DE AD BE EF. */
    uint8_t raw[16] = {0x00, 0x20, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x05,
                       0x00, 0x00, 0x00, 0x00, 0xDE, 0xAD, 0xBE, 0xEF};
    hype_fw_cfg_dma_op_t op;

    hype_fw_cfg_dma_decode(raw, &op);

    CHECK_HEX("control READ bit", 1, (op.control & HYPE_FW_CFG_DMA_CTL_READ) != 0);
    CHECK_HEX("control SELECT bit", 1, (op.control & HYPE_FW_CFG_DMA_CTL_SELECT) != 0);
    CHECK_HEX("select_key", 0x0020, op.select_key);
    CHECK_HEX("length", 5, op.length);
    CHECK_HEX("address", 0xDEADBEEFu, op.address);
}

static void test_dma_execute_select_and_read(void) {
    hype_fw_cfg_t fw;
    static const uint8_t payload[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    uint8_t guest_buf[4] = {0, 0, 0, 0};
    hype_fw_cfg_dma_op_t op;
    uint32_t result;
    int key;

    hype_fw_cfg_reset(&fw);
    key = hype_fw_cfg_add_file(&fw, "etc/acpi/rsdp", payload, sizeof(payload));

    op.control = HYPE_FW_CFG_DMA_CTL_SELECT | HYPE_FW_CFG_DMA_CTL_READ;
    op.select_key = (uint16_t)key;
    op.length = 4;
    op.address = 0; /* unused by hype_fw_cfg_dma_execute() itself */

    result = hype_fw_cfg_dma_execute(&fw, &op, guest_buf);

    CHECK_HEX("result has no error bit", 0, result);
    CHECK_HEX("guest_buf[0]", 0xAA, guest_buf[0]);
    CHECK_HEX("guest_buf[1]", 0xBB, guest_buf[1]);
    CHECK_HEX("guest_buf[2]", 0xCC, guest_buf[2]);
    CHECK_HEX("guest_buf[3]", 0xDD, guest_buf[3]);
}

static void test_dma_execute_read_past_end_fills_zero(void) {
    hype_fw_cfg_t fw;
    static const uint8_t payload[2] = {0x11, 0x22};
    uint8_t guest_buf[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    hype_fw_cfg_dma_op_t op;
    int key;

    hype_fw_cfg_reset(&fw);
    key = hype_fw_cfg_add_file(&fw, "short", payload, sizeof(payload));

    op.control = HYPE_FW_CFG_DMA_CTL_SELECT | HYPE_FW_CFG_DMA_CTL_READ;
    op.select_key = (uint16_t)key;
    op.length = 4;
    op.address = 0;

    hype_fw_cfg_dma_execute(&fw, &op, guest_buf);

    CHECK_HEX("guest_buf[0]", 0x11, guest_buf[0]);
    CHECK_HEX("guest_buf[1]", 0x22, guest_buf[1]);
    CHECK_HEX("guest_buf[2] past end is 0", 0, guest_buf[2]);
    CHECK_HEX("guest_buf[3] past end is 0", 0, guest_buf[3]);
}

static void test_dma_execute_skip_advances_offset_without_data(void) {
    hype_fw_cfg_t fw;
    static const uint8_t payload[4] = {1, 2, 3, 4};
    uint8_t guest_buf[1] = {0};
    hype_fw_cfg_dma_op_t op;
    uint32_t result;
    int key;

    hype_fw_cfg_reset(&fw);
    key = hype_fw_cfg_add_file(&fw, "f", payload, sizeof(payload));

    op.control = HYPE_FW_CFG_DMA_CTL_SELECT | HYPE_FW_CFG_DMA_CTL_SKIP;
    op.select_key = (uint16_t)key;
    op.length = 2;
    op.address = 0;
    result = hype_fw_cfg_dma_execute(&fw, &op, guest_buf);
    CHECK_HEX("skip result has no error bit", 0, result);

    /* Next byte read (classic interface) should be payload[2], not
     * payload[0] -- confirms SKIP actually advanced the offset. */
    CHECK_HEX("byte after skip", 3, hype_fw_cfg_read_byte(&fw));
}

static void test_dma_execute_write_rejected(void) {
    hype_fw_cfg_t fw;
    static const uint8_t payload[2] = {1, 2};
    uint8_t guest_buf[2] = {9, 9};
    hype_fw_cfg_dma_op_t op;
    uint32_t result;
    int key;

    hype_fw_cfg_reset(&fw);
    key = hype_fw_cfg_add_file(&fw, "f", payload, sizeof(payload));

    op.control = HYPE_FW_CFG_DMA_CTL_SELECT | HYPE_FW_CFG_DMA_CTL_WRITE;
    op.select_key = (uint16_t)key;
    op.length = 2;
    op.address = 0;
    result = hype_fw_cfg_dma_execute(&fw, &op, guest_buf);

    CHECK_HEX("write is rejected with the error bit set", HYPE_FW_CFG_DMA_CTL_ERROR, result);
}

static void test_read_byte_unrecognized_key_returns_zero(void) {
    hype_fw_cfg_t fw;
    hype_fw_cfg_reset(&fw);

    hype_fw_cfg_select(&fw, 0x1234); /* never registered */
    CHECK_HEX("unrecognized key reads back as 0", 0, hype_fw_cfg_read_byte(&fw));
}

static void test_dma_execute_select_only_is_a_harmless_no_op(void) {
    hype_fw_cfg_t fw;
    static const uint8_t payload[1] = {0x42};
    uint8_t guest_buf[1] = {0};
    hype_fw_cfg_dma_op_t op;
    uint32_t result;
    int key;

    hype_fw_cfg_reset(&fw);
    key = hype_fw_cfg_add_file(&fw, "f", payload, sizeof(payload));

    op.control = HYPE_FW_CFG_DMA_CTL_SELECT; /* no READ/WRITE/SKIP */
    op.select_key = (uint16_t)key;
    op.length = 0;
    op.address = 0;
    result = hype_fw_cfg_dma_execute(&fw, &op, guest_buf);

    CHECK_HEX("select-only is not an error", 0, result);
    CHECK_HEX("select actually took effect", (uint16_t)key, fw.selected_key);
}

static void test_dma_execute_unrecognized_key_rejected(void) {
    hype_fw_cfg_t fw;
    uint8_t guest_buf[1] = {0};
    hype_fw_cfg_dma_op_t op;
    uint32_t result;

    hype_fw_cfg_reset(&fw);

    op.control = HYPE_FW_CFG_DMA_CTL_SELECT | HYPE_FW_CFG_DMA_CTL_READ;
    op.select_key = 0x9999;
    op.length = 1;
    op.address = 0;
    result = hype_fw_cfg_dma_execute(&fw, &op, guest_buf);

    CHECK_HEX("unrecognized key rejected with the error bit set", HYPE_FW_CFG_DMA_CTL_ERROR, result);
}

int main(void) {
    test_signature_via_classic_interface();
    test_id_advertises_dma_support();
    test_add_file_and_read_back();
    test_add_file_rejects_when_full();
    test_add_file_rejects_name_too_long();
    test_file_directory_content();
    test_dma_addr_high_low_roundtrip();
    test_dma_decode();
    test_dma_execute_select_and_read();
    test_dma_execute_read_past_end_fills_zero();
    test_dma_execute_skip_advances_offset_without_data();
    test_dma_execute_write_rejected();
    test_read_byte_unrecognized_key_returns_zero();
    test_dma_execute_select_only_is_a_harmless_no_op();
    test_dma_execute_unrecognized_key_rejected();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
