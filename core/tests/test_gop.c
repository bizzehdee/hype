#include <stdio.h>
#include <string.h>
#include "../gop.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), (long long)(actual)); \
            failures++; \
        } \
    } while (0)

static int g_locate_calls;
static EFI_STATUS g_locate_status;
static EFI_GUID g_seen_guid;
static EFI_GRAPHICS_OUTPUT_PROTOCOL g_fake_gop;

static EFI_STATUS EFIAPI mock_locate_protocol(EFI_GUID *protocol, void *registration, void **interface) {
    (void)registration;
    g_locate_calls++;
    g_seen_guid = *protocol;
    if (g_locate_status != EFI_SUCCESS) {
        return g_locate_status;
    }
    *interface = &g_fake_gop;
    return EFI_SUCCESS;
}

static void test_locate_success(void) {
    EFI_BOOT_SERVICES bs;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    EFI_STATUS status;
    EFI_GUID expected_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

    memset(&bs, 0, sizeof(bs));
    bs.LocateProtocol = mock_locate_protocol;
    g_locate_calls = 0;
    g_locate_status = EFI_SUCCESS;

    status = hype_gop_locate(&bs, &gop);

    CHECK_INT("locate succeeds", (int)EFI_SUCCESS, (int)status);
    CHECK_INT("locate calls LocateProtocol once", 1, g_locate_calls);
    if (gop != &g_fake_gop) {
        printf("FAIL: locate did not return the interface pointer LocateProtocol supplied\n");
        failures++;
    }
    if (memcmp(&g_seen_guid, &expected_guid, sizeof(EFI_GUID)) != 0) {
        printf("FAIL: locate passed the wrong GUID to LocateProtocol\n");
        failures++;
    }
}

static int g_blt_calls;
static EFI_GRAPHICS_OUTPUT_BLT_PIXEL *g_blt_buffer;
static EFI_GRAPHICS_OUTPUT_BLT_OPERATION g_blt_operation;
static UINTN g_blt_source_x, g_blt_source_y, g_blt_dest_x, g_blt_dest_y, g_blt_width, g_blt_height,
    g_blt_delta;

static EFI_STATUS EFIAPI mock_blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *this_gop, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *buf,
                                   EFI_GRAPHICS_OUTPUT_BLT_OPERATION op, UINTN src_x, UINTN src_y,
                                   UINTN dst_x, UINTN dst_y, UINTN width, UINTN height, UINTN delta) {
    (void)this_gop;
    g_blt_calls++;
    g_blt_buffer = buf;
    g_blt_operation = op;
    g_blt_source_x = src_x;
    g_blt_source_y = src_y;
    g_blt_dest_x = dst_x;
    g_blt_dest_y = dst_y;
    g_blt_width = width;
    g_blt_height = height;
    g_blt_delta = delta;
    return EFI_SUCCESS;
}

static void test_flush_uses_blt_when_gop_is_available(void) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL fake_gop;
    unsigned int shadow[4 * 3]; /* stride=4, height=3 */
    hype_gop_console_t con;

    memset(&fake_gop, 0, sizeof(fake_gop));
    fake_gop.Blt = mock_blt;
    con.fb = shadow;
    con.width = 2;
    con.height = 3;
    con.stride = 4;

    g_blt_calls = 0;
    hype_gop_flush(&fake_gop, &con, (void *)0x1234);

    CHECK_INT("Blt is called exactly once", 1, g_blt_calls);
    if (g_blt_buffer != (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)shadow) {
        printf("FAIL: Blt was not passed the console's own shadow buffer\n");
        failures++;
    }
    CHECK_INT("operation is EfiBltBufferToVideo", (int)EfiBltBufferToVideo, (int)g_blt_operation);
    CHECK_INT("source x/y are 0", 0, g_blt_source_x + g_blt_source_y);
    CHECK_INT("destination x/y are 0", 0, g_blt_dest_x + g_blt_dest_y);
    CHECK_INT("width matches the console's own width", 2, g_blt_width);
    CHECK_INT("height matches the console's own height", 3, g_blt_height);
    CHECK_INT("delta is stride in bytes, not pixels", 4 * (int)sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL),
              g_blt_delta);
}

static void test_flush_falls_back_to_memcpy_when_gop_is_null(void) {
    unsigned int shadow[4 * 3];
    unsigned int real_fb[4 * 3];
    hype_gop_console_t con;
    unsigned int x, y;

    for (y = 0; y < 3; y++) {
        for (x = 0; x < 4; x++) {
            shadow[y * 4 + x] = 0xA0000000u + y * 4 + x;
            real_fb[y * 4 + x] = 0xFFFFFFFFu;
        }
    }
    con.fb = shadow;
    con.width = 2;
    con.height = 3;
    con.stride = 4;

    g_blt_calls = 0;
    hype_gop_flush(0, &con, real_fb);

    CHECK_INT("Blt is never called when gop is NULL", 0, g_blt_calls);
    for (y = 0; y < 3; y++) {
        for (x = 0; x < 2; x++) { /* only within width -- padding columns untouched */
            CHECK_INT("in-bounds pixels are copied from the shadow buffer", shadow[y * 4 + x],
                      real_fb[y * 4 + x]);
        }
        CHECK_INT("padding column (>= width) is left untouched", 0xFFFFFFFFu, real_fb[y * 4 + 2]);
    }
}

static void test_flush_is_a_safe_no_op_with_no_gop_and_no_real_fb(void) {
    unsigned int shadow[4 * 3];
    hype_gop_console_t con;

    con.fb = shadow;
    con.width = 2;
    con.height = 3;
    con.stride = 4;

    g_blt_calls = 0;
    hype_gop_flush(0, &con, 0); /* must not crash */
    CHECK_INT("Blt is never called", 0, g_blt_calls);
}

static void test_locate_failure(void) {
    EFI_BOOT_SERVICES bs;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = (EFI_GRAPHICS_OUTPUT_PROTOCOL *)0x1;
    EFI_STATUS status;

    memset(&bs, 0, sizeof(bs));
    bs.LocateProtocol = mock_locate_protocol;
    g_locate_calls = 0;
    g_locate_status = EFI_ERR_BIT | 14; /* EFI_NOT_FOUND, in spirit */

    status = hype_gop_locate(&bs, &gop);

    CHECK_INT("locate propagates failure", (int)(EFI_ERR_BIT | 14), (int)status);
}

int main(void) {
    test_locate_success();
    test_locate_failure();
    test_flush_uses_blt_when_gop_is_available();
    test_flush_falls_back_to_memcpy_when_gop_is_null();
    test_flush_is_a_safe_no_op_with_no_gop_and_no_real_fb();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
