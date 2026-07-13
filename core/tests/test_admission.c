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

    memset(&cfg, 0, sizeof(cfg));
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

    memset(&cfg, 0, sizeof(cfg));
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

    memset(&cfg, 0, sizeof(cfg));
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

    memset(&cfg, 0, sizeof(cfg));
    cfg.vm_count = 0;

    r = hype_adm_check_memory(&cfg, 0, 0);
    CHECK_INT("zero VMs never overcommits", (int)HYPE_ADM_OK, (int)r.status);
}

/* ---- hype_adm_check_vcpus ---- */

static void test_vcpus_within_budget(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    memset(&cfg, 0, sizeof(cfg));
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 2, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 3, 512, "b.img");

    r = hype_adm_check_vcpus(&cfg, 8);
    CHECK_INT("5 vcpus on 8 cores is OK", (int)HYPE_ADM_OK, (int)r.status);
}

static void test_vcpus_overcommit(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    memset(&cfg, 0, sizeof(cfg));
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 4, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 5, 512, "b.img");

    r = hype_adm_check_vcpus(&cfg, 8);
    CHECK_INT("9 vcpus on 8 cores is rejected", (int)HYPE_ADM_ERR_VCPU_OVERCOMMIT, (int)r.status);
}

static void test_vcpus_exact_fit_is_ok(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    memset(&cfg, 0, sizeof(cfg));
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 8, 512, "a.img");

    r = hype_adm_check_vcpus(&cfg, 8);
    CHECK_INT("using exactly all cores is OK", (int)HYPE_ADM_OK, (int)r.status);
}

/* ---- hype_adm_check_cpu_set ---- */

static void test_cpu_set_no_explicit_sets_is_ok(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    memset(&cfg, 0, sizeof(cfg));
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 2, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 2, 512, "b.img");

    r = hype_adm_check_cpu_set(&cfg, 8);
    CHECK_INT("no explicit cpu_set anywhere is OK", (int)HYPE_ADM_OK, (int)r.status);
}

static void test_cpu_set_count_mismatch(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    memset(&cfg, 0, sizeof(cfg));
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

    memset(&cfg, 0, sizeof(cfg));
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

    memset(&cfg, 0, sizeof(cfg));
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

    memset(&cfg, 0, sizeof(cfg));
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

    memset(&cfg, 0, sizeof(cfg));
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

    memset(&cfg, 0, sizeof(cfg));
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");
    make_vm(&cfg.vms[1], "b", 1, 512, "b.img");

    r = hype_adm_check_target_disk(&cfg);
    CHECK_INT("distinct target_disk paths are OK", (int)HYPE_ADM_OK, (int)r.status);
}

static void test_target_disk_collision_same_kind(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    memset(&cfg, 0, sizeof(cfg));
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

    memset(&cfg, 0, sizeof(cfg));
    cfg.vm_count = 2;
    make_vm(&cfg.vms[0], "a", 1, 512, "SN-1234");
    cfg.vms[0].target_disk.kind = HYPE_CFG_DISK_PHYSICAL;
    make_vm(&cfg.vms[1], "b", 1, 512, "SN-1234");
    cfg.vms[1].target_disk.kind = HYPE_CFG_DISK_FILE;

    r = hype_adm_check_target_disk(&cfg);
    CHECK_INT("same string but different disk kind (file vs physical) is OK",
              (int)HYPE_ADM_OK, (int)r.status);
}

/* ---- hype_adm_check_net_peers ---- */

static void test_net_peers_valid_pairing_is_ok(void) {
    hype_cfg_t cfg;
    hype_adm_result_t r;

    memset(&cfg, 0, sizeof(cfg));
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

    memset(&cfg, 0, sizeof(cfg));
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

    memset(&cfg, 0, sizeof(cfg));
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

    memset(&cfg, 0, sizeof(cfg));
    cfg.vm_count = 1;
    make_vm(&cfg.vms[0], "a", 1, 512, "a.img");

    r = hype_adm_check_net_peers(&cfg);
    CHECK_INT("no net_peers anywhere is OK", (int)HYPE_ADM_OK, (int)r.status);
}

int main(void) {
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
    test_net_peers_valid_pairing_is_ok();
    test_net_peers_unknown_vm_rejected();
    test_net_peers_requires_nat_on_both_sides();
    test_net_peers_none_is_ok();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
