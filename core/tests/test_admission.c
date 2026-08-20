#include <stdio.h>
#include <string.h>
#include "../admission.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), (long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void make_vm(hype_cfg_vm_t *vm, const char *name, unsigned int vcpus, unsigned int mem_mb,
                     const char *disk_path) {
    memset(vm, 0, sizeof(*vm));
    strncpy(vm->name, name, sizeof(vm->name) - 1);
    vm->vcpus = vcpus;
    vm->mem_mb = mem_mb;
    vm->boot = HYPE_CFG_BOOT_DISK;
    vm->firmware = HYPE_CFG_FW_UEFI;
    vm->os_hint = HYPE_CFG_OS_NONE;
    vm->net_mode = HYPE_CFG_NET_NONE;
    vm->target_disk.kind = HYPE_CFG_DISK_FILE;
    strncpy(vm->target_disk.path_or_id, disk_path, sizeof(vm->target_disk.path_or_id) - 1);
}

/* ---- hype_adm_check_memory ---- */

static void test_memory_within_budget(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 1, 1024, "a.img");
    make_vm(&cfg.vms[1], "b", 1, 2048, "b.img");

    /* 3072MB used; budget = 8192MB usable - 256MB reserved = 7936MB. */
    r = hype_adm_check_memory(&cfg, 8192ULL * 1024 * 1024, HYPE_ADM_RESERVED_MB_DEFAULT * 1024ULL * 1024);
    CHECK_INT("memory within budget is OK", (int)HYPE_ADM_OK, (int)r.status);
}

static void test_memory_overcommit(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 1, 9000, "a.img");

    /* 9000MB requested; budget = 8192MB - 256MB = 7936MB. */
    r = hype_adm_check_memory(&cfg, 8192ULL * 1024 * 1024, HYPE_ADM_RESERVED_MB_DEFAULT * 1024ULL * 1024);
    CHECK_INT("memory overcommit is rejected", (int)HYPE_ADM_ERR_MEMORY_OVERCOMMIT, (int)r.status);
    CHECK_INT("memory overcommit isn't tied to a specific VM", HYPE_ADM_NO_VM, r.vm_index_a);
}

static void test_memory_reserved_exceeds_usable(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 1, 1, "a.img");

    /* reserved > usable: budget clamps to 0, so even 1MB is rejected. */
    r = hype_adm_check_memory(&cfg, 100ULL * 1024 * 1024, 1000ULL * 1024 * 1024);
    CHECK_INT("reserved exceeding usable clamps budget to zero, still rejects",
              (int)HYPE_ADM_ERR_MEMORY_OVERCOMMIT, (int)r.status);
}

static void test_memory_no_vms_is_ok(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 0;

    r = hype_adm_check_memory(&cfg, 0, 0);
    CHECK_INT("zero VMs never overcommits", (int)HYPE_ADM_OK, (int)r.status);
}

/* ---- hype_adm_check_vcpus ---- */

static void test_vcpus_within_budget(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 2, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 3, 512, "b.img");

    r = hype_adm_check_vcpus(&cfg, 8);
    CHECK_INT("5 vcpus on 8 cores is OK", (int)HYPE_ADM_OK, (int)r.status);
}

static void test_vcpus_overcommit(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 4, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 5, 512, "b.img");

    r = hype_adm_check_vcpus(&cfg, 8);
    CHECK_INT("9 vcpus on 8 cores is rejected", (int)HYPE_ADM_ERR_VCPU_OVERCOMMIT, (int)r.status);
}

static void test_vcpus_exact_fit_is_ok(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 8, 512, "a.img");

    r = hype_adm_check_vcpus(&cfg, 8);
    CHECK_INT("using exactly all cores is OK", (int)HYPE_ADM_OK, (int)r.status);
}

/* ---- hype_adm_check_cpu_set ---- */

static void test_cpu_set_no_explicit_sets_is_ok(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 2, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 2, 512, "b.img");

    r = hype_adm_check_cpu_set(&cfg, 8);
    CHECK_INT("no explicit cpu_set anywhere is OK", (int)HYPE_ADM_OK, (int)r.status);
}

static void test_cpu_set_count_mismatch(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 2, 512, "a.img");
    cfg.vms[0].has_cpu_set = 1;
    cfg.vms[0].cpu_set_count = 3; /* vcpus=2, cpu_set_count=3 -- mismatch */
    cfg.vms[0].cpu_set[0] = 0;
    cfg.vms[0].cpu_set[1] = 1;
    cfg.vms[0].cpu_set[2] = 2;

    r = hype_adm_check_cpu_set(&cfg, 8);
    CHECK_INT("cpu_set count not matching vcpus is rejected",
              (int)HYPE_ADM_ERR_CPU_SET_COUNT_MISMATCH, (int)r.status);
    CHECK_INT("mismatch names the offending VM", 0, r.vm_index_a);
}

static void test_cpu_set_core_out_of_range(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    cfg.vms[0].has_cpu_set = 1;
    cfg.vms[0].cpu_set_count = 1;
    cfg.vms[0].cpu_set[0] = 8; /* only cores 0-7 exist on an 8-core host */

    r = hype_adm_check_cpu_set(&cfg, 8);
    CHECK_INT("a core beyond the host's count is rejected",
              (int)HYPE_ADM_ERR_CPU_SET_CORE_OUT_OF_RANGE, (int)r.status);
}

