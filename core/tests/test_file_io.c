#include <stdio.h>
#include <string.h>
#include "../file_io.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), (long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* -------- fakes -------- */

static EFI_FILE_PROTOCOL g_fake_root;
static EFI_FILE_PROTOCOL g_fake_file;

static int g_open_calls;
static EFI_STATUS g_open_status;
static int g_close_calls;

static EFI_STATUS EFIAPI fake_open(EFI_FILE_PROTOCOL *this, EFI_FILE_PROTOCOL **new_handle,
                                    CHAR16 *file_name, UINT64 open_mode, UINT64 attributes) {
    (void)this;
    (void)file_name;
    (void)attributes;
    g_open_calls++;
    CHECK_INT("open mode is read-only", EFI_FILE_MODE_READ, open_mode);
    if (g_open_status != EFI_SUCCESS) {
        return g_open_status;
    }
    *new_handle = &g_fake_file;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fake_close(EFI_FILE_PROTOCOL *this) {
    (void)this;
    g_close_calls++;
    return EFI_SUCCESS;
}

/* -------- hype_file_get_size() fakes -------- */

static UINT64 g_fake_file_size;
static int g_getinfo_calls;
static int g_getinfo_second_call_status_success; /* if false, second call also fails */
static EFI_STATUS g_getinfo_first_status; /* what the zero-size probe call returns */

static EFI_STATUS EFIAPI fake_get_info(EFI_FILE_PROTOCOL *this, EFI_GUID *information_type,
                                        UINTN *buffer_size, void *buffer) {
    (void)this;
    (void)information_type;
    g_getinfo_calls++;

    if (buffer == 0) {
        *buffer_size = sizeof(EFI_FILE_INFO_HEADER) + 16; /* pretend a filename follows */
        return g_getinfo_first_status;
    }

    if (!g_getinfo_second_call_status_success) {
        return EFI_NOT_FOUND;
    }
    ((EFI_FILE_INFO_HEADER *)buffer)->Size = *buffer_size;
    ((EFI_FILE_INFO_HEADER *)buffer)->FileSize = g_fake_file_size;
    return EFI_SUCCESS;
}

static int g_allocate_pool_calls;
static EFI_STATUS g_allocate_pool_status;
static unsigned char g_pool_backing[256];

static EFI_STATUS EFIAPI fake_allocate_pool(UINT32 pool_type, UINTN size, void **buffer) {
    (void)pool_type;
    g_allocate_pool_calls++;
    if (g_allocate_pool_status != EFI_SUCCESS) {
        return g_allocate_pool_status;
    }
    if (size > sizeof(g_pool_backing)) {
        return EFI_ABORTED;
    }
    *buffer = g_pool_backing;
    return EFI_SUCCESS;
}

static int g_free_pool_calls;

static EFI_STATUS EFIAPI fake_free_pool(void *buffer) {
    (void)buffer;
    g_free_pool_calls++;
    return EFI_SUCCESS;
}

static void reset_fakes(void) {
    memset(&g_fake_root, 0, sizeof(g_fake_root));
    memset(&g_fake_file, 0, sizeof(g_fake_file));
    g_fake_root.Open = fake_open;
    g_fake_file.Close = fake_close;
    g_fake_file.GetInfo = fake_get_info;

    g_open_calls = 0;
    g_open_status = EFI_SUCCESS;
    g_close_calls = 0;
    g_getinfo_calls = 0;
    g_getinfo_first_status = EFI_BUFFER_TOO_SMALL;
    g_getinfo_second_call_status_success = 1;
    g_fake_file_size = 0;
    g_allocate_pool_calls = 0;
    g_allocate_pool_status = EFI_SUCCESS;
    g_free_pool_calls = 0;
}

static void test_get_size_success(void) {
    EFI_BOOT_SERVICES bs;
    UINT64 size = 0;
    EFI_STATUS status;

    reset_fakes();
    memset(&bs, 0, sizeof(bs));
    bs.AllocatePool = fake_allocate_pool;
    bs.FreePool = fake_free_pool;
    g_fake_file_size = 3653632;

    status = hype_file_get_size(&g_fake_root, &bs, 0, &size);

    CHECK_INT("status", (int)EFI_SUCCESS, (int)status);
    CHECK_INT("size", 3653632, (long long)size);
    CHECK_INT("open called once", 1, g_open_calls);
    CHECK_INT("getinfo called twice (probe + real)", 2, g_getinfo_calls);
    CHECK_INT("allocate pool called once", 1, g_allocate_pool_calls);
    CHECK_INT("free pool called once", 1, g_free_pool_calls);
    CHECK_INT("close called once", 1, g_close_calls);
}

static void test_get_size_open_fails(void) {
    EFI_BOOT_SERVICES bs;
    UINT64 size = 0;
    EFI_STATUS status;

    reset_fakes();
    memset(&bs, 0, sizeof(bs));
    g_open_status = EFI_NOT_FOUND;

    status = hype_file_get_size(&g_fake_root, &bs, 0, &size);

    CHECK_INT("propagates open failure", (int)EFI_NOT_FOUND, (int)status);
    CHECK_INT("getinfo never called", 0, g_getinfo_calls);
}

static void test_get_size_first_getinfo_unexpectedly_succeeds(void) {
    EFI_BOOT_SERVICES bs;
    UINT64 size = 0;
    EFI_STATUS status;

    reset_fakes();
    memset(&bs, 0, sizeof(bs));
    g_getinfo_first_status = EFI_SUCCESS; /* contract violation: should have been EFI_BUFFER_TOO_SMALL */

    status = hype_file_get_size(&g_fake_root, &bs, 0, &size);

    CHECK_INT("aborted when probe doesn't report buffer-too-small", (int)EFI_ABORTED, (int)status);
    CHECK_INT("close still called", 1, g_close_calls);
}

static void test_get_size_first_getinfo_other_error(void) {
    EFI_BOOT_SERVICES bs;
    UINT64 size = 0;
    EFI_STATUS status;

    reset_fakes();
    memset(&bs, 0, sizeof(bs));
    g_getinfo_first_status = EFI_NOT_FOUND;

    status = hype_file_get_size(&g_fake_root, &bs, 0, &size);

    CHECK_INT("propagates the real error", (int)EFI_NOT_FOUND, (int)status);
}

static void test_get_size_allocate_pool_fails(void) {
    EFI_BOOT_SERVICES bs;
    UINT64 size = 0;
    EFI_STATUS status;

    reset_fakes();
    memset(&bs, 0, sizeof(bs));
    bs.AllocatePool = fake_allocate_pool;
    g_allocate_pool_status = EFI_NOT_FOUND;

    status = hype_file_get_size(&g_fake_root, &bs, 0, &size);

    CHECK_INT("propagates allocate pool failure", (int)EFI_NOT_FOUND, (int)status);
    CHECK_INT("close still called", 1, g_close_calls);
}

static void test_get_size_second_getinfo_fails(void) {
    EFI_BOOT_SERVICES bs;
    UINT64 size = 0;
    EFI_STATUS status;

    reset_fakes();
    memset(&bs, 0, sizeof(bs));
    bs.AllocatePool = fake_allocate_pool;
    bs.FreePool = fake_free_pool;
    g_getinfo_second_call_status_success = 0;

    status = hype_file_get_size(&g_fake_root, &bs, 0, &size);

    CHECK_INT("propagates second getinfo failure", (int)EFI_NOT_FOUND, (int)status);
    CHECK_INT("pool freed even on failure", 1, g_free_pool_calls);
    CHECK_INT("close still called", 1, g_close_calls);
}

/* -------- hype_file_read_into() -------- */

static UINTN g_fake_read_result_size;
static EFI_STATUS g_fake_read_status;
static int g_read_calls;

static EFI_STATUS EFIAPI fake_read(EFI_FILE_PROTOCOL *this, UINTN *buffer_size, void *buffer) {
    (void)this;
    (void)buffer;
    g_read_calls++;
    if (g_fake_read_status != EFI_SUCCESS) {
        return g_fake_read_status;
    }
    *buffer_size = g_fake_read_result_size;
    return EFI_SUCCESS;
}

static void test_read_into_success(void) {
    unsigned char buf[64];
    EFI_STATUS status;

    reset_fakes();
    g_fake_file.Read = fake_read;
    g_fake_read_status = EFI_SUCCESS;
    g_fake_read_result_size = sizeof(buf);
    g_read_calls = 0;

    status = hype_file_read_into(&g_fake_root, 0, buf, sizeof(buf));

    CHECK_INT("status", (int)EFI_SUCCESS, (int)status);
    CHECK_INT("read called once", 1, g_read_calls);
    CHECK_INT("close called once", 1, g_close_calls);
}

static void test_read_into_open_fails(void) {
    unsigned char buf[64];
    EFI_STATUS status;

    reset_fakes();
    g_fake_file.Read = fake_read;
    g_open_status = EFI_NOT_FOUND;
    g_read_calls = 0;

    status = hype_file_read_into(&g_fake_root, 0, buf, sizeof(buf));

    CHECK_INT("propagates open failure", (int)EFI_NOT_FOUND, (int)status);
    CHECK_INT("read never called", 0, g_read_calls);
}

static void test_read_into_read_fails(void) {
    unsigned char buf[64];
    EFI_STATUS status;

    reset_fakes();
    g_fake_file.Read = fake_read;
    g_fake_read_status = EFI_NOT_FOUND;
    g_read_calls = 0;

    status = hype_file_read_into(&g_fake_root, 0, buf, sizeof(buf));

    CHECK_INT("propagates read failure", (int)EFI_NOT_FOUND, (int)status);
    CHECK_INT("close still called", 1, g_close_calls);
}

static void test_read_into_short_read_is_aborted(void) {
    unsigned char buf[64];
    EFI_STATUS status;

    reset_fakes();
    g_fake_file.Read = fake_read;
    g_fake_read_status = EFI_SUCCESS;
    g_fake_read_result_size = sizeof(buf) - 1; /* short read */

    status = hype_file_read_into(&g_fake_root, 0, buf, sizeof(buf));

    CHECK_INT("short read is aborted", (int)EFI_ABORTED, (int)status);
    CHECK_INT("close still called", 1, g_close_calls);
}

/* -------- hype_file_locate_root() -------- */

static EFI_LOADED_IMAGE_PROTOCOL g_fake_loaded_image;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL g_fake_fs;
static int g_handle_protocol_calls;
static EFI_STATUS g_handle_protocol_first_status;
static EFI_STATUS g_handle_protocol_second_status;
static EFI_HANDLE g_seen_image_handle;
static EFI_HANDLE g_seen_device_handle;

static EFI_STATUS EFIAPI fake_handle_protocol(EFI_HANDLE handle, EFI_GUID *protocol, void **interface) {
    (void)protocol;
    g_handle_protocol_calls++;
    if (g_handle_protocol_calls == 1) {
        g_seen_image_handle = handle;
        if (g_handle_protocol_first_status != EFI_SUCCESS) {
            return g_handle_protocol_first_status;
        }
        *interface = &g_fake_loaded_image;
        return EFI_SUCCESS;
    }
    g_seen_device_handle = handle;
    if (g_handle_protocol_second_status != EFI_SUCCESS) {
        return g_handle_protocol_second_status;
    }
    *interface = &g_fake_fs;
    return EFI_SUCCESS;
}

static int g_open_volume_calls;
static EFI_STATUS g_open_volume_status;

static EFI_STATUS EFIAPI fake_open_volume(void *this, EFI_FILE_PROTOCOL **root) {
    (void)this;
    g_open_volume_calls++;
    if (g_open_volume_status != EFI_SUCCESS) {
        return g_open_volume_status;
    }
    *root = &g_fake_root;
    return EFI_SUCCESS;
}

static void reset_locate_root_fakes(void) {
    memset(&g_fake_loaded_image, 0, sizeof(g_fake_loaded_image));
    memset(&g_fake_fs, 0, sizeof(g_fake_fs));
    g_fake_loaded_image.DeviceHandle = (EFI_HANDLE)0x1234;
    g_fake_fs.OpenVolume = fake_open_volume;

    g_handle_protocol_calls = 0;
    g_handle_protocol_first_status = EFI_SUCCESS;
    g_handle_protocol_second_status = EFI_SUCCESS;
    g_open_volume_calls = 0;
    g_open_volume_status = EFI_SUCCESS;
}

static void test_locate_root_success(void) {
    EFI_BOOT_SERVICES bs;
    EFI_FILE_PROTOCOL *root = 0;
    EFI_STATUS status;
    EFI_HANDLE image_handle = (EFI_HANDLE)0xABCD;

    reset_locate_root_fakes();
    memset(&bs, 0, sizeof(bs));
    bs.HandleProtocol = fake_handle_protocol;

    status = hype_file_locate_root(image_handle, &bs, &root);

    CHECK_INT("status", (int)EFI_SUCCESS, (int)status);
    CHECK_INT("handle protocol called twice", 2, g_handle_protocol_calls);
    CHECK_INT("open volume called once", 1, g_open_volume_calls);
    if (g_seen_image_handle != image_handle) {
        printf("FAIL: first HandleProtocol call didn't receive our own image handle\n");
        failures++;
    }
    if (g_seen_device_handle != g_fake_loaded_image.DeviceHandle) {
        printf("FAIL: second HandleProtocol call didn't receive DeviceHandle\n");
        failures++;
    }
    if (root != &g_fake_root) {
        printf("FAIL: locate_root did not return OpenVolume's own root pointer\n");
        failures++;
    }
}

static void test_locate_root_first_handle_protocol_fails(void) {
    EFI_BOOT_SERVICES bs;
    EFI_FILE_PROTOCOL *root = 0;
    EFI_STATUS status;

    reset_locate_root_fakes();
    memset(&bs, 0, sizeof(bs));
    bs.HandleProtocol = fake_handle_protocol;
    g_handle_protocol_first_status = EFI_NOT_FOUND;

    status = hype_file_locate_root((EFI_HANDLE)0x1, &bs, &root);

    CHECK_INT("propagates failure", (int)EFI_NOT_FOUND, (int)status);
    CHECK_INT("only called once", 1, g_handle_protocol_calls);
    CHECK_INT("open volume never called", 0, g_open_volume_calls);
}

static void test_locate_root_second_handle_protocol_fails(void) {
    EFI_BOOT_SERVICES bs;
    EFI_FILE_PROTOCOL *root = 0;
    EFI_STATUS status;

    reset_locate_root_fakes();
    memset(&bs, 0, sizeof(bs));
    bs.HandleProtocol = fake_handle_protocol;
    g_handle_protocol_second_status = EFI_NOT_FOUND;

    status = hype_file_locate_root((EFI_HANDLE)0x1, &bs, &root);

    CHECK_INT("propagates failure", (int)EFI_NOT_FOUND, (int)status);
    CHECK_INT("open volume never called", 0, g_open_volume_calls);
}

static void test_locate_root_open_volume_fails(void) {
    EFI_BOOT_SERVICES bs;
    EFI_FILE_PROTOCOL *root = 0;
    EFI_STATUS status;

    reset_locate_root_fakes();
    memset(&bs, 0, sizeof(bs));
    bs.HandleProtocol = fake_handle_protocol;
    g_open_volume_status = EFI_NOT_FOUND;

    status = hype_file_locate_root((EFI_HANDLE)0x1, &bs, &root);

    CHECK_INT("propagates failure", (int)EFI_NOT_FOUND, (int)status);
}

int main(void) {
    test_locate_root_success();
    test_locate_root_first_handle_protocol_fails();
    test_locate_root_second_handle_protocol_fails();
    test_locate_root_open_volume_fails();
    test_get_size_success();
    test_get_size_open_fails();
    test_get_size_first_getinfo_unexpectedly_succeeds();
    test_get_size_first_getinfo_other_error();
    test_get_size_allocate_pool_fails();
    test_get_size_second_getinfo_fails();
    test_read_into_success();
    test_read_into_open_fails();
    test_read_into_read_fails();
    test_read_into_short_read_is_aborted();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
