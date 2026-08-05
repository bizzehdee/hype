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
    /* #243: the default fixture is a believable disk, so the existing cases keep
     * testing what they were written to test. The untrustworthy path has its own. */
    c.sectors_trustworthy = 1;
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
    /*
     * #243: a REAL blank disk is not two zeroed sectors -- every mainstream tool leaves
     * a protective MBR or at least 0x55AA. These tests used all-zero sectors as
     * "blank", which the trustworthiness check now (correctly) refuses, so give them a
     * plausible blank disk: boot signature present, all four partition types zero.
     * The all-zero case is exercised on purpose in test_arm_all_zero_is_refused().
     */
    s0[510] = 0x55u;
    s0[511] = 0xAAu;

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

static void test_attach_mode_separates_attach_from_write(void) {
    /* #267: only an identity mismatch means "not this drive". Every other guard
     * refusal is a refusal to WRITE -- the disk may still be attached read-only so
     * the guest can boot it. Conflating the two made a boot=disk run silently fall
     * back to a blank RAM scratch. */
    if (hype_phys_attach_mode(HYPE_PHYS_GUARD_DENY_ID_MISMATCH) != HYPE_PHYS_ATTACH_NONE) {
        printf("FAIL: an identity mismatch must not attach at all\n");
        failures++;
    }
    if (hype_phys_attach_mode(HYPE_PHYS_GUARD_ALLOW) != HYPE_PHYS_ATTACH_WRITABLE) {
        printf("FAIL: a full guard pass must attach writable\n");
        failures++;
    }
    /* The case behind the bug: an already-installed (non-blank) disk with no
     * allow_overwrite. Booting it is not installing to it. */
    if (hype_phys_attach_mode(HYPE_PHYS_GUARD_DENY_NONEMPTY) != HYPE_PHYS_ATTACH_READ_ONLY) {
        printf("FAIL: a non-blank disk must still attach READ-ONLY so it can be booted\n");
        failures++;
    }
    if (hype_phys_attach_mode(HYPE_PHYS_GUARD_DENY_NEEDS_CONFIRM) != HYPE_PHYS_ATTACH_READ_ONLY) {
        printf("FAIL: an unconfirmed disk must still attach READ-ONLY\n");
        failures++;
    }
}

static void test_attach_mode_never_writable_without_full_pass(void) {
    /* The safety invariant, stated independently of the mapping above: WRITABLE is
     * reachable from HYPE_PHYS_GUARD_ALLOW and from nothing else. */
    int r;
    for (r = 0; r <= (int)HYPE_PHYS_GUARD_DENY_NEEDS_CONFIRM; r++) {
        hype_phys_attach_mode_t m = hype_phys_attach_mode((hype_phys_guard_result_t)r);
        if (m == HYPE_PHYS_ATTACH_WRITABLE && r != (int)HYPE_PHYS_GUARD_ALLOW) {
            printf("FAIL: guard result %d must not yield a writable attach\n", r);
            failures++;
        }
    }
}

static void test_sectors_trustworthy(void) {
    static uint8_t z0[512], z1[512];
    static uint8_t mbr[512], gpt[512];
    unsigned i;
    for (i = 0; i < 512u; i++) { z0[i] = 0; z1[i] = 0; mbr[i] = 0; gpt[i] = 0; }

    /* #243: two fully-zero sectors is a dying drive returning nothing without an
     * error, NOT a blank disk -- any mainstream tool leaves a protective MBR or at
     * least 0x55AA behind. Fails closed. */
    CHECK_HEX("all-zero LBA0+LBA1 is NOT trustworthy", 0, hype_phys_sectors_trustworthy(z0, z1));

    mbr[510] = 0x55u; mbr[511] = 0xAAu;
    CHECK_HEX("a boot signature makes it believable", 1, hype_phys_sectors_trustworthy(mbr, z1));
    gpt[0] = 'E'; gpt[1] = 'F'; gpt[2] = 'I';
    CHECK_HEX("content in LBA1 alone is enough", 1, hype_phys_sectors_trustworthy(z0, gpt));
    /* NULL is the least trustworthy state there is. */
    CHECK_HEX("NULL sector0", 0, hype_phys_sectors_trustworthy(0, z1));
    CHECK_HEX("NULL sector1", 0, hype_phys_sectors_trustworthy(z0, 0));
}

