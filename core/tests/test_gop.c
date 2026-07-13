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

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
