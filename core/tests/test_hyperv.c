#include <stdio.h>
#include <stdint.h>

#include "../../arch/x86_64/cpu/hyperv.h"

static int failures;

#define CHECK(desc, expected, actual)                                                        \
    do {                                                                                     \
        unsigned long long e_ = (unsigned long long)(expected);                              \
        unsigned long long a_ = (unsigned long long)(actual);                                \
        if (e_ != a_) {                                                                       \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), e_, a_);              \
            failures++;                                                                       \
        }                                                                                     \
    } while (0)

static void map_page(hype_gpa_map_t *map, uint8_t *page, uint64_t gpa) {
    hype_gpa_map_reset(map);
    CHECK("map add", 0, hype_gpa_map_add(map, gpa, (uint64_t)(uintptr_t)page,
                                          HYPE_HV_HYPERCALL_PAGE_SIZE));
}

static void test_vendor_stubs_and_page_clear(void) {
    uint8_t page[HYPE_HV_HYPERCALL_PAGE_SIZE];
    hype_gpa_map_t map;
    uint64_t out = 0;
    unsigned i;

    for (i = 0; i < sizeof(page); i++) page[i] = 0xA5u;
    map_page(&map, page, 0x2000u);
    CHECK("VMX page write", 0,
          hype_hv_hypercall_page_write(0, 0x2001u, 1u, &map,
                                       HYPE_HV_HYPERCALL_VENDOR_VMX, &out));
    CHECK("VMX readback", 0x2001u, out);
    CHECK("VMX opcode 0", 0x0Fu, page[0]);
    CHECK("VMX opcode 1", 0x01u, page[1]);
    CHECK("VMX opcode 2", 0xC1u, page[2]);
    CHECK("VMX near return", 0xC3u, page[3]);
    CHECK("VMX page remainder cleared", 0u, page[4] | page[4095]);

    for (i = 0; i < sizeof(page); i++) page[i] = 0x5Au;
    CHECK("SVM page write", 0,
          hype_hv_hypercall_page_write(0, 0x2001u, 1u, &map,
                                       HYPE_HV_HYPERCALL_VENDOR_SVM, &out));
    CHECK("SVM opcode 2", 0xD9u, page[2]);
    CHECK("SVM near return", 0xC3u, page[3]);
    CHECK("SVM page remainder cleared", 0u, page[4] | page[4095]);
}

static void test_disabled_and_missing_os_id_do_not_touch_page(void) {
    uint8_t page[HYPE_HV_HYPERCALL_PAGE_SIZE];
    hype_gpa_map_t map;
    uint64_t out = 0;

    page[0] = 0xA5u;
    map_page(&map, page, 0x4000u);
    CHECK("disabled write", 0,
          hype_hv_hypercall_page_write(0, 0x4000u, 1u, &map,
                                       HYPE_HV_HYPERCALL_VENDOR_VMX, &out));
    CHECK("disabled readback", 0x4000u, out);
    CHECK("disabled page untouched", 0xA5u, page[0]);

    CHECK("missing OS ID write", 0,
          hype_hv_hypercall_page_write(0, 0x4001u, 0u, &map,
                                       HYPE_HV_HYPERCALL_VENDOR_VMX, &out));
    CHECK("missing OS ID clears enable", 0x4000u, out);
    CHECK("missing OS ID page untouched", 0xA5u, page[0]);
}

static void test_invalid_full_page_is_rejected_without_changes(void) {
    uint8_t page[HYPE_HV_HYPERCALL_PAGE_SIZE];
    hype_gpa_map_t map;
    uint64_t out = 0xDEADBEEFu;

    page[0] = 0xA5u;
    hype_gpa_map_reset(&map);
    CHECK("short map add", 0,
          hype_gpa_map_add(&map, 0x8000u, (uint64_t)(uintptr_t)page,
                           HYPE_HV_HYPERCALL_PAGE_SIZE - 1u));
    CHECK("short page rejected", -1,
          hype_hv_hypercall_page_write(0x1000u, 0x8001u, 1u, &map,
                                       HYPE_HV_HYPERCALL_VENDOR_SVM, &out));
    CHECK("failed readback untouched", 0xDEADBEEFu, out);
    CHECK("failed page untouched", 0xA5u, page[0]);
    CHECK("NULL map rejected", -1,
          hype_hv_hypercall_page_write(0, 0x8001u, 1u, 0,
                                       HYPE_HV_HYPERCALL_VENDOR_SVM, &out));
    CHECK("NULL output rejected", -1,
          hype_hv_hypercall_page_write(0, 0x8001u, 1u, &map,
                                       HYPE_HV_HYPERCALL_VENDOR_SVM, 0));
}

static void test_locked_and_identity_clear(void) {
    uint64_t out = 0;

    CHECK("locked write accepted", 0,
          hype_hv_hypercall_page_write(0x9003u, 0xA001u, 1u, 0,
                                       HYPE_HV_HYPERCALL_VENDOR_VMX, &out));
    CHECK("locked value unchanged", 0x9003u, out);
    CHECK("identity clear disables", 0x9002u, hype_hv_hypercall_disable(0x9003u));
}

static void test_unknown_calls_return_well_formed_status(void) {
    CHECK("unknown zero", HYPE_HV_STATUS_INVALID_HYPERCALL_CODE,
          hype_hv_hypercall_dispatch(0u));
    CHECK("unknown ordinary code", HYPE_HV_STATUS_INVALID_HYPERCALL_CODE,
          hype_hv_hypercall_dispatch(0x1234u));
    CHECK("unknown code with other fields", HYPE_HV_STATUS_INVALID_HYPERCALL_CODE,
          hype_hv_hypercall_dispatch(0xFFFF000000011234ull));
}

int main(void) {
    test_vendor_stubs_and_page_clear();
    test_disabled_and_missing_os_id_do_not_touch_page();
    test_invalid_full_page_is_rejected_without_changes();
    test_locked_and_identity_clear();
    test_unknown_calls_return_well_formed_status();
    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return 1;
    }
    printf("hyperv tests passed\n");
    return 0;
}