static void test_cpu_set_overlap_rejected(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 2, 512, "a.img");
    cfg.vms[0].has_cpu_set = 1;
    cfg.vms[0].cpu_set_count = 2;
    cfg.vms[0].cpu_set[0] = 0;
    cfg.vms[0].cpu_set[1] = 1;

    make_vm(&cfg.vms[1], "b", 2, 512, "b.img");
    cfg.vms[1].has_cpu_set = 1;
    cfg.vms[1].cpu_set_count = 2;
    cfg.vms[1].cpu_set[0] = 1; /* shares core 1 with vm[0] */
    cfg.vms[1].cpu_set[1] = 2;

    r = hype_adm_check_cpu_set(&cfg, 8);
    CHECK_INT("overlapping cpu_set ranges are rejected", (int)HYPE_ADM_ERR_CPU_SET_OVERLAP, (int)r.status);
    CHECK_INT("overlap names the first VM", 0, r.vm_index_a);
    CHECK_INT("overlap names the second VM", 1, r.vm_index_b);
}

static void test_cpu_set_disjoint_is_ok(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 2, 512, "a.img");
    cfg.vms[0].has_cpu_set = 1;
    cfg.vms[0].cpu_set_count = 2;
    cfg.vms[0].cpu_set[0] = 0;
    cfg.vms[0].cpu_set[1] = 1;

    make_vm(&cfg.vms[1], "b", 2, 512, "b.img");
    cfg.vms[1].has_cpu_set = 1;
    cfg.vms[1].cpu_set_count = 2;
    cfg.vms[1].cpu_set[0] = 2;
    cfg.vms[1].cpu_set[1] = 3;

    r = hype_adm_check_cpu_set(&cfg, 8);
    CHECK_INT("disjoint cpu_set ranges are OK", (int)HYPE_ADM_OK, (int)r.status);
}

static void test_cpu_set_mixed_explicit_and_auto_is_ok(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 2, 512, "a.img");
    cfg.vms[0].has_cpu_set = 1;
    cfg.vms[0].cpu_set_count = 2;
    cfg.vms[0].cpu_set[0] = 0;
    cfg.vms[0].cpu_set[1] = 1;

    make_vm(&cfg.vms[1], "b", 2, 512, "b.img"); /* no explicit cpu_set at all */

    r = hype_adm_check_cpu_set(&cfg, 8);
    CHECK_INT("one explicit VM alongside one auto-assigned VM is OK", (int)HYPE_ADM_OK, (int)r.status);
}

/* ---- hype_adm_check_target_disk ---- */

static void test_target_disk_unique_is_ok(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 1, 512, "b.img");

    r = hype_adm_check_target_disk(&cfg);
    CHECK_INT("distinct target_disk paths are OK", (int)HYPE_ADM_OK, (int)r.status);
}

static void test_target_disk_collision_same_kind(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 1, 512, "shared.img");
    make_vm(&cfg.vms[1], "b", 1, 512, "shared.img");

    r = hype_adm_check_target_disk(&cfg);
    CHECK_INT("identical file: paths are rejected", (int)HYPE_ADM_ERR_TARGET_DISK_COLLISION, (int)r.status);
    CHECK_INT("collision names the first VM", 0, r.vm_index_a);
    CHECK_INT("collision names the second VM", 1, r.vm_index_b);
}

static void test_target_disk_same_path_different_kind_is_ok(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 1, 512, "SN-1234");
    cfg.vms[0].target_disk.kind = HYPE_CFG_DISK_PHYSICAL;
    make_vm(&cfg.vms[1], "b", 1, 512, "SN-1234");
    cfg.vms[1].target_disk.kind = HYPE_CFG_DISK_FILE;

    r = hype_adm_check_target_disk(&cfg);
    CHECK_INT("same string but different disk kind (file vs physical) is OK",
              (int)HYPE_ADM_OK, (int)r.status);
}

/*
 * #537: two VMs with NO target_disk must NOT collide. An absent inline target leaves the struct
 * zeroed -- kind FILE (enum 0), empty path -- so this compared equal and refused the second VM.
 * Reachable without #535's storage-less boot mode: §5.2's `disks = <id>` reference form leaves
 * target_disk empty too.
 */
static void test_target_disk_absent_on_both_is_ok(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg);
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 1, 512, "b.img");
    /* Both VMs declare no inline target -- what a reference-form or kernel-boot VM looks like. */
    cfg.vms[0].target_disk.path_or_id[0] = '\0';
    cfg.vms[1].target_disk.path_or_id[0] = '\0';

    r = hype_adm_check_target_disk(&cfg);
    CHECK_INT("two VMs with no target_disk do not collide", (int)HYPE_ADM_OK, (int)r.status);
}

static void test_target_disk_absent_on_one_is_ok(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg);
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 1, 512, "b.img");
    cfg.vms[1].target_disk.path_or_id[0] = '\0';

    r = hype_adm_check_target_disk(&cfg);
    CHECK_INT("one VM with a target and one without do not collide", (int)HYPE_ADM_OK,
              (int)r.status);

    /* And the other way round, so the fix is not order-dependent. */
    hype_cfg_init(&cfg);
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 1, 512, "b.img");
    cfg.vms[0].target_disk.path_or_id[0] = '\0';
    r = hype_adm_check_target_disk(&cfg);
    CHECK_INT("absent on the FIRST VM does not collide either", (int)HYPE_ADM_OK, (int)r.status);
}

/*
 * The invariant itself must be untouched: three VMs where only the middle pair share a real path
 * must still be refused, and named correctly.
 */
