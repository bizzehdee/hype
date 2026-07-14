#include <stdio.h>
#include <string.h>
#include "../mp.h"

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
static EFI_MP_SERVICES_PROTOCOL g_fake_mp;

static EFI_STATUS EFIAPI mock_locate_protocol(EFI_GUID *protocol, void *registration, void **interface) {
    (void)registration;
    g_locate_calls++;
    g_seen_guid = *protocol;
    if (g_locate_status != EFI_SUCCESS) {
        return g_locate_status;
    }
    *interface = &g_fake_mp;
    return EFI_SUCCESS;
}

static void test_locate_success(void) {
    EFI_BOOT_SERVICES bs;
    EFI_MP_SERVICES_PROTOCOL *mp = 0;
    EFI_STATUS status;
    EFI_GUID expected_guid = EFI_MP_SERVICES_PROTOCOL_GUID;

    memset(&bs, 0, sizeof(bs));
    bs.LocateProtocol = mock_locate_protocol;
    g_locate_calls = 0;
    g_locate_status = EFI_SUCCESS;

    status = hype_mp_locate(&bs, &mp);

    CHECK_INT("locate succeeds", (int)EFI_SUCCESS, (int)status);
    CHECK_INT("locate calls LocateProtocol once", 1, g_locate_calls);
    if (mp != &g_fake_mp) {
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
    EFI_MP_SERVICES_PROTOCOL *mp = (EFI_MP_SERVICES_PROTOCOL *)0x1;
    EFI_STATUS status;

    memset(&bs, 0, sizeof(bs));
    bs.LocateProtocol = mock_locate_protocol;
    g_locate_calls = 0;
    g_locate_status = EFI_NOT_FOUND;

    status = hype_mp_locate(&bs, &mp);

    CHECK_INT("locate propagates failure", (int)EFI_NOT_FOUND, (int)status);
}

/* Fake GetNumberOfProcessors/GetProcessorInfo for
 * hype_mp_pick_target_ap(), configured per test via these globals. */
static UINTN g_fake_count;
static UINT32 g_fake_status_flags[8];
static int g_get_info_calls;

static EFI_STATUS EFIAPI fake_get_number_of_processors(EFI_MP_SERVICES_PROTOCOL *this, UINTN *count,
                                                        UINTN *enabled_count) {
    (void)this;
    *count = g_fake_count;
    *enabled_count = g_fake_count;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fake_get_processor_info(EFI_MP_SERVICES_PROTOCOL *this, UINTN processor_number,
                                                  EFI_PROCESSOR_INFORMATION *info) {
    (void)this;
    g_get_info_calls++;
    info->ProcessorId = processor_number;
    info->StatusFlag = g_fake_status_flags[processor_number];
    info->Location.Package = 0;
    info->Location.Core = (UINT32)processor_number;
    info->Location.Thread = 0;
    return EFI_SUCCESS;
}

static void test_pick_target_ap_skips_bsp(void) {
    EFI_MP_SERVICES_PROTOCOL mp;
    UINTN target = 999;
    EFI_STATUS status;

    memset(&mp, 0, sizeof(mp));
    mp.GetNumberOfProcessors = fake_get_number_of_processors;
    mp.GetProcessorInfo = fake_get_processor_info;

    g_fake_count = 2;
    g_fake_status_flags[0] = HYPE_MP_PROCESSOR_AS_BSP_BIT | HYPE_MP_PROCESSOR_ENABLED_BIT;
    g_fake_status_flags[1] = HYPE_MP_PROCESSOR_ENABLED_BIT;
    g_get_info_calls = 0;

    status = hype_mp_pick_target_ap(&mp, &target);

    CHECK_INT("pick succeeds", (int)EFI_SUCCESS, (int)status);
    CHECK_INT("picks processor 1, not the BSP (0)", 1, (long long)target);
}

static void test_pick_target_ap_skips_disabled(void) {
    EFI_MP_SERVICES_PROTOCOL mp;
    UINTN target = 999;
    EFI_STATUS status;

    memset(&mp, 0, sizeof(mp));
    mp.GetNumberOfProcessors = fake_get_number_of_processors;
    mp.GetProcessorInfo = fake_get_processor_info;

    g_fake_count = 3;
    g_fake_status_flags[0] = HYPE_MP_PROCESSOR_AS_BSP_BIT | HYPE_MP_PROCESSOR_ENABLED_BIT;
    g_fake_status_flags[1] = 0; /* disabled AP */
    g_fake_status_flags[2] = HYPE_MP_PROCESSOR_ENABLED_BIT;

    status = hype_mp_pick_target_ap(&mp, &target);

    CHECK_INT("pick succeeds", (int)EFI_SUCCESS, (int)status);
    CHECK_INT("skips disabled processor 1, picks enabled processor 2", 2, (long long)target);
}

static EFI_STATUS EFIAPI fake_get_number_of_processors_fails(EFI_MP_SERVICES_PROTOCOL *this, UINTN *count,
                                                              UINTN *enabled_count) {
    (void)this;
    (void)count;
    (void)enabled_count;
    return EFI_NOT_FOUND;
}

static void test_pick_target_ap_propagates_get_number_failure(void) {
    EFI_MP_SERVICES_PROTOCOL mp;
    UINTN target = 999;
    EFI_STATUS status;

    memset(&mp, 0, sizeof(mp));
    mp.GetNumberOfProcessors = fake_get_number_of_processors_fails;
    mp.GetProcessorInfo = fake_get_processor_info;

    status = hype_mp_pick_target_ap(&mp, &target);

    CHECK_INT("propagates GetNumberOfProcessors failure", (int)EFI_NOT_FOUND, (int)status);
}

static EFI_STATUS EFIAPI fake_get_processor_info_fails(EFI_MP_SERVICES_PROTOCOL *this,
                                                        UINTN processor_number,
                                                        EFI_PROCESSOR_INFORMATION *info) {
    (void)this;
    (void)processor_number;
    (void)info;
    return EFI_NOT_FOUND;
}

static void test_pick_target_ap_propagates_get_info_failure(void) {
    EFI_MP_SERVICES_PROTOCOL mp;
    UINTN target = 999;
    EFI_STATUS status;

    memset(&mp, 0, sizeof(mp));
    mp.GetNumberOfProcessors = fake_get_number_of_processors;
    mp.GetProcessorInfo = fake_get_processor_info_fails;

    g_fake_count = 1;

    status = hype_mp_pick_target_ap(&mp, &target);

    CHECK_INT("propagates GetProcessorInfo failure", (int)EFI_NOT_FOUND, (int)status);
}

static void test_pick_target_ap_none_available(void) {
    EFI_MP_SERVICES_PROTOCOL mp;
    UINTN target = 999;
    EFI_STATUS status;

    memset(&mp, 0, sizeof(mp));
    mp.GetNumberOfProcessors = fake_get_number_of_processors;
    mp.GetProcessorInfo = fake_get_processor_info;

    g_fake_count = 1;
    g_fake_status_flags[0] = HYPE_MP_PROCESSOR_AS_BSP_BIT | HYPE_MP_PROCESSOR_ENABLED_BIT;

    status = hype_mp_pick_target_ap(&mp, &target);

    CHECK_INT("returns NOT_FOUND with only a BSP present", (int)EFI_NOT_FOUND, (int)status);
}

int main(void) {
    test_locate_success();
    test_locate_failure();
    test_pick_target_ap_skips_bsp();
    test_pick_target_ap_skips_disabled();
    test_pick_target_ap_propagates_get_number_failure();
    test_pick_target_ap_propagates_get_info_failure();
    test_pick_target_ap_none_available();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
