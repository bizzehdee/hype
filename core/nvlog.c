#include "nvlog.h"

unsigned int hype_nvlog_tail_offset(unsigned int total_len, unsigned int cap) {
    return (total_len > cap) ? (total_len - cap) : 0u;
}

uint32_t hype_nvlog_checksum(const char *data, unsigned int len) {
    uint32_t sum = 0;
    unsigned int i;

    if (data == 0) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        sum += (unsigned char)data[i];
    }
    return sum;
}

int hype_nvlog_should_write(uint64_t now_tsc, uint64_t last_write_tsc, uint64_t interval_tsc,
                            int have_written, uint32_t cur_checksum, uint32_t last_checksum) {
    if (!have_written) {
        return 1;
    }
    if (cur_checksum == last_checksum) {
        return 0; /* nothing new -- don't burn a flash write */
    }
    return (now_tsc - last_write_tsc >= interval_tsc) ? 1 : 0;
}

EFI_STATUS hype_nvlog_write(EFI_RUNTIME_SERVICES *rt, const char *data, unsigned int len) {
    EFI_GUID guid = HYPE_NVLOG_GUID;
    unsigned int off;
    unsigned int size;

    if (rt == 0 || rt->SetVariable == 0 || data == 0) {
        return EFI_NOT_FOUND;
    }
    off = hype_nvlog_tail_offset(len, HYPE_NVLOG_CAPACITY);
    size = len - off; /* == min(len, HYPE_NVLOG_CAPACITY) */
    return rt->SetVariable((CHAR16 *)HYPE_NVLOG_VAR_NAME, &guid, HYPE_NVLOG_ATTRS, (UINTN)size,
                           (void *)(const void *)(data + off));
}

EFI_STATUS hype_nvlog_read(EFI_RUNTIME_SERVICES *rt, char *out, unsigned int out_cap,
                           unsigned int *out_len) {
    EFI_GUID guid = HYPE_NVLOG_GUID;
    UINTN size = (UINTN)out_cap;
    EFI_STATUS status;

    if (out_len != 0) {
        *out_len = 0;
    }
    if (rt == 0 || rt->GetVariable == 0 || out == 0) {
        return EFI_NOT_FOUND;
    }
    status = rt->GetVariable((CHAR16 *)HYPE_NVLOG_VAR_NAME, &guid, 0, &size, out);
    if (status == EFI_SUCCESS && out_len != 0) {
        *out_len = (size > out_cap) ? out_cap : (unsigned int)size;
    }
    return status;
}

EFI_STATUS hype_nvlog_clear(EFI_RUNTIME_SERVICES *rt) {
    EFI_GUID guid = HYPE_NVLOG_GUID;

    if (rt == 0 || rt->SetVariable == 0) {
        return EFI_NOT_FOUND;
    }
    /* A zero-size SetVariable deletes the variable (UEFI spec). */
    return rt->SetVariable((CHAR16 *)HYPE_NVLOG_VAR_NAME, &guid, HYPE_NVLOG_ATTRS, 0, (void *)0);
}