static void test_sectors_agree(void) {
    static uint8_t a0[512], a1[512], b0[512], b1[512];
    unsigned i;
    for (i = 0; i < 512u; i++) { a0[i] = (uint8_t)i; a1[i] = (uint8_t)(i ^ 0xFFu);
                                 b0[i] = (uint8_t)i; b1[i] = (uint8_t)(i ^ 0xFFu); }
    CHECK_HEX("identical reads agree", 1, hype_phys_sectors_agree(a0, a1, b0, b1));

    /* A drive whose content changes between back-to-back reads is lying whatever the
     * bytes say -- this is the observed SN5000 failure mode. */
    b0[17] ^= 0x01u;
    CHECK_HEX("a single differing byte in LBA0 disagrees", 0,
              hype_phys_sectors_agree(a0, a1, b0, b1));
    b0[17] ^= 0x01u;
    b1[511] ^= 0x80u;
    CHECK_HEX("a differing LAST byte of LBA1 disagrees", 0,
              hype_phys_sectors_agree(a0, a1, b0, b1));
    b1[511] ^= 0x80u;
    CHECK_HEX("restored, agrees again", 1, hype_phys_sectors_agree(a0, a1, b0, b1));
    CHECK_HEX("NULL never agrees", 0, hype_phys_sectors_agree(0, a1, b0, b1));
}

static void test_untrustworthy_denies_and_beats_allow_overwrite(void) {
    hype_phys_guard_ctx_t c;
    c.configured_id = "SN123";
    c.drive_serial = "SN123";
    c.disk_guid = 0;
    c.partition_table_nonempty = 0; /* looks blank -- which is exactly the trap */
    c.allow_overwrite = 0;
    c.operator_confirmed = 1;
    c.sectors_trustworthy = 0;
    CHECK_HEX("untrustworthy sectors deny even when the table looks empty",
              HYPE_PHYS_GUARD_DENY_UNTRUSTWORTHY_SECTORS, hype_phys_guard_check(&c));

    /* allow_overwrite means "I know there is data here, wipe it" -- an informed choice
     * about KNOWN content. It cannot express consent about content nobody can read, so
     * it must not override this. */
    c.allow_overwrite = 1;
    CHECK_HEX("allow_overwrite does NOT override untrustworthy sectors",
              HYPE_PHYS_GUARD_DENY_UNTRUSTWORTHY_SECTORS, hype_phys_guard_check(&c));

    /* Identity still reported first: an operator who aimed at the wrong drive must
     * hear that, not a health complaint about a disk they never meant to touch. */
    c.drive_serial = "OTHER";
    CHECK_HEX("identity mismatch still wins", HYPE_PHYS_GUARD_DENY_ID_MISMATCH,
              hype_phys_guard_check(&c));

    /* And a trustworthy blank disk still passes, so this did not just deny everything. */
    c.drive_serial = "SN123";
    c.sectors_trustworthy = 1;
    c.allow_overwrite = 0;
    CHECK_HEX("trustworthy blank disk still allowed", HYPE_PHYS_GUARD_ALLOW,
              hype_phys_guard_check(&c));
}

static void test_untrustworthy_attaches_nothing(void) {
    /* NONE rather than READ_ONLY: this disk cannot be trusted as a data SOURCE either,
     * and an install that "succeeds" against fabricated content is worse than no disk. */
    CHECK_HEX("untrustworthy -> attach nothing", HYPE_PHYS_ATTACH_NONE,
              hype_phys_attach_mode(HYPE_PHYS_GUARD_DENY_UNTRUSTWORTHY_SECTORS));
}

static void test_arm_all_zero_is_refused(void) {
    uint8_t s0[512], s1[512];
    unsigned i;
    for (i = 0; i < 512u; i++) { s0[i] = 0; s1[i] = 0; }
    /*
     * #243, end to end through arm(): a disk whose first two sectors read back as all
     * zeros is refused rather than treated as blank. This is the exact shape of the
     * observed SN5000 failure -- reads succeed, content is empty, the disk is full.
     */
    CHECK_HEX("arm: all-zero sectors => DENY_UNTRUSTWORTHY",
              HYPE_PHYS_GUARD_DENY_UNTRUSTWORTHY_SECTORS,
              hype_phys_guard_arm("QM00013", "QM00013", GUID, s0, s1, 0, 1));
    /* And not rescued by allow_overwrite, which is consent about KNOWN content. */
    CHECK_HEX("arm: all-zero + allow_overwrite still denied",
              HYPE_PHYS_GUARD_DENY_UNTRUSTWORTHY_SECTORS,
              hype_phys_guard_arm("QM00013", "QM00013", GUID, s0, s1, 1, 1));
}

/* ---- #332: partition-scoped emptiness (a wipe-the-wrong-thing guard, so tested hard) ---- */

