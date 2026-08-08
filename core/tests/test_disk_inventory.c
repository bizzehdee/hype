#include <stdio.h>
#include <string.h>

#include "../disk_inventory.h"

static int failures = 0;
#define CHECK(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)
#define CHECK_INT(desc, expected, actual)                                                    \
    do {                                                                                     \
        long long e_ = (long long)(expected), a_ = (long long)(actual);                      \
        if (e_ != a_) {                                                                      \
            printf("FAIL: %s (expected %lld, got %lld)\n", (desc), e_, a_);                  \
            failures++;                                                                      \
        }                                                                                    \
    } while (0)

/* The layout the ticket was filed about: ESP on port 0, scratch on port 1, and
 * the config targets the scratch. The old single-port scan stopped at port 0 and
 * the scratch was unreachable. */
static void test_second_port_disk_is_selectable(void) {
    hype_disk_inventory_t inv;
    int idx;
    const hype_disk_entry_t *e;

    hype_disk_inventory_reset(&inv);
    CHECK_INT("esp on port 0", 0,
              hype_disk_inventory_add(&inv, HYPE_DISK_BUS_AHCI, 0xF0000000ull, 0u, "HYPEESPDISK",
                                      "QEMU HARDDISK", 1433600ull));
    CHECK_INT("scratch on port 1", 0,
              hype_disk_inventory_add(&inv, HYPE_DISK_BUS_AHCI, 0xF0000000ull, 1u,
                                      "HYPE228SCRATCH", "QEMU HARDDISK", 8388608ull));

    idx = hype_disk_inventory_find_serial(&inv, "HYPE228SCRATCH");
    CHECK("the second-port disk is found", idx >= 0);
    e = hype_disk_inventory_get(&inv, (unsigned int)idx);
    CHECK("entry returned", e != 0);
    if (e != 0) {
        CHECK_INT("its port is the one it was found on", 1u, e->port);
        CHECK_INT("its capacity came with it", 8388608ull, (long long)e->total_sectors);
        CHECK_INT("its bar came with it", 0xF0000000ull, (long long)e->bar_phys);
    }
}

static void test_absent_serial_is_distinguishable_from_present(void) {
    hype_disk_inventory_t inv;
    hype_disk_inventory_reset(&inv);
    (void)hype_disk_inventory_add(&inv, HYPE_DISK_BUS_AHCI, 1u, 0u, "PRESENT", "m", 10u);

    CHECK_INT("a disk that is here is found", 0, hype_disk_inventory_find_serial(&inv, "PRESENT"));
    CHECK_INT("a disk that is not here is not found", -1,
              hype_disk_inventory_find_serial(&inv, "ELSEWHERE"));
    CHECK_INT("and the inventory says how many WERE seen", 1u, inv.count);
}

/* An empty config value must not select a disk that reported no serial -- that
 * would arm a WRITE target by accident. */
static void test_empty_serial_never_matches(void) {
    hype_disk_inventory_t inv;
    hype_disk_inventory_reset(&inv);
    (void)hype_disk_inventory_add(&inv, HYPE_DISK_BUS_AHCI, 1u, 0u, "", "no-serial-disk", 10u);

    CHECK_INT("empty target does not match the empty-serial disk", -1,
              hype_disk_inventory_find_serial(&inv, ""));
    CHECK_INT("NULL target does not match", -1, hype_disk_inventory_find_serial(&inv, 0));
    CHECK_INT("empty serial is not counted as a match", 0u,
              hype_disk_inventory_count_serial(&inv, ""));
}

/* Two disks with the same serial makes "matched by serial" ambiguous. The
 * caller must be able to see that and refuse, rather than write to a guess. */
static void test_duplicate_serials_are_countable(void) {
    hype_disk_inventory_t inv;
    hype_disk_inventory_reset(&inv);
    (void)hype_disk_inventory_add(&inv, HYPE_DISK_BUS_AHCI, 1u, 0u, "SAME", "a", 10u);
    (void)hype_disk_inventory_add(&inv, HYPE_DISK_BUS_AHCI, 1u, 3u, "SAME", "b", 20u);
    (void)hype_disk_inventory_add(&inv, HYPE_DISK_BUS_NVME, 2u, 0u, "UNIQUE", "c", 30u);

    CHECK_INT("duplicate counted twice", 2u, hype_disk_inventory_count_serial(&inv, "SAME"));
    CHECK_INT("unique counted once", 1u, hype_disk_inventory_count_serial(&inv, "UNIQUE"));
    CHECK_INT("find still returns the first", 0, hype_disk_inventory_find_serial(&inv, "SAME"));
}

