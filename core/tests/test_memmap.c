#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "../memmap.h"

static int failures = 0;

#define CHECK_STR(desc, expected, actual) \
    do { \
        if (strcmp((expected), (actual)) != 0) { \
            printf("FAIL: %s: expected \"%s\", got \"%s\"\n", (desc), (expected), (actual)); \
            failures++; \
        } \
    } while (0)

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((expected) != (actual)) { \
            printf("FAIL: %s: expected %d, got %d\n", (desc), (int)(expected), (int)(actual)); \
            failures++; \
        } \
    } while (0)

/* ---- hype_memmap_type_name ---- */

static void test_type_name(void) {
    static const struct {
        UINT32 type;
        const char *name;
    } cases[] = {
        {EfiReservedMemoryType, "Reserved"},
        {EfiLoaderCode, "LoaderCode"},
        {EfiLoaderData, "LoaderData"},
        {EfiBootServicesCode, "BootServicesCode"},
        {EfiBootServicesData, "BootServicesData"},
        {EfiRuntimeServicesCode, "RuntimeServicesCode"},
        {EfiRuntimeServicesData, "RuntimeServicesData"},
        {EfiConventionalMemory, "Conventional"},
        {EfiUnusableMemory, "Unusable"},
        {EfiACPIReclaimMemory, "ACPIReclaim"},
        {EfiACPIMemoryNVS, "ACPIMemoryNVS"},
        {EfiMemoryMappedIO, "MMIO"},
        {EfiMemoryMappedIOPortSpace, "MMIOPortSpace"},
        {EfiPalCode, "PalCode"},
        {EfiPersistentMemory, "Persistent"},
        {EfiUnacceptedMemoryType, "Unaccepted"},
    };
    unsigned long long i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CHECK_STR("memory type name", cases[i].name, hype_memmap_type_name(cases[i].type));
    }
    CHECK_STR("unknown type name", "Unknown", hype_memmap_type_name(9999));
}

/* ---- hype_memmap_get: mocked EFI_BOOT_SERVICES ---- */

static UINTN g_probe_map_size;
static UINTN g_desc_size;
static EFI_STATUS g_probe_status;
static EFI_STATUS g_allocate_status;
static EFI_STATUS g_second_status;
static int g_get_calls;
static int g_allocate_calls;
static int g_free_calls;
static UINT8 g_pool_buf[4096];
static EFI_MEMORY_DESCRIPTOR g_canned_map[2];

static void reset_mocks(void) {
    g_get_calls = 0;
    g_allocate_calls = 0;
    g_free_calls = 0;
}

static EFI_STATUS EFIAPI mock_get_memory_map(UINTN *map_size, EFI_MEMORY_DESCRIPTOR *map,
                                              UINTN *map_key, UINTN *desc_size,
                                              UINT32 *desc_version) {
    g_get_calls++;
    *desc_size = g_desc_size;
    *desc_version = 1;
    *map_key = 42;

    if (g_get_calls == 1) {
        *map_size = g_probe_map_size;
        return g_probe_status;
    }

    if (g_second_status == EFI_SUCCESS && map != 0) {
        memcpy(map, g_canned_map, sizeof(g_canned_map));
        *map_size = sizeof(g_canned_map);
    }
    return g_second_status;
}