static void test_target_disk_collision_still_caught_among_absent_ones(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg);
    cfg.vm_count = 3;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 1, 512, "shared.img");
    make_vm(&cfg.vms[2], "c", 1, 512, "shared.img");
    cfg.vms[0].target_disk.path_or_id[0] = '\0';

    r = hype_adm_check_target_disk(&cfg);
    CHECK_INT("a real collision is still refused", (int)HYPE_ADM_ERR_TARGET_DISK_COLLISION,
              (int)r.status);
    CHECK_INT("and names the right first VM", 1, r.vm_index_a);
    CHECK_INT("and the right second VM", 2, r.vm_index_b);
}

/* ---- hype_adm_check_net_peers ---- */

static void test_net_peers_valid_pairing_is_ok(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "debian", 1, 512, "a.img");
    cfg.vms[0].net_mode = HYPE_CFG_NET_NAT;
    cfg.vms[0].net_peers_count = 1;
    strncpy(cfg.vms[0].net_peers[0], "freebsd", sizeof(cfg.vms[0].net_peers[0]) - 1);

    make_vm(&cfg.vms[1], "freebsd", 1, 512, "b.img");
    cfg.vms[1].net_mode = HYPE_CFG_NET_NAT;

    r = hype_adm_check_net_peers(&cfg);
    CHECK_INT("a valid nat<->nat pairing is OK", (int)HYPE_ADM_OK, (int)r.status);
}

static void test_net_peers_unknown_vm_rejected(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "debian", 1, 512, "a.img");
    cfg.vms[0].net_mode = HYPE_CFG_NET_NAT;
    cfg.vms[0].net_peers_count = 1;
    strncpy(cfg.vms[0].net_peers[0], "typo-name", sizeof(cfg.vms[0].net_peers[0]) - 1);

    r = hype_adm_check_net_peers(&cfg);
    CHECK_INT("a net_peers name that isn't a defined VM is rejected",
              (int)HYPE_ADM_ERR_NET_PEER_UNKNOWN_VM, (int)r.status);
    CHECK_INT("unknown-vm error names the referencing VM", 0, r.vm_index_a);
}

static void test_net_peers_requires_nat_on_both_sides(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "debian", 1, 512, "a.img");
    cfg.vms[0].net_mode = HYPE_CFG_NET_NAT;
    cfg.vms[0].net_peers_count = 1;
    strncpy(cfg.vms[0].net_peers[0], "freebsd", sizeof(cfg.vms[0].net_peers[0]) - 1);

    make_vm(&cfg.vms[1], "freebsd", 1, 512, "b.img");
    cfg.vms[1].net_mode = HYPE_CFG_NET_NONE; /* peer isn't NAT */

    r = hype_adm_check_net_peers(&cfg);
    CHECK_INT("peer without net_mode=nat is rejected", (int)HYPE_ADM_ERR_NET_PEER_NOT_NAT, (int)r.status);
    CHECK_INT("not-nat error names the referencing VM", 0, r.vm_index_a);
    CHECK_INT("not-nat error names the peer VM", 1, r.vm_index_b);
}

static void test_net_peers_none_is_ok(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");

    r = hype_adm_check_net_peers(&cfg);
    CHECK_INT("no net_peers anywhere is OK", (int)HYPE_ADM_OK, (int)r.status);
}

/* ---- hype_adm_select_media_dev / hype_adm_check_media_disk (#323) ---- */

static void set_media_disk(hype_cfg_vm_t *vm, const char *serial) {
    vm->has_media_disk = 1;
    strncpy(vm->media_disk, serial, sizeof(vm->media_disk) - 1);
}

static void test_media_disk_unset_is_auto(void) {
    hype_cfg_t cfg;
    static const char *const devs[2] = {"SN-A", "SN-B"};

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");

    CHECK_INT("no media_disk means auto-detect", HYPE_ADM_MEDIA_AUTO,
              hype_adm_select_media_dev(&cfg, 0, devs, 2));
    CHECK_INT("no media_disk passes admission", (int)HYPE_ADM_OK,
              (int)hype_adm_check_media_disk(&cfg, devs, 2).status);
}

static void test_media_disk_matches_second_device(void) {
    hype_cfg_t cfg;
    static const char *const devs[3] = {"SN-A", "SN-B", "SN-C"};

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    set_media_disk(&cfg.vms[0], "SN-B");

    CHECK_INT("named drive resolves to its own index", 1,
              hype_adm_select_media_dev(&cfg, 0, devs, 3));
    CHECK_INT("a present named drive passes admission", (int)HYPE_ADM_OK,
              (int)hype_adm_check_media_disk(&cfg, devs, 3).status);
}

static void test_media_disk_absent_is_refused_not_substituted(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;
    static const char *const devs[2] = {"SN-A", "SN-B"};

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 1, 512, "b.img");
    set_media_disk(&cfg.vms[1], "SN-GONE");

    /* The whole point: two other drives ARE present, and neither may be substituted. */
    CHECK_INT("absent named drive is ABSENT, never a fallback index", HYPE_ADM_MEDIA_ABSENT,
              hype_adm_select_media_dev(&cfg, 1, devs, 2));
    r = hype_adm_check_media_disk(&cfg, devs, 2);
    CHECK_INT("absent named drive is refused", (int)HYPE_ADM_ERR_MEDIA_DISK_ABSENT, (int)r.status);
    CHECK_INT("refusal names the offending VM", 1, r.vm_index_a);
    CHECK_INT("refusal has no second VM", (int)HYPE_ADM_NO_VM, (int)r.vm_index_b);
}

