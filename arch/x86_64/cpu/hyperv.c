#include "hyperv.h"

static void install_stub(uint8_t *page, hype_hv_hypercall_vendor_t vendor) {
    unsigned i;

    /* The page is hypervisor-provided executable content. Clear the complete
     * overlay before publishing the entry sequence so no prior guest bytes can
     * remain as accidental entry points. */
    for (i = 0; i < HYPE_HV_HYPERCALL_PAGE_SIZE; i++) {
        page[i] = 0;
    }

    page[0] = 0x0Fu;
    page[1] = 0x01u;
    page[2] = (vendor == HYPE_HV_HYPERCALL_VENDOR_SVM) ? 0xD9u : 0xC1u;
    page[3] = 0xC3u; /* near RET, the return behavior required by the TLFS */
}

int hype_hv_hypercall_page_write(uint64_t current_value, uint64_t requested_value,
                                 uint64_t guest_os_id, const hype_gpa_map_t *map,
                                 hype_hv_hypercall_vendor_t vendor, uint64_t *out_value) {
    uint64_t gpa;
    uint64_t host;

    if (out_value == 0) {
        return -1;
    }
    if ((current_value & HYPE_HV_HYPERCALL_LOCKED) != 0u) {
        *out_value = current_value;
        return 0;
    }
    if ((requested_value & HYPE_HV_HYPERCALL_ENABLE) == 0u || guest_os_id == 0u) {
        *out_value = requested_value & ~HYPE_HV_HYPERCALL_ENABLE;
        return 0;
    }
    if (map == 0) {
        return -1;
    }

    gpa = requested_value & HYPE_HV_HYPERCALL_GPA_MASK;
    host = hype_gpa_to_host(map, gpa, HYPE_HV_HYPERCALL_PAGE_SIZE);
    if (host == 0u) {
        return -1;
    }

    install_stub((uint8_t *)(uintptr_t)host, vendor);
    *out_value = requested_value;
    return 0;
}

uint64_t hype_hv_hypercall_disable(uint64_t msr_value) {
    return msr_value & ~HYPE_HV_HYPERCALL_ENABLE;
}

uint64_t hype_hv_hypercall_dispatch(uint64_t input_value) {
    /* Bits 15:0 are the call code. Keep the input read explicit so extending
     * this switch with implemented calls cannot accidentally use another field. */
    uint16_t call_code = (uint16_t)input_value;
    (void)call_code;
    return HYPE_HV_STATUS_INVALID_HYPERCALL_CODE;
}

int hype_hv_reference_tsc_write(uint64_t requested_value, const hype_gpa_map_t *map,
                                uint64_t tsc_hz, uint64_t *out_value) {
    if ((requested_value & HYPE_HV_REF_TSC_ENABLE) == 0u || tsc_hz == 0u) {
        *out_value = requested_value & ~HYPE_HV_REF_TSC_ENABLE;
        return 0;
    }
    {
        uint64_t gpa = requested_value & HYPE_HV_REF_TSC_GPA_MASK;
        uint8_t *host = (uint8_t *)(uintptr_t)hype_gpa_to_host(map, gpa, 4096u);
        uint64_t q1, r1, scale;
        unsigned i;
        if (host == 0) {
            return -1;
        }
        /*
         * TscScale = floor(2^64 * 10^7 / tsc_hz), without 128-bit division
         * (freestanding: no __udivti3). 2^64 = q1*hz + r1 with r1 in [1, hz]:
         * derive from UINT64_MAX = 2^64 - 1.
         */
        q1 = 0xFFFFFFFFFFFFFFFFull / tsc_hz;
        r1 = (0xFFFFFFFFFFFFFFFFull % tsc_hz) + 1u;
        if (r1 == tsc_hz) {
            q1 += 1u;
            r1 = 0u;
        }
        scale = q1 * 10000000ull + (r1 * 10000000ull) / tsc_hz;
        /* Zero the page, then scale/offset, then a valid sequence LAST so a
         * concurrent reader never pairs a live sequence with stale fields.
         * (Sequence 0 means "invalid, fall back to the reference-count MSR";
         * any other value is valid under the current TLFS.) */
        for (i = 0; i < 4096u; i++) {
            host[i] = 0;
        }
        /* TscScale at offset 8, TscOffset at offset 16 (both little-endian). */
        for (i = 0; i < 8u; i++) {
            host[8u + i] = (uint8_t)(scale >> (8u * i));
        }
        /* TscOffset = 0: the guest TSC is the raw host TSC (tsc_offset=0). */
        host[0] = 1u; /* TscSequence = 1 */
        *out_value = requested_value;
    }
    return 0;
}