static EFI_STATUS EFIAPI mock_allocate_pool(UINT32 pool_type, UINTN size, void **buffer) {
    (void)pool_type;
    g_allocate_calls++;
    if (g_allocate_status != EFI_SUCCESS) {
        return g_allocate_status;
    }
    if (size > sizeof(g_pool_buf)) {
        return EFI_BUFFER_TOO_SMALL;
    }
    *buffer = g_pool_buf;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI mock_free_pool(void *buffer) {
    (void)buffer;
    g_free_calls++;
    return EFI_SUCCESS;
}

static void make_boot_services(EFI_BOOT_SERVICES *bs) {
    memset(bs, 0, sizeof(*bs));
    bs->GetMemoryMap = mock_get_memory_map;
    bs->AllocatePool = mock_allocate_pool;
    bs->FreePool = mock_free_pool;
}

static void test_get_success(void) {
    EFI_BOOT_SERVICES bs;
    EFI_MEMORY_DESCRIPTOR *map = 0;
    UINTN map_size = 0, desc_size = 0, map_key = 0;
    EFI_STATUS status;

    make_boot_services(&bs);
    reset_mocks();
    g_probe_map_size = sizeof(g_canned_map);
    g_desc_size = sizeof(EFI_MEMORY_DESCRIPTOR);
    g_probe_status = EFI_BUFFER_TOO_SMALL;
    g_allocate_status = EFI_SUCCESS;
    g_second_status = EFI_SUCCESS;
    g_canned_map[0].Type = EfiConventionalMemory;
    g_canned_map[0].PhysicalStart = 0x100000;
    g_canned_map[0].NumberOfPages = 16;
    g_canned_map[1].Type = EfiLoaderCode;
    g_canned_map[1].PhysicalStart = 0x200000;
    g_canned_map[1].NumberOfPages = 4;

    status = hype_memmap_get(&bs, &map, &map_size, &desc_size, &map_key);

    CHECK_INT("get() success status", (int)EFI_SUCCESS, (int)status);
    CHECK_INT("get() calls GetMemoryMap twice", 2, g_get_calls);
    CHECK_INT("get() calls AllocatePool once", 1, g_allocate_calls);
    CHECK_INT("get() does not free on success", 0, g_free_calls);
    CHECK_INT("get() reports map_key from second call", 42, (int)map_key);
    if (map != 0) {
        CHECK_INT("first entry type round-trips", (int)EfiConventionalMemory, (int)map[0].Type);
    } else {
        printf("FAIL: get() success returned NULL map\n");
        failures++;
    }
}

static void test_get_probe_error(void) {
    EFI_BOOT_SERVICES bs;
    EFI_MEMORY_DESCRIPTOR *map = (EFI_MEMORY_DESCRIPTOR *)0x1;
    UINTN map_size = 0, desc_size = 0, map_key = 0;
    EFI_STATUS status;

    make_boot_services(&bs);
    reset_mocks();
    g_probe_status = EFI_ERR_BIT | 2; /* arbitrary non-BUFFER_TOO_SMALL error */

    status = hype_memmap_get(&bs, &map, &map_size, &desc_size, &map_key);

    CHECK_INT("get() propagates unexpected probe status", (int)(EFI_ERR_BIT | 2), (int)status);
    CHECK_INT("get() does not allocate on probe error", 0, g_allocate_calls);
}

static void test_get_allocate_error(void) {
    EFI_BOOT_SERVICES bs;
    EFI_MEMORY_DESCRIPTOR *map = 0;
    UINTN map_size = 0, desc_size = 0, map_key = 0;
    EFI_STATUS status;

    make_boot_services(&bs);
    reset_mocks();
    g_probe_map_size = sizeof(g_canned_map);
    g_desc_size = sizeof(EFI_MEMORY_DESCRIPTOR);
    g_probe_status = EFI_BUFFER_TOO_SMALL;
    g_allocate_status = EFI_ERR_BIT | 9; /* out of resources, say */

    status = hype_memmap_get(&bs, &map, &map_size, &desc_size, &map_key);

    CHECK_INT("get() propagates allocate failure", (int)(EFI_ERR_BIT | 9), (int)status);
    CHECK_INT("get() does not call FreePool when AllocatePool itself failed", 0, g_free_calls);
    CHECK_INT("get() only calls GetMemoryMap once when allocate fails", 1, g_get_calls);
}

static void test_get_second_call_error(void) {
    EFI_BOOT_SERVICES bs;
    EFI_MEMORY_DESCRIPTOR *map = 0;
    UINTN map_size = 0, desc_size = 0, map_key = 0;
    EFI_STATUS status;

    make_boot_services(&bs);
    reset_mocks();
    g_probe_map_size = sizeof(g_canned_map);
    g_desc_size = sizeof(EFI_MEMORY_DESCRIPTOR);
    g_probe_status = EFI_BUFFER_TOO_SMALL;
    g_allocate_status = EFI_SUCCESS;
    g_second_status = EFI_ERR_BIT | 3; /* map changed between calls, say */

    status = hype_memmap_get(&bs, &map, &map_size, &desc_size, &map_key);

    CHECK_INT("get() propagates second-call failure", (int)(EFI_ERR_BIT | 3), (int)status);
    CHECK_INT("get() frees pool when second call fails", 1, g_free_calls);
}

/* ---- hype_memmap_dump: captured from the injected emit sink ---- */

/*
 * #296: the dump used to go to ConOut, and this harness mocked ConOut->OutputString to read it
 * back. ConOut reaches the firmware console and no durable sink -- not the logbuf, so not
 * \hype-log.txt or the persistent USB logs -- which on a serial-less machine made the dump
 * recoverable never. The test could not have caught that, because it asserted on the one channel
 * that was wrong.
 *
 * The sink is now a parameter. boot/main.c passes hype_debug_print, which does real port I/O and
 * would fault in a host test; this stub records what was emitted instead.
 */
static char g_emitted[4096];
static int g_emit_calls;

static void capture_emit(const char *fmt, ...) {
    va_list ap;
    unsigned long long used = 0;
    while (used < sizeof(g_emitted) - 1u && g_emitted[used] != 0) {
        used++;
    }
    va_start(ap, fmt);
    vsnprintf(g_emitted + used, sizeof(g_emitted) - used, fmt, ap);
    va_end(ap);
    g_emit_calls++;
}

static void capture_reset(void) {
    memset(g_emitted, 0, sizeof(g_emitted));
    g_emit_calls = 0;
}

static void test_dump(void) {
    EFI_MEMORY_DESCRIPTOR map[2];

    capture_reset();

    map[0].Type = EfiConventionalMemory;
    map[0].PhysicalStart = 0x100000;
    map[0].NumberOfPages = 16;
    map[1].Type = EfiACPIReclaimMemory;
    map[1].PhysicalStart = 0x200000;
    map[1].NumberOfPages = 1;

    hype_memmap_dump(capture_emit, map, sizeof(map), sizeof(EFI_MEMORY_DESCRIPTOR));

    CHECK_INT("dump() emits header plus one line per entry", 3, g_emit_calls);
    if (strstr(g_emitted, "2 entries") == 0) {
        printf("FAIL: dump() header missing entry count\n");
        failures++;
    }
    if (strstr(g_emitted, "Conventional") == 0) {
        printf("FAIL: dump() missing Conventional entry\n");
        failures++;
    }
    if (strstr(g_emitted, "ACPIReclaim") == 0) {
        printf("FAIL: dump() missing ACPIReclaim entry\n");
        failures++;
    }
    if (strstr(g_emitted, "0x100000") == 0) {
        printf("FAIL: dump() missing first entry's physical address\n");
        failures++;
    }
}

static void test_dump_empty(void) {
    capture_reset();
    hype_memmap_dump(capture_emit, 0, 0, sizeof(EFI_MEMORY_DESCRIPTOR));

    CHECK_INT("dump() with zero entries emits only the header", 1, g_emit_calls);
    if (strstr(g_emitted, "0 entries") == 0) {
        printf("FAIL: dump() empty header missing zero count\n");
        failures++;
    }
}

/* #296: a caller with no sink must be a no-op, not a null call through a function pointer. */
static void test_dump_without_a_sink_is_silent(void) {
    EFI_MEMORY_DESCRIPTOR map[1];
    map[0].Type = EfiConventionalMemory;
    map[0].PhysicalStart = 0x100000;
    map[0].NumberOfPages = 1;

    capture_reset();
    hype_memmap_dump(0, map, sizeof(map), sizeof(EFI_MEMORY_DESCRIPTOR));
    CHECK_INT("dump() with no emit sink writes nothing", 0, g_emit_calls);
}

/* ---- hype_exit_boot_services: mocked EFI_BOOT_SERVICES ---- */

static int g_ebs_get_calls;
static int g_ebs_free_calls;
static int g_ebs_exit_calls;
static int g_ebs_fail_count;
static UINTN g_ebs_desc_size;
static UINT8 g_ebs_pool_buf[4096];
static int g_ebs_force_probe_error;
static EFI_STATUS g_ebs_probe_error_value;

static EFI_STATUS EFIAPI mock_ebs_get_memory_map(UINTN *map_size, EFI_MEMORY_DESCRIPTOR *map,
                                                  UINTN *map_key, UINTN *desc_size,
                                                  UINT32 *desc_version) {
    g_ebs_get_calls++;
    *desc_size = g_ebs_desc_size;
    *desc_version = 1;
    *map_key = (UINTN)g_ebs_get_calls; /* distinct per attempt, mimics a changing map */

    if (map == 0) {
        if (g_ebs_force_probe_error) {
            return g_ebs_probe_error_value;
        }
        *map_size = g_ebs_desc_size * 2;
        return EFI_BUFFER_TOO_SMALL;
    }
    *map_size = g_ebs_desc_size * 2;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI mock_ebs_allocate_pool(UINT32 pool_type, UINTN size, void **buffer) {
    (void)pool_type;
    if (size > sizeof(g_ebs_pool_buf)) {
        return EFI_BUFFER_TOO_SMALL;
    }
    *buffer = g_ebs_pool_buf;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI mock_ebs_free_pool(void *buffer) {
    (void)buffer;
    g_ebs_free_calls++;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI mock_ebs_exit_boot_services(EFI_HANDLE image_handle, UINTN map_key) {
    (void)image_handle;
    (void)map_key;
    g_ebs_exit_calls++;
    if (g_ebs_exit_calls <= g_ebs_fail_count) {
        return EFI_ERR_BIT | 2; /* stale map key, in spirit */
    }
    return EFI_SUCCESS;
}

static void make_ebs_boot_services(EFI_BOOT_SERVICES *bs) {
    memset(bs, 0, sizeof(*bs));
    bs->GetMemoryMap = mock_ebs_get_memory_map;
    bs->AllocatePool = mock_ebs_allocate_pool;
    bs->FreePool = mock_ebs_free_pool;
    bs->ExitBootServices = mock_ebs_exit_boot_services;
}

static void reset_ebs_mocks(void) {
    g_ebs_get_calls = 0;
    g_ebs_free_calls = 0;
    g_ebs_exit_calls = 0;
    g_ebs_fail_count = 0;
    g_ebs_desc_size = sizeof(EFI_MEMORY_DESCRIPTOR);
    g_ebs_force_probe_error = 0;
}

static void test_exit_boot_services_success_first_try(void) {
    EFI_BOOT_SERVICES bs;
    EFI_STATUS status;

    make_ebs_boot_services(&bs);
    reset_ebs_mocks();

    status = hype_exit_boot_services((EFI_HANDLE)0x1234, &bs);

    CHECK_INT("exit_boot_services succeeds first try", (int)EFI_SUCCESS, (int)status);
    CHECK_INT("exit_boot_services calls ExitBootServices once", 1, g_ebs_exit_calls);
    CHECK_INT("exit_boot_services does not free the map on success", 0, g_ebs_free_calls);
}

static void test_exit_boot_services_retries_on_stale_map_key(void) {
    EFI_BOOT_SERVICES bs;
    EFI_STATUS status;

    make_ebs_boot_services(&bs);
    reset_ebs_mocks();
    g_ebs_fail_count = 2; /* fail twice, succeed on the 3rd attempt */

    status = hype_exit_boot_services((EFI_HANDLE)0x1234, &bs);

    CHECK_INT("exit_boot_services eventually succeeds", (int)EFI_SUCCESS, (int)status);
    CHECK_INT("exit_boot_services retried exactly 3 times", 3, g_ebs_exit_calls);
    CHECK_INT("exit_boot_services frees the map on each failed attempt", 2, g_ebs_free_calls);
}

static void test_exit_boot_services_propagates_memmap_error(void) {
    EFI_BOOT_SERVICES bs;
    EFI_STATUS status;

    make_ebs_boot_services(&bs);
    reset_ebs_mocks();
    g_ebs_force_probe_error = 1;
    g_ebs_probe_error_value = EFI_ERR_BIT | 9;

    status = hype_exit_boot_services((EFI_HANDLE)0x1234, &bs);

    CHECK_INT("exit_boot_services propagates a real GetMemoryMap error",
              (int)(EFI_ERR_BIT | 9), (int)status);
    CHECK_INT("exit_boot_services never calls ExitBootServices if memmap fetch fails",
              0, g_ebs_exit_calls);
}

static void test_usable_bytes(void) {
    EFI_MEMORY_DESCRIPTOR map[5];
    UINT64 result;

    map[0].Type = EfiConventionalMemory;
    map[0].NumberOfPages = 10;
    map[1].Type = EfiBootServicesCode;
    map[1].NumberOfPages = 5;
    map[2].Type = EfiBootServicesData;
    map[2].NumberOfPages = 3;
    map[3].Type = EfiLoaderData; /* excluded -- our own image */
    map[3].NumberOfPages = 1000;
    map[4].Type = EfiReservedMemoryType; /* excluded */
    map[4].NumberOfPages = 1000;

    result = hype_memmap_usable_bytes(map, sizeof(map), sizeof(EFI_MEMORY_DESCRIPTOR));

    CHECK_INT("usable_bytes sums only Conventional+BootServicesCode/Data",
              (18ULL * 4096ULL), (long long)result);
}

static void test_usable_bytes_empty(void) {
    UINT64 result = hype_memmap_usable_bytes(0, 0, sizeof(EFI_MEMORY_DESCRIPTOR));
    CHECK_INT("usable_bytes of an empty map is zero", 0, (long long)result);
}


/*
 * #290: largest SINGLE contiguous Conventional block. This is the number that
 * explains an EFI_OUT_OF_RESOURCES on a host with gigabytes free -- AllocateAnyPages
 * needs one contiguous run, and a request can be refused while the TOTAL free far
 * exceeds it.
 */
static void test_largest_conventional_block(void) {
    EFI_MEMORY_DESCRIPTOR m[5];
    UINT64 got;

    memset(m, 0, sizeof(m));
    /* Two Conventional runs, the smaller one FIRST so a buggy "take the first"
     * implementation is caught, plus non-Conventional types that must be ignored. */
    m[0].Type = EfiConventionalMemory; m[0].PhysicalStart = 0x100000;   m[0].NumberOfPages = 100;
    m[1].Type = EfiBootServicesData;   m[1].PhysicalStart = 0x1000000;  m[1].NumberOfPages = 9999;
    m[2].Type = EfiConventionalMemory; m[2].PhysicalStart = 0x2000000;  m[2].NumberOfPages = 500;
    m[3].Type = EfiACPIMemoryNVS;      m[3].PhysicalStart = 0x3000000;  m[3].NumberOfPages = 8888;
    m[4].Type = EfiConventionalMemory; m[4].PhysicalStart = 0x4000000;  m[4].NumberOfPages = 250;

    got = hype_memmap_largest_conventional_bytes(m, sizeof(m), sizeof(m[0]));
    CHECK_INT("largest is the 500-page run", 500ULL * 4096ULL, got);

    /*
     * BootServicesCode/Data must NOT count, even though hype_memmap_usable_bytes()
     * includes them in its total: they only become free AFTER ExitBootServices, and
     * every allocation failure this diagnostic explains happens BEFORE it. m[1] is
     * 9999 pages -- far larger than any Conventional run here -- so counting it
     * would be obvious.
     */
    if (got >= 9999ULL * 4096ULL) {
        printf("FAIL: BootServicesData counted as available pre-EBS\n");
        failures++;
    }

    /* An all-non-Conventional map has no usable contiguous run at all. */
    memset(m, 0, sizeof(m));
    m[0].Type = EfiACPIMemoryNVS; m[0].NumberOfPages = 1000;
    CHECK_INT("no Conventional -> 0", 0ULL,
              hype_memmap_largest_conventional_bytes(m, sizeof(m[0]), sizeof(m[0])));

    /* Degenerate inputs must not divide by zero or deref. */
    CHECK_INT("NULL map -> 0", 0ULL, hype_memmap_largest_conventional_bytes(0, 64, 8));
    CHECK_INT("desc_size 0 -> 0", 0ULL, hype_memmap_largest_conventional_bytes(m, 64, 0));
    CHECK_INT("empty map -> 0", 0ULL, hype_memmap_largest_conventional_bytes(m, 0, sizeof(m[0])));
}

int main(void) {
    test_largest_conventional_block();
    test_type_name();
    test_get_success();
    test_get_probe_error();
    test_get_allocate_error();
    test_usable_bytes();
    test_usable_bytes_empty();
    test_get_second_call_error();
    test_dump();
    test_dump_empty();
    test_dump_without_a_sink_is_silent();
    test_exit_boot_services_success_first_try();
    test_exit_boot_services_retries_on_stale_map_key();
    test_exit_boot_services_propagates_memmap_error();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