static void test_media_disk_no_devices_enumerated_is_refused(void) {
    hype_cfg_t cfg;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    set_media_disk(&cfg.vms[0], "SN-A");

    CHECK_INT("nothing enumerated cannot match", HYPE_ADM_MEDIA_ABSENT,
              hype_adm_select_media_dev(&cfg, 0, 0, 0));
}

/* A drive that reported no serial can never match: matching it would mean guessing. */
static void test_media_disk_unidentified_devices_never_match(void) {
    hype_cfg_t cfg;
    static const char *const devs[3] = {0, "", "SN-C"};

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    set_media_disk(&cfg.vms[0], "");

    CHECK_INT("an empty media_disk is treated as unset", HYPE_ADM_MEDIA_AUTO,
              hype_adm_select_media_dev(&cfg, 0, devs, 3));

    set_media_disk(&cfg.vms[0], "SN-C");
    CHECK_INT("unidentified drives are skipped, not matched", 2,
              hype_adm_select_media_dev(&cfg, 0, devs, 3));
}

static void test_media_disk_is_exact_match(void) {
    hype_cfg_t cfg;
    static const char *const devs[2] = {"SN-ABC", "SN-ABCDEF"};

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    set_media_disk(&cfg.vms[0], "SN-ABCDEF");

    /* A prefix must not win: serials commonly share a vendor prefix. */
    CHECK_INT("match is exact, not a prefix", 1, hype_adm_select_media_dev(&cfg, 0, devs, 2));
}

static void test_media_disk_out_of_range_vm_is_auto(void) {
    hype_cfg_t cfg;
    static const char *const devs[1] = {"SN-A"};

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    set_media_disk(&cfg.vms[0], "SN-A");

    CHECK_INT("a VM index past vm_count is auto", HYPE_ADM_MEDIA_AUTO,
              hype_adm_select_media_dev(&cfg, 7, devs, 1));
    CHECK_INT("a null config is auto", HYPE_ADM_MEDIA_AUTO,
              hype_adm_select_media_dev(0, 0, devs, 1));
}


/* ---- #329: multi-disk admission ---- */

static void add_disk(hype_cfg_t *c, const char *id, hype_cfg_disk_type_t type,
                     hype_cfg_backing_t backing, int read_only) {
    hype_cfg_disk_t *d = &c->disks[c->disk_count++];
    memset(d, 0, sizeof(*d));
    strncpy(d->id, id, sizeof(d->id) - 1);
    d->type = type;
    d->backing = backing;
    d->has_backing = 1;
    d->read_only = read_only;
}

static void attach(hype_cfg_vm_t *vm, const char *id, int as_cdrom) {
    if (as_cdrom) {
        strncpy(vm->cdroms[vm->cdroms_count++], id, HYPE_CFG_NAME_MAX - 1);
    } else {
        strncpy(vm->disks[vm->disks_count++], id, HYPE_CFG_NAME_MAX - 1);
    }
}

static void test_disk_refs_must_exist_and_match_type(void) {
    hype_cfg_t cfg;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    add_disk(&cfg, "sys", HYPE_CFG_DISK_TYPE_DISK, HYPE_CFG_BACKING_FILE, 0);
    add_disk(&cfg, "inst", HYPE_CFG_DISK_TYPE_CDROM, HYPE_CFG_BACKING_FILE, 1);

    attach(&cfg.vms[0], "sys", 0);
    attach(&cfg.vms[0], "inst", 1);
    CHECK_INT("a well-formed reference set passes", (int)HYPE_ADM_OK,
              (int)hype_adm_check_disk_refs(&cfg).status);

    /* A typo'd id must not silently mean "no disk" -- that is a VM that boots nothing for no visible
     * reason. */
    cfg.vms[0].disks_count = 0;
    attach(&cfg.vms[0], "syz", 0);
    CHECK_INT("an unknown id is refused", (int)HYPE_ADM_ERR_DISK_REF_UNKNOWN,
              (int)hype_adm_check_disk_refs(&cfg).status);

    /* The lists mean different front-ends and different read-only semantics, so a cdrom in disks= is
     * an error rather than something to coerce. */
    cfg.vms[0].disks_count = 0;
    attach(&cfg.vms[0], "inst", 0);
    CHECK_INT("a cdrom in disks= is refused", (int)HYPE_ADM_ERR_DISK_REF_WRONG_TYPE,
              (int)hype_adm_check_disk_refs(&cfg).status);

    cfg.vms[0].disks_count = 0;
    cfg.vms[0].cdroms_count = 0;
    attach(&cfg.vms[0], "sys", 1);
    CHECK_INT("a disk in cdroms= is refused", (int)HYPE_ADM_ERR_DISK_REF_WRONG_TYPE,
              (int)hype_adm_check_disk_refs(&cfg).status);
}

