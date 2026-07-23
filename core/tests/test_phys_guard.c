#include <stdio.h>
#include <string.h>
#include "../phys_guard.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_guid_parse(void) {
    uint8_t g[16];
    int i, bad = 0;
    CHECK_HEX("grouped guid parses", 0,
              hype_phys_guid_parse("abfcf892-fa76-3c41-87ac-3598d881fee8", g));
    CHECK_HEX("byte0", 0xabu, g[0]);
    CHECK_HEX("byte1", 0xfcu, g[1]);
    CHECK_HEX("byte15", 0xe8u, g[15]);
    /* ungrouped (32 hex, no dashes) is equivalent. */
    {
        uint8_t g2[16];
        CHECK_HEX("ungrouped guid parses", 0,
                  hype_phys_guid_parse("abfcf892fa763c4187ac3598d881fee8", g2));
        for (i = 0; i < 16; i++) { if (g[i] != g2[i]) bad = 1; }
        CHECK_HEX("grouped == ungrouped", 0, bad);
    }
    /* uppercase hex digits parse the same as lowercase. */
    {
        uint8_t gu[16];
        CHECK_HEX("uppercase guid parses", 0,
                  hype_phys_guid_parse("ABFCF892-FA76-3C41-87AC-3598D881FEE8", gu));
        CHECK_HEX("uppercase byte0", 0xabu, gu[0]);
        CHECK_HEX("uppercase byte15", 0xe8u, gu[15]);
    }
    CHECK_HEX("too short rejected", (unsigned long long)(-1),
              (unsigned long long)hype_phys_guid_parse("abfc", g));
    CHECK_HEX("too long rejected", (unsigned long long)(-1),
              (unsigned long long)hype_phys_guid_parse("abfcf892fa763c4187ac3598d881fee800", g));
    CHECK_HEX("non-hex rejected", (unsigned long long)(-1),
              (unsigned long long)hype_phys_guid_parse("abfcf892-fa76-3c41-87ac-3598d881feeZ", g));
    CHECK_HEX("NULL rejected", (unsigned long long)(-1),
              (unsigned long long)hype_phys_guid_parse((const char *)0, g));
}

static const uint8_t GUID[16] = {0xab,0xfc,0xf8,0x92,0xfa,0x76,0x3c,0x41,
                                 0x87,0xac,0x35,0x98,0xd8,0x81,0xfe,0xe8};

static hype_phys_guard_ctx_t base_ctx(void) {
    hype_phys_guard_ctx_t c;
    c.configured_id = "QM00013";
    c.drive_serial = "QM00013";
    c.disk_guid = GUID;
    c.partition_table_nonempty = 0;
    c.allow_overwrite = 0;
    c.operator_confirmed = 1;
    return c;
}

static void test_allow_paths(void) {
    hype_phys_guard_ctx_t c = base_ctx();
    CHECK_HEX("serial match + empty + confirmed => ALLOW", HYPE_PHYS_GUARD_ALLOW,
              hype_phys_guard_check(&c));

    /* match by GUID instead of serial */
    c = base_ctx();
    c.configured_id = "abfcf892-fa76-3c41-87ac-3598d881fee8";
    c.drive_serial = "SOMETHINGELSE";
    CHECK_HEX("GUID match => ALLOW", HYPE_PHYS_GUARD_ALLOW, hype_phys_guard_check(&c));

    /* non-empty but overwrite explicitly allowed */
    c = base_ctx();
    c.partition_table_nonempty = 1;
    c.allow_overwrite = 1;
    CHECK_HEX("nonempty + allow_overwrite + confirmed => ALLOW", HYPE_PHYS_GUARD_ALLOW,
              hype_phys_guard_check(&c));

    /* case-insensitive serial */
    c = base_ctx();
    c.configured_id = "qm00013";
    CHECK_HEX("serial match case-insensitive => ALLOW", HYPE_PHYS_GUARD_ALLOW,
              hype_phys_guard_check(&c));
}