/* Both buses live in one inventory, so selection does not care which driver
 * found the disk -- only which one must be used to reach it. */
static void test_both_buses_coexist(void) {
    hype_disk_inventory_t inv;
    int idx;
    const hype_disk_entry_t *e;
    hype_disk_inventory_reset(&inv);
    (void)hype_disk_inventory_add(&inv, HYPE_DISK_BUS_AHCI, 0xA000u, 2u, "SATA1", "ssd", 100u);
    (void)hype_disk_inventory_add(&inv, HYPE_DISK_BUS_NVME, 0xB000u, 0u, "NVME1", "m2", 200u);

    idx = hype_disk_inventory_find_serial(&inv, "NVME1");
    e = hype_disk_inventory_get(&inv, (unsigned int)idx);
    CHECK("nvme entry found", e != 0);
    if (e != 0) CHECK_INT("bus recorded", HYPE_DISK_BUS_NVME, e->bus);

    idx = hype_disk_inventory_find_serial(&inv, "SATA1");
    e = hype_disk_inventory_get(&inv, (unsigned int)idx);
    CHECK("ahci entry found", e != 0);
    if (e != 0) CHECK_INT("bus recorded", HYPE_DISK_BUS_AHCI, e->bus);
}

/* A full inventory must SAY it dropped disks. Silent truncation reads exactly
 * like "that disk is not in this machine", which is the misreading this ticket
 * exists to remove. */
static void test_overflow_is_reported_not_silent(void) {
    hype_disk_inventory_t inv;
    unsigned int i;
    char sn[8];
    hype_disk_inventory_reset(&inv);
    for (i = 0; i < HYPE_DISK_INVENTORY_MAX + 3u; i++) {
        sn[0] = 'D'; sn[1] = (char)('0' + (i % 10u)); sn[2] = '\0';
        (void)hype_disk_inventory_add(&inv, HYPE_DISK_BUS_AHCI, 1u, i, sn, "m", 10u);
    }
    CHECK_INT("count clamps to capacity", HYPE_DISK_INVENTORY_MAX, inv.count);
    CHECK_INT("dropped is counted", 3u, inv.dropped);
    CHECK_INT("add returns failure when full", -1,
              hype_disk_inventory_add(&inv, HYPE_DISK_BUS_AHCI, 1u, 99u, "X", "m", 10u));
}

static void test_long_strings_truncate_rather_than_overrun(void) {
    hype_disk_inventory_t inv;
    const hype_disk_entry_t *e;
    hype_disk_inventory_reset(&inv);
    (void)hype_disk_inventory_add(&inv, HYPE_DISK_BUS_AHCI, 1u, 0u,
                                  "0123456789012345678901234567890123456789",
                                  "0123456789012345678901234567890123456789012345678901234567890",
                                  10u);
    e = hype_disk_inventory_get(&inv, 0);
    CHECK("entry stored", e != 0);
    if (e != 0) {
        CHECK_INT("serial truncated to its field", 20u, (unsigned int)strlen(e->serial));
        CHECK_INT("model truncated to its field", 40u, (unsigned int)strlen(e->model));
    }
}

static void test_get_out_of_range_and_null_safety(void) {
    hype_disk_inventory_t inv;
    hype_disk_inventory_reset(&inv);
    CHECK("empty inventory returns nothing", hype_disk_inventory_get(&inv, 0) == 0);
    CHECK("null inventory is safe to search", hype_disk_inventory_find_serial(0, "x") == -1);
    CHECK("null inventory is safe to get", hype_disk_inventory_get(0, 0) == 0);
    CHECK_INT("null inventory counts nothing", 0u, hype_disk_inventory_count_serial(0, "x"));
    CHECK_INT("null inventory refuses an add", -1,
              hype_disk_inventory_add(0, HYPE_DISK_BUS_AHCI, 1u, 0u, "s", "m", 1u));
    hype_disk_inventory_reset(0); /* must not fault */
    (void)hype_disk_inventory_add(&inv, HYPE_DISK_BUS_AHCI, 1u, 0u, 0, 0, 1u);
    CHECK_INT("null strings become empty", 0u, (unsigned int)strlen(inv.disks[0].serial));
}

int main(void) {
    test_second_port_disk_is_selectable();
    test_absent_serial_is_distinguishable_from_present();
    test_empty_serial_never_matches();
    test_duplicate_serials_are_countable();
    test_both_buses_coexist();
    test_overflow_is_reported_not_silent();
    test_long_strings_truncate_rather_than_overrun();
    test_get_out_of_range_and_null_safety();
    if (failures != 0) {
        printf("test_disk_inventory: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_disk_inventory: all checks passed\n");
    return 0;
}