static void put(uint8_t *sec, unsigned off, const char *str) {
    unsigned i;
    for (i = 0; str[i] != '\0'; i++) {
        sec[off + i] = (uint8_t)str[i];
    }
}

static void test_partition_nonempty_recognises_each_filesystem(void) {
    uint8_t s0[512], s2[512];
    unsigned i;

    /* A blank partition: all zeros. This is the case that MUST read as empty -- it is exactly what a
     * freshly created partition looks like, and refusing it would make the feature useless. */
    for (i = 0; i < 512u; i++) { s0[i] = 0; s2[i] = 0; }
    CHECK_HEX("an all-zero partition is EMPTY", 0, hype_phys_partition_nonempty(s0, s2));

    /* exFAT / NTFS: OEM name at offset 3. */
    for (i = 0; i < 512u; i++) s0[i] = 0;
    put(s0, 3, "EXFAT   ");
    CHECK_HEX("exFAT detected", 1, hype_phys_partition_nonempty(s0, s2));

    for (i = 0; i < 512u; i++) s0[i] = 0;
    put(s0, 3, "NTFS    ");
    CHECK_HEX("NTFS detected", 1, hype_phys_partition_nonempty(s0, s2));

    /* FAT32 names its type at 82; FAT12/16 at 54. */
    for (i = 0; i < 512u; i++) s0[i] = 0;
    put(s0, 82, "FAT32   ");
    CHECK_HEX("FAT32 detected", 1, hype_phys_partition_nonempty(s0, s2));

    for (i = 0; i < 512u; i++) s0[i] = 0;
    put(s0, 54, "FAT16   ");
    CHECK_HEX("FAT16 detected", 1, hype_phys_partition_nonempty(s0, s2));

    for (i = 0; i < 512u; i++) s0[i] = 0;
    put(s0, 54, "FAT12   ");
    CHECK_HEX("FAT12 detected", 1, hype_phys_partition_nonempty(s0, s2));

    /* A raw ISO in a partition -- the GLADDER-10 media layout does exactly this, so overwriting one
     * would destroy an operator's installer media. */
    for (i = 0; i < 512u; i++) s0[i] = 0;
    put(s0, 1, "CD001");
    CHECK_HEX("ISO9660 detected", 1, hype_phys_partition_nonempty(s0, s2));

    /* ext2/3/4: magic 0xEF53 at superblock offset 56, i.e. byte 56 of partition sector 2. */
    for (i = 0; i < 512u; i++) { s0[i] = 0; s2[i] = 0; }
    s2[56] = 0x53u; s2[57] = 0xEFu;
    CHECK_HEX("ext detected via sector 2", 1, hype_phys_partition_nonempty(s0, s2));
    /* ...and NOT via sector 0, which is why the second sector is a parameter at all. */
    CHECK_HEX("ext is invisible without sector 2", 0, hype_phys_partition_nonempty(s0, 0));
}

static void test_partition_nonempty_does_not_key_off_0x55aa(void) {
    uint8_t s0[512], s2[512];
    unsigned i;

    for (i = 0; i < 512u; i++) { s0[i] = 0; s2[i] = 0; }
    /* A boot signature with no filesystem: plenty of non-filesystem content carries 0x55AA, and
     * treating it as "occupied" would refuse partitions that are genuinely free -- leaving the
     * operator no way forward but allow_overwrite, which defeats the guard entirely. */
    s0[510] = 0x55u;
    s0[511] = 0xAAu;
    CHECK_HEX("0x55AA alone does not mean occupied", 0, hype_phys_partition_nonempty(s0, s2));

    /* A whole-disk MBR partition table, by contrast, IS occupied -- but that is the other function's
     * question, and it must NOT be answered by this one. */
    s0[446 + 4] = 0x83u;
    CHECK_HEX("an MBR table in the partition's own sector 0 is still not a filesystem here", 0,
              hype_phys_partition_nonempty(s0, s2));
    CHECK_HEX("...while the whole-disk check does report it", 1,
              hype_phys_part_table_nonempty(s0, (const uint8_t *)0));
}

static void test_partition_nonempty_null_tolerance(void) {
    uint8_t s0[512];
    unsigned i;
    for (i = 0; i < 512u; i++) s0[i] = 0;
    put(s0, 3, "EXFAT   ");

    CHECK_HEX("NULL sector2 still detects a boot-sector filesystem", 1,
              hype_phys_partition_nonempty(s0, 0));
    CHECK_HEX("both NULL contributes nothing", 0, hype_phys_partition_nonempty(0, 0));
}