static void test_deny_paths(void) {
    hype_phys_guard_ctx_t c;

    /* identity mismatch: neither serial nor GUID matches */
    c = base_ctx();
    c.configured_id = "WRONGSERIAL";
    CHECK_HEX("mismatch => DENY_ID_MISMATCH", HYPE_PHYS_GUARD_DENY_ID_MISMATCH,
              hype_phys_guard_check(&c));

    /* mismatch takes priority over everything else */
    c = base_ctx();
    c.configured_id = "WRONGSERIAL";
    c.partition_table_nonempty = 1;
    c.operator_confirmed = 0;
    CHECK_HEX("mismatch beats other denies", HYPE_PHYS_GUARD_DENY_ID_MISMATCH,
              hype_phys_guard_check(&c));

    /* non-empty disk, no override */
    c = base_ctx();
    c.partition_table_nonempty = 1;
    CHECK_HEX("nonempty no override => DENY_NONEMPTY", HYPE_PHYS_GUARD_DENY_NONEMPTY,
              hype_phys_guard_check(&c));

    /* nonempty deny beats missing confirmation */
    c = base_ctx();
    c.partition_table_nonempty = 1;
    c.operator_confirmed = 0;
    CHECK_HEX("nonempty beats needs-confirm", HYPE_PHYS_GUARD_DENY_NONEMPTY,
              hype_phys_guard_check(&c));

    /* matched, empty, but not confirmed */
    c = base_ctx();
    c.operator_confirmed = 0;
    CHECK_HEX("empty + unconfirmed => DENY_NEEDS_CONFIRM", HYPE_PHYS_GUARD_DENY_NEEDS_CONFIRM,
              hype_phys_guard_check(&c));

    /* NULL configured id => mismatch (never matches) */
    c = base_ctx();
    c.configured_id = (const char *)0;
    CHECK_HEX("NULL configured id => mismatch", HYPE_PHYS_GUARD_DENY_ID_MISMATCH,
              hype_phys_guard_check(&c));

    /* GUID present but configured id is a non-matching GUID string, serial NULL */
    c = base_ctx();
    c.drive_serial = (const char *)0;
    c.configured_id = "00000000-0000-0000-0000-000000000000";
    CHECK_HEX("wrong GUID + no serial => mismatch", HYPE_PHYS_GUARD_DENY_ID_MISMATCH,
              hype_phys_guard_check(&c));
}

static void test_part_table_nonempty(void) {
    uint8_t s0[512], s1[512];
    unsigned i;
    for (i = 0; i < 512u; i++) { s0[i] = 0; s1[i] = 0; }

    /* blank disk -> empty */
    CHECK_HEX("blank -> 0", 0, hype_phys_part_table_nonempty(s0, s1));

    /* GPT header on sector 1 -> nonempty */
    { const char *sig = "EFI PART"; for (i = 0; i < 8u; i++) s1[i] = (uint8_t)sig[i]; }
    CHECK_HEX("GPT sig -> 1", 1, hype_phys_part_table_nonempty(s0, s1));
    CHECK_HEX("GPT sig, s0 NULL -> 1", 1, hype_phys_part_table_nonempty((const uint8_t *)0, s1));
    for (i = 0; i < 8u; i++) s1[i] = 0; /* clear GPT */

    /* MBR boot sig but all partition types zero -> empty */
    s0[510] = 0x55u; s0[511] = 0xAAu;
    CHECK_HEX("MBR sig, no partitions -> 0", 0, hype_phys_part_table_nonempty(s0, s1));

    /* MBR sig + a non-zero partition type (entry 2, type byte) -> nonempty */
    s0[446u + 1u * 16u + 4u] = 0x83u; /* Linux partition type */
    CHECK_HEX("MBR + partition -> 1", 1, hype_phys_part_table_nonempty(s0, s1));

    /* partition-looking bytes but NO boot sig -> empty (not a valid MBR) */
    s0[510] = 0; s0[511] = 0;
    CHECK_HEX("partition byte but no 0x55AA -> 0", 0, hype_phys_part_table_nonempty(s0, s1));

    /* both NULL -> 0 */
    CHECK_HEX("both NULL -> 0", 0,
              hype_phys_part_table_nonempty((const uint8_t *)0, (const uint8_t *)0));
}

static void test_arm(void) {
    uint8_t s0[512], s1[512];
    unsigned i;
    for (i = 0; i < 512u; i++) { s0[i] = 0; s1[i] = 0; }

    /* matched serial, blank disk, confirmed -> ALLOW */
    CHECK_HEX("arm: match+blank+confirmed => ALLOW", HYPE_PHYS_GUARD_ALLOW,
              hype_phys_guard_arm("QM00013", "QM00013", GUID, s0, s1, 0, 1));
    /* mismatch -> DENY_ID_MISMATCH regardless */
    CHECK_HEX("arm: mismatch => DENY", HYPE_PHYS_GUARD_DENY_ID_MISMATCH,
              hype_phys_guard_arm("WRONG", "QM00013", GUID, s0, s1, 0, 1));
    /* matched but unconfirmed -> NEEDS_CONFIRM */
    CHECK_HEX("arm: unconfirmed => NEEDS_CONFIRM", HYPE_PHYS_GUARD_DENY_NEEDS_CONFIRM,
              hype_phys_guard_arm("QM00013", "QM00013", GUID, s0, s1, 0, 0));
    /* GPT on disk, matched, confirmed, no override -> DENY_NONEMPTY */
    { const char *sig = "EFI PART"; for (i = 0; i < 8u; i++) s1[i] = (uint8_t)sig[i]; }
    CHECK_HEX("arm: nonempty disk => DENY_NONEMPTY", HYPE_PHYS_GUARD_DENY_NONEMPTY,
              hype_phys_guard_arm("QM00013", "QM00013", GUID, s0, s1, 0, 1));
    /* ... unless overwrite explicitly allowed -> ALLOW */
    CHECK_HEX("arm: nonempty + allow_overwrite => ALLOW", HYPE_PHYS_GUARD_ALLOW,
              hype_phys_guard_arm("QM00013", "QM00013", GUID, s0, s1, 1, 1));
}

int main(void) {
    test_guid_parse();
    test_allow_paths();
    test_deny_paths();
    test_part_table_nonempty();
    test_arm();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