static void test_writable_disks_cannot_be_shared_but_read_only_can(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 1, 512, "b.img");
    add_disk(&cfg, "rw", HYPE_CFG_DISK_TYPE_DISK, HYPE_CFG_BACKING_FILE, 0);
    add_disk(&cfg, "ro", HYPE_CFG_DISK_TYPE_DISK, HYPE_CFG_BACKING_FILE, 1);
    add_disk(&cfg, "iso", HYPE_CFG_DISK_TYPE_CDROM, HYPE_CFG_BACKING_FILE, 1);

    /* Two guests writing one backing store corrupts it, and neither can detect the other. */
    attach(&cfg.vms[0], "rw", 0);
    attach(&cfg.vms[1], "rw", 0);
    r = hype_adm_check_disk_sharing(&cfg);
    CHECK_INT("a shared WRITABLE disk is refused", (int)HYPE_ADM_ERR_DISK_SHARED_WRITABLE,
              (int)r.status);
    CHECK_INT("names the first VM", 0, r.vm_index_a);
    CHECK_INT("names the second", 1, r.vm_index_b);

    /* A shared read-only disk is legitimate -- a common reference image. */
    cfg.vms[0].disks_count = 0;
    cfg.vms[1].disks_count = 0;
    attach(&cfg.vms[0], "ro", 0);
    attach(&cfg.vms[1], "ro", 0);
    CHECK_INT("a shared READ-ONLY disk is allowed", (int)HYPE_ADM_OK,
              (int)hype_adm_check_disk_sharing(&cfg).status);

    /* One installer ISO serving every VM is the normal case, not a mistake. */
    attach(&cfg.vms[0], "iso", 1);
    attach(&cfg.vms[1], "iso", 1);
    CHECK_INT("a shared cdrom is allowed", (int)HYPE_ADM_OK,
              (int)hype_adm_check_disk_sharing(&cfg).status);
}

static void test_physical_overlap_allows_distinct_partitions(void) {
    hype_cfg_t cfg;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 0;

    /* THE case this ticket calls out as easy to get wrong: two DIFFERENT partitions on one drive are
     * disjoint and must be ALLOWED -- refusing them defeats #332's whole purpose. */
    add_disk(&cfg, "p2", HYPE_CFG_DISK_TYPE_DISK, HYPE_CFG_BACKING_PHYSICAL, 0);
    cfg.disks[0].has_id_match = 1;
    strncpy(cfg.disks[0].id_match, "SN-1", sizeof(cfg.disks[0].id_match) - 1);
    cfg.disks[0].partition = 2u;

    add_disk(&cfg, "p3", HYPE_CFG_DISK_TYPE_DISK, HYPE_CFG_BACKING_PHYSICAL, 0);
    cfg.disks[1].has_id_match = 1;
    strncpy(cfg.disks[1].id_match, "SN-1", sizeof(cfg.disks[1].id_match) - 1);
    cfg.disks[1].partition = 3u;

    CHECK_INT("adjacent partitions on one drive are ALLOWED", (int)HYPE_ADM_OK,
              (int)hype_adm_check_disk_phys_overlap(&cfg).status);

    /* The same partition twice is a genuine conflict. */
    cfg.disks[1].partition = 2u;
    CHECK_INT("the same partition twice is refused", (int)HYPE_ADM_ERR_DISK_PHYS_OVERLAP,
              (int)hype_adm_check_disk_phys_overlap(&cfg).status);

    /* whole-disk CONTAINS every partition, so it conflicts with any of them. */
    cfg.disks[1].partition = 0u; /* whole */
    CHECK_INT("whole-disk conflicts with a partition on the same drive",
              (int)HYPE_ADM_ERR_DISK_PHYS_OVERLAP,
              (int)hype_adm_check_disk_phys_overlap(&cfg).status);

    /* Different drives cannot overlap however they are scoped. */
    strncpy(cfg.disks[1].id_match, "SN-2", sizeof(cfg.disks[1].id_match) - 1);
    CHECK_INT("different drives never conflict", (int)HYPE_ADM_OK,
              (int)hype_adm_check_disk_phys_overlap(&cfg).status);
}

static void test_unpresentable_bus_is_refused(void) {
    hype_cfg_t cfg;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    cfg.vms[0].os_hint = HYPE_CFG_OS_LINUX;
    add_disk(&cfg, "d", HYPE_CFG_DISK_TYPE_DISK, HYPE_CFG_BACKING_FILE, 0);
    attach(&cfg.vms[0], "d", 0);

    CHECK_INT("the os_hint default (virtio-blk) is presentable", (int)HYPE_ADM_OK,
              (int)hype_adm_check_disk_bus(&cfg).status);

    cfg.disks[0].bus = HYPE_CFG_BUS_AHCI_SATA;
    CHECK_INT("ahci-sata is presentable (#333)", (int)HYPE_ADM_OK,
              (int)hype_adm_check_disk_bus(&cfg).status);

    /* #202 slice 6: nvme is now a real front-end, so it must be ACCEPTED. This assertion flipped when
     * the controller landed -- it is the gate that stops a config asking for a bus hype cannot present. */
    cfg.disks[0].bus = HYPE_CFG_BUS_NVME;
    CHECK_INT("nvme is accepted now the controller exists", (int)HYPE_ADM_OK,
              (int)hype_adm_check_disk_bus(&cfg).status);

    /* ahci-atapi remains unpresentable as a DISK bus: it is the optical front-end, and a hard disk on
     * it is a config error rather than something to coerce. */
    cfg.disks[0].bus = HYPE_CFG_BUS_AHCI_ATAPI;
    CHECK_INT("ahci-atapi is still refused for a disk", (int)HYPE_ADM_ERR_DISK_BUS_UNSUPPORTED,
              (int)hype_adm_check_disk_bus(&cfg).status);

    /* A windows VM defaulting to ahci-sata must still pass. */
    cfg.disks[0].bus = HYPE_CFG_BUS_DEFAULT;
    cfg.vms[0].os_hint = HYPE_CFG_OS_WINDOWS;
    CHECK_INT("the windows default is presentable", (int)HYPE_ADM_OK,
              (int)hype_adm_check_disk_bus(&cfg).status);
}

/* #329: the parser's list cap (HYPE_CFG_MAX_VM_DISKS) is larger than what the FW-1 machine model can
 * route, so a config can parse cleanly and still name more disks than can exist. The boundary itself
 * is the caller's fact (an interrupt-budget limit) and arrives as a parameter. */