static void test_arm_partition_asks_each_question_of_the_right_sectors(void) {
    uint8_t disk0[512], disk1[512], part0[512], part2[512];
    unsigned i;
    hype_phys_guard_result_t r;

    /* A realistic drive: GPT-partitioned (so the WHOLE-disk check would say "occupied"), holding one
     * genuinely EMPTY partition. This combination is the whole reason the partition path exists -- the
     * old whole-disk guard would refuse it outright. */
    for (i = 0; i < 512u; i++) { disk0[i] = 0; disk1[i] = 0; part0[i] = 0; part2[i] = 0; }
    disk0[510] = 0x55u; disk0[511] = 0xAAu;         /* protective MBR */
    put(disk1, 0, "EFI PART");                      /* GPT header */

    r = hype_phys_guard_arm_partition("SN-1", "SN-1", 0, disk0, disk1, part0, part2,
                                      /*allow_overwrite=*/0, /*confirmed=*/0);
    /* Reaches the confirmation gate: identity matched and the PARTITION is blank, even though the
     * DISK plainly has a partition table. */
    CHECK_HEX("a blank partition on a partitioned disk is not refused as non-empty",
              (unsigned long long)HYPE_PHYS_GUARD_DENY_NEEDS_CONFIRM, (unsigned long long)r);

    /* Same disk, but now the partition holds a filesystem: refused without allow_overwrite. */
    put(part0, 3, "EXFAT   ");
    r = hype_phys_guard_arm_partition("SN-1", "SN-1", 0, disk0, disk1, part0, part2, 0, 0);
    CHECK_HEX("an occupied partition IS refused", (unsigned long long)HYPE_PHYS_GUARD_DENY_NONEMPTY,
              (unsigned long long)r);

    /* ...and allow_overwrite gets past it, still needing the operator. */
    r = hype_phys_guard_arm_partition("SN-1", "SN-1", 0, disk0, disk1, part0, part2, 1, 0);
    CHECK_HEX("allow_overwrite still requires confirmation",
              (unsigned long long)HYPE_PHYS_GUARD_DENY_NEEDS_CONFIRM, (unsigned long long)r);

    /* Identity is still checked first. */
    r = hype_phys_guard_arm_partition("SN-OTHER", "SN-1", 0, disk0, disk1, part0, part2, 1, 1);
    CHECK_HEX("identity mismatch still wins", (unsigned long long)HYPE_PHYS_GUARD_DENY_ID_MISMATCH,
              (unsigned long long)r);
}

static void test_arm_partition_drive_health_comes_from_the_disk(void) {
    uint8_t disk0[512], disk1[512], part0[512], part2[512];
    unsigned i;
    hype_phys_guard_result_t r;

    /* #243: a drive whose LBA0/LBA1 are BOTH all-zero is lying, and that verdict must still apply to
     * a partition target -- the health question is about the DISK. */
    for (i = 0; i < 512u; i++) { disk0[i] = 0; disk1[i] = 0; part0[i] = 0; part2[i] = 0; }
    r = hype_phys_guard_arm_partition("SN-1", "SN-1", 0, disk0, disk1, part0, part2, 1, 1);
    CHECK_HEX("an untrustworthy DRIVE is refused even for a partition target",
              (unsigned long long)HYPE_PHYS_GUARD_DENY_UNTRUSTWORTHY_SECTORS, (unsigned long long)r);

    /* With a believable disk and a blank partition, the same call is allowed once confirmed --
     * proving the refusal above came from the disk bytes, not the partition's zeros. */
    disk0[510] = 0x55u; disk0[511] = 0xAAu;
    put(disk1, 0, "EFI PART");
    r = hype_phys_guard_arm_partition("SN-1", "SN-1", 0, disk0, disk1, part0, part2, 0, 1);
    CHECK_HEX("a believable disk with a blank partition is ALLOWED once confirmed",
              (unsigned long long)HYPE_PHYS_GUARD_ALLOW, (unsigned long long)r);
}


int main(void) {
    test_guid_parse();
    test_allow_paths();
    test_deny_paths();
    test_part_table_nonempty();
    test_arm();

    test_attach_mode_separates_attach_from_write();
    test_attach_mode_never_writable_without_full_pass();

    test_sectors_trustworthy();
    test_sectors_agree();
    test_untrustworthy_denies_and_beats_allow_overwrite();
    test_untrustworthy_attaches_nothing();
    test_arm_all_zero_is_refused();
    test_partition_nonempty_recognises_each_filesystem();
    test_partition_nonempty_does_not_key_off_0x55aa();
    test_partition_nonempty_null_tolerance();
    test_arm_partition_asks_each_question_of_the_right_sectors();
    test_arm_partition_drive_health_comes_from_the_disk();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
