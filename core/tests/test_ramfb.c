#include <stdio.h>
#include "../../devices/ramfb.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_decode_known_config(void) {
    /* address=0x00000000DEADBEEF, fourcc=DRM_FORMAT_XRGB8888
     * (0x34325258), flags=0, width=800, height=600,
     * stride=800*4=3200 (0x00000C80) -- every field big-endian, same
     * byte layout QemuRamfb.c's QemuRamfbGraphicsOutputSetMode()
     * actually writes (SwapBytes64/SwapBytes32 before
     * QemuFwCfgWriteBytes()). */
    uint8_t buf[HYPE_RAMFB_CONFIG_SIZE] = {
        0x00, 0x00, 0x00, 0x00, 0xDE, 0xAD, 0xBE, 0xEF, /* address */
        0x34, 0x32, 0x52, 0x58,                         /* fourcc */
        0x00, 0x00, 0x00, 0x00,                         /* flags */
        0x00, 0x00, 0x03, 0x20,                         /* width = 800 */
        0x00, 0x00, 0x02, 0x58,                         /* height = 600 */
        0x00, 0x00, 0x0C, 0x80                          /* stride = 3200 */
    };
    hype_ramfb_config_t cfg;

    hype_ramfb_decode_config(buf, &cfg);

    CHECK_HEX("address", 0xDEADBEEFULL, cfg.address);
    CHECK_HEX("fourcc", HYPE_RAMFB_FORMAT_XRGB8888, cfg.fourcc);
    CHECK_HEX("flags", 0, cfg.flags);
    CHECK_HEX("width", 800, cfg.width);
    CHECK_HEX("height", 600, cfg.height);
    CHECK_HEX("stride", 3200, cfg.stride);
}

static void test_decode_all_ones(void) {
    uint8_t buf[HYPE_RAMFB_CONFIG_SIZE];
    hype_ramfb_config_t cfg;
    int i;

    for (i = 0; i < HYPE_RAMFB_CONFIG_SIZE; i++) {
        buf[i] = 0xFF;
    }
    hype_ramfb_decode_config(buf, &cfg);

    CHECK_HEX("address", 0xFFFFFFFFFFFFFFFFULL, cfg.address);
    CHECK_HEX("fourcc", 0xFFFFFFFFu, cfg.fourcc);
    CHECK_HEX("flags", 0xFFFFFFFFu, cfg.flags);
    CHECK_HEX("width", 0xFFFFFFFFu, cfg.width);
    CHECK_HEX("height", 0xFFFFFFFFu, cfg.height);
    CHECK_HEX("stride", 0xFFFFFFFFu, cfg.stride);
}

int main(void) {
    test_decode_known_config();
    test_decode_all_ones();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