static void test_disk_count_over_frontend_budget_is_refused(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    hype_cfg_init(&cfg); /* #393: zeroes AND binds VM storage */
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    add_disk(&cfg, "d0", HYPE_CFG_DISK_TYPE_DISK, HYPE_CFG_BACKING_FILE, 0);
    add_disk(&cfg, "d1", HYPE_CFG_DISK_TYPE_DISK, HYPE_CFG_BACKING_FILE, 0);
    add_disk(&cfg, "d2", HYPE_CFG_DISK_TYPE_DISK, HYPE_CFG_BACKING_FILE, 0);
    add_disk(&cfg, "d3", HYPE_CFG_DISK_TYPE_DISK, HYPE_CFG_BACKING_FILE, 0);
    attach(&cfg.vms[0], "d0", 0);
    attach(&cfg.vms[0], "d1", 0);
    attach(&cfg.vms[0], "d2", 0);

    CHECK_INT("exactly at the budget passes", (int)HYPE_ADM_OK,
              (int)hype_adm_check_disk_count(&cfg, 3u).status);

    attach(&cfg.vms[0], "d3", 0);
    r = hype_adm_check_disk_count(&cfg, 3u);
    CHECK_INT("one past the budget is refused", (int)HYPE_ADM_ERR_DISK_COUNT_EXCEEDED, (int)r.status);
    CHECK_INT("the refusal names the offending VM", 0, (int)r.vm_index_a);

    CHECK_INT("no config at all passes trivially", (int)HYPE_ADM_OK,
              (int)hype_adm_check_disk_count(&(hype_cfg_t){0}, 3u).status);
}


/* ---- ADM-7 (#453): memory checked against the RESERVED POOL, not a memory-map estimate ---- */

#define ADM_MB (1024ULL * 1024ULL)
#define ADM_GRANULE (2ULL * ADM_MB)
#define ADM_FW (4ULL * ADM_MB)
#define ADM_VDISK (64ULL * ADM_MB)

static void test_pool_admits_what_fits(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;
    unsigned int fit = 99u;
    UINT64 short_by = 1ULL;

    hype_cfg_init(&cfg);
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 1, 512, "b.img");

    /* Each VM costs 512 MiB + 4 MiB firmware + 64 MiB vdisk = 580 MiB; two is 1160 MiB. */
    r = hype_adm_check_pool(&cfg, cfg.vm_count, 1024ULL * ADM_MB, 2048ULL * ADM_MB, ADM_FW, ADM_VDISK, ADM_GRANULE, &fit,
                            &short_by);
    CHECK_INT("both fit a 2 GiB pool", (int)HYPE_ADM_OK, (int)r.status);
    CHECK_INT("and both are counted", 2, (int)fit);
    CHECK_INT("with no shortfall", 0, (int)short_by);
}

/* The point of the change: a VM that does not fit is named, and the ones before it still run. */
static void test_pool_names_the_first_vm_that_does_not_fit(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;
    unsigned int fit = 99u;
    UINT64 short_by = 0ULL;

    hype_cfg_init(&cfg);
    cfg.vm_count = 3;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 1, 512, "b.img");
    make_vm(&cfg.vms[2], "c", 1, 512, "c.img");

    /* 580 MiB each; a 1200 MiB pool holds two. */
    r = hype_adm_check_pool(&cfg, cfg.vm_count, 1024ULL * ADM_MB, 1200ULL * ADM_MB, ADM_FW, ADM_VDISK, ADM_GRANULE, &fit,
                            &short_by);
    CHECK_INT("the third VM is refused", (int)HYPE_ADM_ERR_MEMORY_OVERCOMMIT, (int)r.status);
    CHECK_INT("and it is named by index", 2, (int)r.vm_index_a);
    CHECK_INT("the two that fit are still counted", 2, (int)fit);
    /* 3 x 580 = 1740 against 1200 leaves 540 MiB missing at the third. */
    CHECK_INT("the shortfall is reported in whole MiB", 540, (int)(short_by / ADM_MB));
}

/* A carve consumes whole granules, so admission must count them that way or it will admit a
 * config the allocator then cannot serve -- which is the exact failure #449's own margin bug was. */
static void test_pool_counts_whole_granules(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;
    unsigned int fit = 0u;

    hype_cfg_init(&cfg);
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 1, 1, "a.img"); /* 1 MiB asked, 2 MiB consumed */

    r = hype_adm_check_pool(&cfg, cfg.vm_count, 1024ULL * ADM_MB, 2ULL * ADM_MB, 0ULL, 0ULL, ADM_GRANULE, &fit, 0);
    CHECK_INT("a 1 MiB guest fills a 2 MiB pool exactly", (int)HYPE_ADM_OK, (int)r.status);
    CHECK_INT("and it fits", 1, (int)fit);

    /* Two of them need two granules, not 2 MiB total. */
    cfg.vm_count = 2;
    make_vm(&cfg.vms[1], "b", 1, 1, "b.img");
    r = hype_adm_check_pool(&cfg, cfg.vm_count, 1024ULL * ADM_MB, 2ULL * ADM_MB, 0ULL, 0ULL, ADM_GRANULE, &fit, 0);
    CHECK_INT("two 1 MiB guests do NOT fit one granule",
              (int)HYPE_ADM_ERR_MEMORY_OVERCOMMIT, (int)r.status);
    CHECK_INT("the first still fits", 1, (int)fit);
}

