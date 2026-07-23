#include "phys_guard.h"

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int hype_phys_guid_parse(const char *s, uint8_t out16[16]) {
    unsigned n = 0; /* nibble count consumed into out16 */

    if (s == (const char *)0) {
        return -1;
    }
    while (*s != '\0') {
        int v;
        if (*s == '-') {
            s++;
            continue; /* group separators are cosmetic */
        }
        v = hexval(*s);
        if (v < 0 || n >= 32u) {
            return -1; /* non-hex char, or more than 16 bytes of hex */
        }
        if ((n & 1u) == 0u) {
            out16[n / 2u] = (uint8_t)(v << 4); /* high nibble */
        } else {
            out16[n / 2u] |= (uint8_t)v; /* low nibble */
        }
        n++;
        s++;
    }
    return (n == 32u) ? 0 : -1; /* must be exactly 16 bytes */
}

/* Case-insensitive equality of two NUL-terminated strings. Callers
 * (identity_matches) only invoke this with non-NULL arguments. */
static int str_ieq(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b; /* both ended together */
}

/* Does `configured_id` name this drive -- by ATA/NVMe serial, or by GPT GUID? */
static int identity_matches(const hype_phys_guard_ctx_t *c) {
    if (c->configured_id == (const char *)0) {
        return 0;
    }
    if (c->drive_serial != (const char *)0 && str_ieq(c->configured_id, c->drive_serial)) {
        return 1;
    }
    if (c->disk_guid != (const uint8_t *)0) {
        uint8_t want[16];
        if (hype_phys_guid_parse(c->configured_id, want) == 0) {
            unsigned i;
            int eq = 1;
            for (i = 0; i < 16u; i++) {
                if (want[i] != c->disk_guid[i]) {
                    eq = 0;
                }
            }
            if (eq) {
                return 1;
            }
        }
    }
    return 0;
}

int hype_phys_part_table_nonempty(const uint8_t *sector0, const uint8_t *sector1) {
    /* GPT: the header sits in LBA 1 and begins with "EFI PART" (ECMA/UEFI). */
    if (sector1 != 0) {
        static const uint8_t gpt_sig[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
        int gpt = 1;
        unsigned i;
        for (i = 0; i < 8u; i++) {
            if (sector1[i] != gpt_sig[i]) {
                gpt = 0;
                break;
            }
        }
        if (gpt) {
            return 1;
        }
    }
    /* MBR: 0x55AA boot signature at bytes 510-511 + a non-zero partition type in
     * any of the four 16-byte entries at offset 446 (type byte at entry+4). */
    if (sector0 != 0) {
        if (sector0[510] == 0x55u && sector0[511] == 0xAAu) {
            unsigned e;
            for (e = 0; e < 4u; e++) {
                if (sector0[446u + e * 16u + 4u] != 0u) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

hype_phys_guard_result_t hype_phys_guard_check(const hype_phys_guard_ctx_t *c) {
    if (!identity_matches(c)) {
        return HYPE_PHYS_GUARD_DENY_ID_MISMATCH;
    }
    if (c->partition_table_nonempty && !c->allow_overwrite) {
        return HYPE_PHYS_GUARD_DENY_NONEMPTY;
    }
    if (!c->operator_confirmed) {
        return HYPE_PHYS_GUARD_DENY_NEEDS_CONFIRM;
    }
    return HYPE_PHYS_GUARD_ALLOW;
}

hype_phys_guard_result_t hype_phys_guard_arm(const char *configured_id, const char *drive_serial,
                                             const uint8_t *disk_guid, const uint8_t *sector0,
                                             const uint8_t *sector1, int allow_overwrite,
                                             int operator_confirmed) {
    hype_phys_guard_ctx_t c;
    c.configured_id = configured_id;
    c.drive_serial = drive_serial;
    c.disk_guid = disk_guid;
    c.partition_table_nonempty = hype_phys_part_table_nonempty(sector0, sector1);
    c.allow_overwrite = allow_overwrite;
    c.operator_confirmed = operator_confirmed;
    return hype_phys_guard_check(&c);
}
