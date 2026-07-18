#include <stdio.h>
#include <string.h>
#include "../nvlog.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), \
                   (long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* --- pure helpers --- */

static void test_tail_offset(void) {
    CHECK_INT("shorter than cap -> offset 0", 0, hype_nvlog_tail_offset(100, 4096));
    CHECK_INT("exactly cap -> offset 0", 0, hype_nvlog_tail_offset(4096, 4096));
    CHECK_INT("longer than cap -> keeps the last cap bytes", 4, hype_nvlog_tail_offset(4100, 4096));
    CHECK_INT("empty -> offset 0", 0, hype_nvlog_tail_offset(0, 4096));
}

static void test_checksum(void) {
    CHECK_INT("checksum sums bytes", (long long)('a' + 'b' + 'c'), (long long)hype_nvlog_checksum("abc", 3));
    CHECK_INT("empty checksum is 0", 0, (long long)hype_nvlog_checksum("", 0));
    CHECK_INT("NULL checksum is 0", 0, (long long)hype_nvlog_checksum(0, 5));
}

static void test_should_write(void) {
    /* First write always happens. */
    CHECK_INT("first write forced regardless of interval", 1,
              hype_nvlog_should_write(0, 0, 1000, 0 /*have_written*/, 123, 123));
    /* Unchanged content -> skip even if the interval elapsed (protect flash). */
    CHECK_INT("unchanged content is skipped", 0,
              hype_nvlog_should_write(100000, 0, 1000, 1, 123, 123));
    /* Changed content but interval not yet elapsed -> skip. */
    CHECK_INT("changed but too soon is skipped", 0,
              hype_nvlog_should_write(500, 0, 1000, 1, 124, 123));
    /* Changed content and interval elapsed -> write. */
    CHECK_INT("changed and interval elapsed writes", 1,
              hype_nvlog_should_write(2000, 0, 1000, 1, 124, 123));
}

/* --- EFI glue against a mock EFI_RUNTIME_SERVICES --- */

static int g_set_calls;
static UINT32 g_set_attrs;
static UINTN g_set_size;
static const void *g_set_data;
static EFI_GUID g_set_guid;

static EFI_STATUS EFIAPI mock_set_variable(CHAR16 *name, EFI_GUID *guid, UINT32 attrs, UINTN size,
                                           void *data) {
    (void)name;
    g_set_calls++;
    g_set_attrs = attrs;
    g_set_size = size;
    g_set_data = data;
    g_set_guid = *guid;
    return EFI_SUCCESS;
}

static int g_get_calls;
static const char *g_get_payload;
static UINTN g_get_payload_len;
static EFI_STATUS g_get_status;

static EFI_STATUS EFIAPI mock_get_variable(CHAR16 *name, EFI_GUID *guid, UINT32 *attrs,
                                           UINTN *size, void *data) {
    (void)name;
    (void)guid;
    (void)attrs;
    g_get_calls++;
    if (g_get_status != EFI_SUCCESS) {
        return g_get_status;
    }
    if (*size < g_get_payload_len) {
        *size = g_get_payload_len;
        return EFI_BUFFER_TOO_SMALL;
    }
    memcpy(data, g_get_payload, g_get_payload_len);
    *size = g_get_payload_len;
    return EFI_SUCCESS;
}

static void test_write_sends_the_tail(void) {
    EFI_RUNTIME_SERVICES rt;
    /* A payload longer than capacity so a real tail slice is exercised. */
    static char big[HYPE_NVLOG_CAPACITY + 100];
    unsigned int i;
    EFI_GUID expect = HYPE_NVLOG_GUID;

    for (i = 0; i < sizeof(big); i++) {
        big[i] = (char)(i & 0x7f);
    }
    memset(&rt, 0, sizeof(rt));
    rt.SetVariable = mock_set_variable;
    g_set_calls = 0;

    CHECK_INT("write succeeds", (int)EFI_SUCCESS,
              (int)hype_nvlog_write(&rt, big, (unsigned int)sizeof(big)));
    CHECK_INT("SetVariable called once", 1, g_set_calls);
    CHECK_INT("size capped to capacity", (int)HYPE_NVLOG_CAPACITY, (int)g_set_size);
    CHECK_INT("attributes are NV|BS|RT", (int)HYPE_NVLOG_ATTRS, (int)g_set_attrs);
    if (g_set_data != big + 100) {
        printf("FAIL: write did not pass the last-capacity-bytes tail pointer\n");
        failures++;
    }
    if (memcmp(&g_set_guid, &expect, sizeof(EFI_GUID)) != 0) {
        printf("FAIL: write used the wrong vendor GUID\n");
        failures++;
    }
}

static void test_write_short_payload_sends_all(void) {
    EFI_RUNTIME_SERVICES rt;
    memset(&rt, 0, sizeof(rt));
    rt.SetVariable = mock_set_variable;
    g_set_calls = 0;

    hype_nvlog_write(&rt, "hello", 5);
    CHECK_INT("short payload size is its full length", 5, (int)g_set_size);
    if (g_set_data == 0) {
        printf("FAIL: short write passed a null data pointer\n");
        failures++;
    }
}

static void test_write_guards_nulls(void) {
    EFI_RUNTIME_SERVICES rt;
    memset(&rt, 0, sizeof(rt));
    g_set_calls = 0;
    CHECK_INT("NULL rt -> not found, no call", (int)EFI_NOT_FOUND,
              (int)hype_nvlog_write(0, "x", 1));
    CHECK_INT("NULL SetVariable -> not found, no call", (int)EFI_NOT_FOUND,
              (int)hype_nvlog_write(&rt, "x", 1));
    rt.SetVariable = mock_set_variable;
    CHECK_INT("NULL data -> not found, no call", (int)EFI_NOT_FOUND,
              (int)hype_nvlog_write(&rt, 0, 1));
    CHECK_INT("no SetVariable calls on guarded paths", 0, g_set_calls);
}

static void test_read_returns_payload(void) {
    EFI_RUNTIME_SERVICES rt;
    char out[HYPE_NVLOG_CAPACITY];
    unsigned int out_len = 999;

    memset(&rt, 0, sizeof(rt));
    rt.GetVariable = mock_get_variable;
    g_get_calls = 0;
    g_get_status = EFI_SUCCESS;
    g_get_payload = "recovered-tail";
    g_get_payload_len = 14;

    CHECK_INT("read succeeds", (int)EFI_SUCCESS,
              (int)hype_nvlog_read(&rt, out, sizeof(out), &out_len));
    CHECK_INT("read reports the payload length", 14, (int)out_len);
    if (memcmp(out, "recovered-tail", 14) != 0) {
        printf("FAIL: read did not copy the payload\n");
        failures++;
    }
}

static void test_read_not_found_zeroes_len(void) {
    EFI_RUNTIME_SERVICES rt;
    char out[64];
    unsigned int out_len = 999;

    memset(&rt, 0, sizeof(rt));
    rt.GetVariable = mock_get_variable;
    g_get_status = EFI_NOT_FOUND;

    CHECK_INT("read propagates NOT_FOUND", (int)EFI_NOT_FOUND,
              (int)hype_nvlog_read(&rt, out, sizeof(out), &out_len));
    CHECK_INT("out_len zeroed when nothing found", 0, (int)out_len);
}

static void test_clear_deletes(void) {
    EFI_RUNTIME_SERVICES rt;
    memset(&rt, 0, sizeof(rt));
    rt.SetVariable = mock_set_variable;
    g_set_calls = 0;

    CHECK_INT("clear succeeds", (int)EFI_SUCCESS, (int)hype_nvlog_clear(&rt));
    CHECK_INT("clear calls SetVariable once", 1, g_set_calls);
    CHECK_INT("clear writes zero bytes (deletes the variable)", 0, (int)g_set_size);
}

int main(void) {
    test_tail_offset();
    test_checksum();
    test_should_write();
    test_write_sends_the_tail();
    test_write_short_payload_sends_all();
    test_write_guards_nulls();
    test_read_returns_payload();
    test_read_not_found_zeroes_len();
    test_clear_deletes();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