static void test_pool_edge_cases(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;
    unsigned int fit = 5u;

    hype_cfg_init(&cfg);
    cfg.vm_count = 0;
    r = hype_adm_check_pool(&cfg, cfg.vm_count, 1024ULL * ADM_MB, 0ULL, 0ULL, 0ULL, ADM_GRANULE, &fit, 0);
    CHECK_INT("no VMs fit any pool, including an empty one", (int)HYPE_ADM_OK, (int)r.status);
    CHECK_INT("and nothing is counted", 0, (int)fit);

    r = hype_adm_check_pool(0, 1u, 1024ULL * ADM_MB, 1024ULL * ADM_MB, 0ULL, 0ULL, ADM_GRANULE, &fit, 0);
    CHECK_INT("a null config is refused, not admitted",
              (int)HYPE_ADM_ERR_MEMORY_OVERCOMMIT, (int)r.status);

    hype_cfg_init(&cfg);
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 1, 8, "a.img");
    r = hype_adm_check_pool(&cfg, cfg.vm_count, 1024ULL * ADM_MB, 64ULL * ADM_MB, 0ULL, 0ULL, 0ULL, &fit, 0);
    CHECK_INT("a zero granule means no rounding, not a divide", (int)HYPE_ADM_OK, (int)r.status);
    CHECK_INT("optional out-params may be null", (int)HYPE_ADM_OK,
              (int)hype_adm_check_pool(&cfg, cfg.vm_count, 1024ULL * ADM_MB, 64ULL * ADM_MB, 0ULL, 0ULL, ADM_GRANULE, 0, 0).status);
}


/* With no hype.cfg at all, hype runs its built-in default set and the config describes none of
 * them. Checking only the configured VMs reported "0 fit" and capped a default boot to one VM. */
static void test_pool_accounts_for_unconfigured_default_vms(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;
    unsigned int fit = 0u;

    hype_cfg_init(&cfg);
    cfg.vm_count = 0; /* no config */

    r = hype_adm_check_pool(&cfg, 2u, 512ULL * ADM_MB, 4096ULL * ADM_MB, ADM_FW, ADM_VDISK,
                            ADM_GRANULE, &fit, 0);
    CHECK_INT("two default VMs fit a 4 GiB pool", (int)HYPE_ADM_OK, (int)r.status);
    CHECK_INT("and both are counted", 2, (int)fit);

    /* A VM whose configured mem_mb is zero also falls back to the default. */
    hype_cfg_init(&cfg);
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 1, 0, "a.img");
    r = hype_adm_check_pool(&cfg, 1u, 512ULL * ADM_MB, 600ULL * ADM_MB, 0ULL, 0ULL, ADM_GRANULE,
                            &fit, 0);
    CHECK_INT("mem_mb=0 means the default, not free", (int)HYPE_ADM_OK, (int)r.status);
    r = hype_adm_check_pool(&cfg, 1u, 512ULL * ADM_MB, 256ULL * ADM_MB, 0ULL, 0ULL, ADM_GRANULE,
                            &fit, 0);
    CHECK_INT("and it is charged for", (int)HYPE_ADM_ERR_MEMORY_OVERCOMMIT, (int)r.status);
}


/* ---- hype_adm_vms_are_peers (#84/#85) ---- */

/*
 * DEFAULT-DENY is the property, so the no-config case is tested first and hardest: two VMs that both
 * have networking and neither names the other must not be able to reach each other. Guests are never
 * reachable from one another by accident.
 */
static void test_peers_default_deny(void) {
    hype_cfg_t cfg;

    hype_cfg_init(&cfg);
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 1, 512, "b.img");
    cfg.vms[0].net_mode = HYPE_CFG_NET_NAT;
    cfg.vms[1].net_mode = HYPE_CFG_NET_NAT;

    CHECK_INT("two networked VMs naming nobody are not peers", 0,
              hype_adm_vms_are_peers(&cfg, 0, 1));
    CHECK_INT("and it is symmetric", 0, hype_adm_vms_are_peers(&cfg, 1, 0));
}

/* Listing on EITHER side establishes the pair -- plan.md §6e: "no need to list it on both". */
static void test_peers_one_sided_listing_is_bidirectional(void) {
    hype_cfg_t cfg;

    hype_cfg_init(&cfg);
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 1, 512, "b.img");
    cfg.vms[0].net_mode = HYPE_CFG_NET_NAT;
    cfg.vms[1].net_mode = HYPE_CFG_NET_NAT;
    cfg.vms[0].net_peers_count = 1;
    strncpy(cfg.vms[0].net_peers[0], "b", sizeof(cfg.vms[0].net_peers[0]) - 1);

    CHECK_INT("a names b, so a may reach b", 1, hype_adm_vms_are_peers(&cfg, 0, 1));
    CHECK_INT("and b may reach a WITHOUT naming a", 1, hype_adm_vms_are_peers(&cfg, 1, 0));

    /* The other direction of the same rule. */
    hype_cfg_init(&cfg);
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 1, 512, "b.img");
    cfg.vms[0].net_mode = HYPE_CFG_NET_NAT;
    cfg.vms[1].net_mode = HYPE_CFG_NET_NAT;
    cfg.vms[1].net_peers_count = 1;
    strncpy(cfg.vms[1].net_peers[0], "a", sizeof(cfg.vms[1].net_peers[0]) - 1);
    CHECK_INT("b names a, so a may reach b", 1, hype_adm_vms_are_peers(&cfg, 0, 1));
}

/*
 * A pairing must not open a third VM. This is the difference between `net_peers` and a shared
 * broadcast domain, and it is the whole reason §6e keeps the pairwise mechanism.
 */
static void test_peers_a_pairing_does_not_include_a_third_vm(void) {
    hype_cfg_t cfg;

    hype_cfg_init(&cfg);
    cfg.vm_count = 3;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 1, 512, "b.img");
    make_vm(&cfg.vms[2], "c", 1, 512, "c.img");
    cfg.vms[0].net_mode = HYPE_CFG_NET_NAT;
    cfg.vms[1].net_mode = HYPE_CFG_NET_NAT;
    cfg.vms[2].net_mode = HYPE_CFG_NET_NAT;
    cfg.vms[0].net_peers_count = 1;
    strncpy(cfg.vms[0].net_peers[0], "b", sizeof(cfg.vms[0].net_peers[0]) - 1);

    CHECK_INT("a and b are peers", 1, hype_adm_vms_are_peers(&cfg, 0, 1));
    CHECK_INT("c is not reachable from a", 0, hype_adm_vms_are_peers(&cfg, 0, 2));
    CHECK_INT("nor from b", 0, hype_adm_vms_are_peers(&cfg, 1, 2));
    CHECK_INT("nor is a reachable from c", 0, hype_adm_vms_are_peers(&cfg, 2, 0));
}

/*
 * A named pair where one end has no networking is not a pair. The startup check refuses that config
 * (test_net_peers_requires_nat_on_both_sides), but a forwarding decision must not depend on the
 * check having run -- it is the last thing between a packet and another guest's ring.
 */
static void test_peers_requires_networking_on_both_sides(void) {
    hype_cfg_t cfg;

    hype_cfg_init(&cfg);
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 1, 512, "b.img");
    cfg.vms[0].net_mode = HYPE_CFG_NET_NAT;
    cfg.vms[1].net_mode = HYPE_CFG_NET_NONE; /* named, but has no NIC */
    cfg.vms[0].net_peers_count = 1;
    strncpy(cfg.vms[0].net_peers[0], "b", sizeof(cfg.vms[0].net_peers[0]) - 1);

    CHECK_INT("a peer with no NIC is not a peer", 0, hype_adm_vms_are_peers(&cfg, 0, 1));
}

/* A VM is not its own peer: forwarding a guest's packet back into its own ring is a loop, not
 * connectivity. And out-of-range indices answer no rather than reading past the array. */
static void test_peers_self_and_bounds(void) {
    hype_cfg_t cfg;

    hype_cfg_init(&cfg);
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 1, 512, "b.img");
    cfg.vms[0].net_mode = HYPE_CFG_NET_NAT;
    cfg.vms[1].net_mode = HYPE_CFG_NET_NAT;
    cfg.vms[0].net_peers_count = 1;
    strncpy(cfg.vms[0].net_peers[0], "a", sizeof(cfg.vms[0].net_peers[0]) - 1); /* names itself */

    CHECK_INT("a VM naming itself is still not its own peer", 0,
              hype_adm_vms_are_peers(&cfg, 0, 0));
    CHECK_INT("an index past vm_count is refused", 0, hype_adm_vms_are_peers(&cfg, 0, 9));
    CHECK_INT("...either way round", 0, hype_adm_vms_are_peers(&cfg, 9, 0));
    CHECK_INT("a null config is refused", 0, hype_adm_vms_are_peers(0, 0, 1));
}

int main(void) {
    test_peers_default_deny();
    test_peers_one_sided_listing_is_bidirectional();
    test_peers_a_pairing_does_not_include_a_third_vm();
    test_peers_requires_networking_on_both_sides();
    test_peers_self_and_bounds();
    test_pool_admits_what_fits();
    test_pool_names_the_first_vm_that_does_not_fit();
    test_pool_counts_whole_granules();
    test_pool_edge_cases();
    test_pool_accounts_for_unconfigured_default_vms();
    test_memory_within_budget();
    test_memory_overcommit();
    test_memory_reserved_exceeds_usable();
    test_memory_no_vms_is_ok();
    test_vcpus_within_budget();
    test_vcpus_overcommit();
    test_vcpus_exact_fit_is_ok();
    test_cpu_set_no_explicit_sets_is_ok();
    test_cpu_set_count_mismatch();
    test_cpu_set_core_out_of_range();
    test_cpu_set_overlap_rejected();
    test_cpu_set_disjoint_is_ok();
    test_cpu_set_mixed_explicit_and_auto_is_ok();
    test_target_disk_unique_is_ok();
    test_target_disk_collision_same_kind();
    test_target_disk_same_path_different_kind_is_ok();
    test_target_disk_absent_on_both_is_ok();
    test_target_disk_absent_on_one_is_ok();
    test_target_disk_collision_still_caught_among_absent_ones();
    test_net_peers_valid_pairing_is_ok();
    test_net_peers_unknown_vm_rejected();
    test_net_peers_requires_nat_on_both_sides();
    test_net_peers_none_is_ok();
    test_media_disk_unset_is_auto();
    test_media_disk_matches_second_device();
    test_media_disk_absent_is_refused_not_substituted();
    test_media_disk_no_devices_enumerated_is_refused();
    test_media_disk_unidentified_devices_never_match();
    test_media_disk_is_exact_match();
    test_media_disk_out_of_range_vm_is_auto();
    test_disk_refs_must_exist_and_match_type();
    test_writable_disks_cannot_be_shared_but_read_only_can();
    test_physical_overlap_allows_distinct_partitions();
    test_unpresentable_bus_is_refused();
    test_disk_count_over_frontend_budget_is_refused();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
