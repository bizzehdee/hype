#include <stdio.h>
#include <string.h>
#include "../cfg.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), (long long)(actual)); \
            failures++; \
        } \
    } while (0)

#define CHECK_STR(desc, expected, actual) \
    do { \
        if (strcmp((expected), (actual)) != 0) { \
            printf("FAIL: %s: expected \"%s\", got \"%s\"\n", (desc), (expected), (actual)); \
            failures++; \
        } \
    } while (0)

static hype_cfg_result_t parse_copy(const char *text, hype_cfg_t *out) {
    static char buf[8192];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return hype_cfg_parse(buf, out);
}

static void test_full_example_from_plan(void) {
    const char *cfg =
        "[vm.win11]\n"
        "vcpus = 4\n"
        "cpu_set = 4-7             ; explicit host core subset to pin to\n"
        "mem_mb = 8192\n"
        "boot = installer\n"
        "install_media = \\EFI\\hype\\win11.iso\n"
        "target_disk = file:\\hype\\disks\\win11.img\n"
        "target_disk_size_gb = 128\n"
        "firmware = uefi\n"
        "os_hint = windows\n"
        "net_mode = nat\n"
        "\n"
        "[vm.debian]\n"
        "vcpus = 2\n"
        "mem_mb = 4096\n"
        "boot = installer\n"
        "install_media = \\EFI\\hype\\debian-netinst.iso\n"
        "target_disk = physical:SN-WDC-1234567890\n"
        "firmware = uefi\n"
        "os_hint = linux\n"
        "net_mode = nat\n"
        "net_peers = freebsd\n"
        "\n"
        "[vm.freebsd]\n"
        "vcpus = 2\n"
        "mem_mb = 4096\n"
        "boot = installer\n"
        "install_media = \\EFI\\hype\\FreeBSD.iso\n"
        "target_disk = file:\\hype\\disks\\freebsd.img\n"
        "target_disk_size_gb = 64\n"
        "firmware = uefi\n"
        "os_hint = bsd\n"
        "net_mode = nat\n";
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy(cfg, &out);

    CHECK_INT("plan.md example parses OK", HYPE_CFG_OK, res.status);
    CHECK_INT("plan.md example has 3 VMs", 3, out.vm_count);

    CHECK_STR("vm0 name", "win11", out.vms[0].name);
    CHECK_INT("vm0 vcpus", 4, out.vms[0].vcpus);
    CHECK_INT("vm0 has_cpu_set", 1, out.vms[0].has_cpu_set);
    CHECK_INT("vm0 cpu_set_count", 4, out.vms[0].cpu_set_count);
    CHECK_INT("vm0 cpu_set[0]", 4, out.vms[0].cpu_set[0]);
    CHECK_INT("vm0 cpu_set[3]", 7, out.vms[0].cpu_set[3]);
    CHECK_INT("vm0 mem_mb", 8192, out.vms[0].mem_mb);
    CHECK_INT("vm0 boot", (int)HYPE_CFG_BOOT_INSTALLER, (int)out.vms[0].boot);
    CHECK_STR("vm0 install_media", "\\EFI\\hype\\win11.iso", out.vms[0].install_media);
    CHECK_INT("vm0 target_disk kind", (int)HYPE_CFG_DISK_FILE, (int)out.vms[0].target_disk.kind);
    CHECK_STR("vm0 target_disk path", "\\hype\\disks\\win11.img", out.vms[0].target_disk.path_or_id);
    CHECK_INT("vm0 target_disk_size_gb", 128, out.vms[0].target_disk_size_gb);
    CHECK_INT("vm0 firmware", (int)HYPE_CFG_FW_UEFI, (int)out.vms[0].firmware);
    CHECK_INT("vm0 os_hint", (int)HYPE_CFG_OS_WINDOWS, (int)out.vms[0].os_hint);
    CHECK_INT("vm0 net_mode", (int)HYPE_CFG_NET_NAT, (int)out.vms[0].net_mode);
    CHECK_INT("vm0 has no cpu_set-less default weirdness", 0, out.vms[0].net_peers_count);

    CHECK_STR("vm1 name", "debian", out.vms[1].name);
    CHECK_INT("vm1 target_disk kind physical", (int)HYPE_CFG_DISK_PHYSICAL, (int)out.vms[1].target_disk.kind);
    CHECK_STR("vm1 target_disk id", "SN-WDC-1234567890", out.vms[1].target_disk.path_or_id);
    CHECK_INT("vm1 net_peers_count", 1, out.vms[1].net_peers_count);
    CHECK_STR("vm1 net_peers[0]", "freebsd", out.vms[1].net_peers[0]);
    CHECK_INT("vm1 has_cpu_set is false (omitted)", 0, out.vms[1].has_cpu_set);

    CHECK_STR("vm2 name", "freebsd", out.vms[2].name);
    CHECK_INT("vm2 os_hint bsd", (int)HYPE_CFG_OS_BSD, (int)out.vms[2].os_hint);
}

static void test_cpu_set_comma_list(void) {
    const char *cfg =
        "[vm.a]\n"
        "vcpus = 3\n"
        "cpu_set = 0,2,4-6\n"
        "mem_mb = 1024\n"
        "boot = disk\n"
        "target_disk = file:x.img\n"
        "firmware = uefi\n"
        "os_hint = none\n";
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy(cfg, &out);

    CHECK_INT("comma+range cpu_set parses OK", HYPE_CFG_OK, res.status);
    CHECK_INT("comma+range cpu_set count", 5, out.vms[0].cpu_set_count);
    CHECK_INT("cpu_set[0]", 0, out.vms[0].cpu_set[0]);
    CHECK_INT("cpu_set[1]", 2, out.vms[0].cpu_set[1]);
    CHECK_INT("cpu_set[2]", 4, out.vms[0].cpu_set[2]);
    CHECK_INT("cpu_set[3]", 5, out.vms[0].cpu_set[3]);
    CHECK_INT("cpu_set[4]", 6, out.vms[0].cpu_set[4]);
}

static void test_boot_disk_no_install_media_required(void) {
    const char *cfg =
        "[vm.a]\n"
        "vcpus = 1\n"
        "mem_mb = 512\n"
        "boot = disk\n"
        "target_disk = file:x.img\n"
        "firmware = legacy\n"
        "os_hint = none\n";
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy(cfg, &out);

    CHECK_INT("boot=disk without install_media is valid", HYPE_CFG_OK, res.status);
    CHECK_INT("firmware legacy", (int)HYPE_CFG_FW_LEGACY, (int)out.vms[0].firmware);
}

/* ---- error cases ---- */

struct error_case {
    const char *desc;
    const char *cfg;
    hype_cfg_status_t expect;
};

static const struct error_case ERROR_CASES[] = {
    {"key before any section", "vcpus = 1\n", HYPE_CFG_ERR_KEY_BEFORE_SECTION},
    {"malformed section (no closing bracket)", "[vm.a\nvcpus=1\n", HYPE_CFG_ERR_SYNTAX},
    {"section not vm.*", "[bogus]\nvcpus=1\n", HYPE_CFG_ERR_SYNTAX},
    {"empty vm name", "[vm.]\nvcpus=1\n", HYPE_CFG_ERR_BAD_VALUE},
    {"duplicate vm name",
     "[vm.a]\nvcpus=1\nmem_mb=1\nboot=disk\ntarget_disk=file:x\nfirmware=uefi\nos_hint=none\n"
     "[vm.a]\nvcpus=1\nmem_mb=1\nboot=disk\ntarget_disk=file:x\nfirmware=uefi\nos_hint=none\n",
     HYPE_CFG_ERR_DUPLICATE_VM_NAME},
    {"key with no '='", "[vm.a]\nvcpus 1\n", HYPE_CFG_ERR_SYNTAX},
    {"empty key", "[vm.a]\n = 1\n", HYPE_CFG_ERR_SYNTAX},
    {"unknown key", "[vm.a]\nbogus = 1\n", HYPE_CFG_ERR_UNKNOWN_KEY},
    {"duplicate key", "[vm.a]\nvcpus=1\nvcpus=2\n", HYPE_CFG_ERR_DUPLICATE_KEY},
    {"vcpus zero", "[vm.a]\nvcpus=0\n", HYPE_CFG_ERR_BAD_VALUE},
    {"vcpus not a number", "[vm.a]\nvcpus=four\n", HYPE_CFG_ERR_BAD_VALUE},
    {"mem_mb zero", "[vm.a]\nvcpus=1\nmem_mb=0\n", HYPE_CFG_ERR_BAD_VALUE},
    {"boot bad value", "[vm.a]\nboot=maybe\n", HYPE_CFG_ERR_BAD_VALUE},
    {"target_disk bad prefix", "[vm.a]\ntarget_disk=nope:x\n", HYPE_CFG_ERR_BAD_VALUE},
    {"target_disk empty path", "[vm.a]\ntarget_disk=file:\n", HYPE_CFG_ERR_BAD_VALUE},
    {"firmware bad value", "[vm.a]\nfirmware=bios\n", HYPE_CFG_ERR_BAD_VALUE},
    {"os_hint bad value", "[vm.a]\nos_hint=macos\n", HYPE_CFG_ERR_BAD_VALUE},
    {"net_mode bad value", "[vm.a]\nnet_mode=bridge\n", HYPE_CFG_ERR_BAD_VALUE},
    {"target_disk_size_gb zero", "[vm.a]\ntarget_disk_size_gb=0\n", HYPE_CFG_ERR_BAD_VALUE},
    {"cpu_set empty", "[vm.a]\ncpu_set=\n", HYPE_CFG_ERR_BAD_VALUE},
    {"cpu_set inverted range", "[vm.a]\ncpu_set=7-4\n", HYPE_CFG_ERR_BAD_VALUE},
    {"cpu_set non-numeric", "[vm.a]\ncpu_set=x-y\n", HYPE_CFG_ERR_BAD_VALUE},
    {"cpu_set non-numeric single", "[vm.a]\ncpu_set=x\n", HYPE_CFG_ERR_BAD_VALUE},
    {"cpu_set duplicate core", "[vm.a]\ncpu_set=1,1\n", HYPE_CFG_ERR_BAD_VALUE},
    {"net_peers empty", "[vm.a]\nnet_peers=\n", HYPE_CFG_ERR_BAD_VALUE},
    {"net_peers empty piece", "[vm.a]\nnet_peers=a,,b\n", HYPE_CFG_ERR_BAD_VALUE},
    {"net_peers duplicate", "[vm.a]\nnet_peers=a,a\n", HYPE_CFG_ERR_BAD_VALUE},
    {"missing vcpus",
     "[vm.a]\nmem_mb=1\nboot=disk\ntarget_disk=file:x\nfirmware=uefi\nos_hint=none\n",
     HYPE_CFG_ERR_MISSING_REQUIRED},
    {"missing install_media when boot=installer",
     "[vm.a]\nvcpus=1\nmem_mb=1\nboot=installer\ntarget_disk=file:x\nfirmware=uefi\nos_hint=none\n",
     HYPE_CFG_ERR_MISSING_REQUIRED},
    {"missing target_disk",
     "[vm.a]\nvcpus=1\nmem_mb=1\nboot=disk\nfirmware=uefi\nos_hint=none\n",
     HYPE_CFG_ERR_MISSING_REQUIRED},
    {"missing firmware",
     "[vm.a]\nvcpus=1\nmem_mb=1\nboot=disk\ntarget_disk=file:x\nos_hint=none\n",
     HYPE_CFG_ERR_MISSING_REQUIRED},
    {"missing os_hint",
     "[vm.a]\nvcpus=1\nmem_mb=1\nboot=disk\ntarget_disk=file:x\nfirmware=uefi\n",
     HYPE_CFG_ERR_MISSING_REQUIRED},
    {"missing mem_mb",
     "[vm.a]\nvcpus=1\nboot=disk\ntarget_disk=file:x\nfirmware=uefi\nos_hint=none\n",
     HYPE_CFG_ERR_MISSING_REQUIRED},
    {"missing boot",
     "[vm.a]\nvcpus=1\nmem_mb=1\ntarget_disk=file:x\nfirmware=uefi\nos_hint=none\n",
     HYPE_CFG_ERR_MISSING_REQUIRED},
    {"duplicate key cpu_set", "[vm.a]\ncpu_set=1\ncpu_set=2\n", HYPE_CFG_ERR_DUPLICATE_KEY},
    {"duplicate key boot", "[vm.a]\nboot=disk\nboot=disk\n", HYPE_CFG_ERR_DUPLICATE_KEY},
    {"duplicate key install_media", "[vm.a]\ninstall_media=x\ninstall_media=y\n", HYPE_CFG_ERR_DUPLICATE_KEY},
    {"duplicate key target_disk", "[vm.a]\ntarget_disk=file:x\ntarget_disk=file:y\n", HYPE_CFG_ERR_DUPLICATE_KEY},
    {"duplicate key target_disk_size_gb", "[vm.a]\ntarget_disk_size_gb=1\ntarget_disk_size_gb=2\n",
     HYPE_CFG_ERR_DUPLICATE_KEY},
    {"duplicate key firmware", "[vm.a]\nfirmware=uefi\nfirmware=uefi\n", HYPE_CFG_ERR_DUPLICATE_KEY},
    {"duplicate key os_hint", "[vm.a]\nos_hint=none\nos_hint=none\n", HYPE_CFG_ERR_DUPLICATE_KEY},
    {"duplicate key net_mode", "[vm.a]\nnet_mode=none\nnet_mode=none\n", HYPE_CFG_ERR_DUPLICATE_KEY},
    {"duplicate key net_peers", "[vm.a]\nnet_peers=x\nnet_peers=y\n", HYPE_CFG_ERR_DUPLICATE_KEY},
    {"cpu_set range with non-numeric end", "[vm.a]\ncpu_set=1-y\n", HYPE_CFG_ERR_BAD_VALUE},
    {"cpu_set range end exceeds UINT32_MAX", "[vm.a]\ncpu_set=1-4294967296\n", HYPE_CFG_ERR_BAD_VALUE},
    {"cpu_set single value exceeds UINT32_MAX", "[vm.a]\ncpu_set=4294967296\n", HYPE_CFG_ERR_BAD_VALUE},
    {"cpu_set overlapping ranges cause duplicate", "[vm.a]\ncpu_set=1-3,2-4\n", HYPE_CFG_ERR_BAD_VALUE},
    {"cpu_set exceeds MAX_CPUS", "[vm.a]\ncpu_set=0-300\n", HYPE_CFG_ERR_TOO_MANY_ENTRIES},
    {"net_peers too many", "[vm.a]\nnet_peers=a,b,c,d,e,f,g,h,i\n", HYPE_CFG_ERR_TOO_MANY_ENTRIES},
    {"section header too short for '[' + ']'", "[\nvcpus=1\n", HYPE_CFG_ERR_SYNTAX},
};

static void test_error_cases(void) {
    unsigned long long i;
    for (i = 0; i < sizeof(ERROR_CASES) / sizeof(ERROR_CASES[0]); i++) {
        hype_cfg_t out;
        hype_cfg_result_t res = parse_copy(ERROR_CASES[i].cfg, &out);
        if (res.status != ERROR_CASES[i].expect) {
            printf("FAIL: %s: expected status %d, got %d\n",
                   ERROR_CASES[i].desc, (int)ERROR_CASES[i].expect, (int)res.status);
            failures++;
        }
    }
}

static void test_too_many_vms(void) {
    char cfg[8192] = "";
    char section[128];
    int i;
    hype_cfg_t out;
    hype_cfg_result_t res;

    for (i = 0; i < HYPE_CFG_MAX_VMS + 1; i++) {
        snprintf(section, sizeof(section),
                 "[vm.v%d]\nvcpus=1\nmem_mb=1\nboot=disk\ntarget_disk=file:x\nfirmware=uefi\nos_hint=none\n", i);
        strncat(cfg, section, sizeof(cfg) - strlen(cfg) - 1);
    }
    res = parse_copy(cfg, &out);
    CHECK_INT("too many VMs rejected", HYPE_CFG_ERR_TOO_MANY_VMS, res.status);
}

static void test_net_peers_multiple_unique(void) {
    const char *cfg =
        "[vm.a]\n"
        "vcpus = 1\nmem_mb = 1\nboot = disk\ntarget_disk = file:x\nfirmware = uefi\nos_hint = none\n"
        "net_peers = a,b,c\n";
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy(cfg, &out);

    CHECK_INT("net_peers with 3 unique names parses OK", HYPE_CFG_OK, res.status);
    CHECK_INT("net_peers count", 3, out.vms[0].net_peers_count);
    CHECK_STR("net_peers[0]", "a", out.vms[0].net_peers[0]);
    CHECK_STR("net_peers[1]", "b", out.vms[0].net_peers[1]);
    CHECK_STR("net_peers[2]", "c", out.vms[0].net_peers[2]);
}

static void test_net_peer_name_too_long(void) {
    char cfg[HYPE_CFG_NAME_MAX + 64] = "[vm.a]\nnet_peers = ";
    unsigned long long i;
    hype_cfg_t out;
    hype_cfg_result_t res;

    for (i = 0; i < HYPE_CFG_NAME_MAX; i++) {
        strncat(cfg, "z", sizeof(cfg) - strlen(cfg) - 1);
    }
    strncat(cfg, "\n", sizeof(cfg) - strlen(cfg) - 1);
    res = parse_copy(cfg, &out);
    CHECK_INT("overlong net_peers name rejected", HYPE_CFG_ERR_VALUE_TOO_LONG, res.status);
}

static void test_target_disk_path_too_long(void) {
    char cfg[HYPE_CFG_PATH_MAX + 64] = "[vm.a]\ntarget_disk = file:";
    unsigned long long i;
    hype_cfg_t out;
    hype_cfg_result_t res;

    for (i = 0; i < HYPE_CFG_PATH_MAX; i++) {
        strncat(cfg, "p", sizeof(cfg) - strlen(cfg) - 1);
    }
    strncat(cfg, "\n", sizeof(cfg) - strlen(cfg) - 1);
    res = parse_copy(cfg, &out);
    CHECK_INT("overlong target_disk path rejected", HYPE_CFG_ERR_VALUE_TOO_LONG, res.status);
}

static void test_no_trailing_newline(void) {
    const char *cfg = "[vm.a]\nvcpus = 1\nmem_mb = 1\nboot = disk\ntarget_disk = file:x\nfirmware = uefi\nos_hint = none";
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy(cfg, &out);

    CHECK_INT("config without a trailing newline on the last line still parses", HYPE_CFG_OK, res.status);
    CHECK_INT("last field survives EOF-without-newline", (int)HYPE_CFG_OS_NONE, (int)out.vms[0].os_hint);
}

static void test_value_too_long(void) {
    char cfg[HYPE_CFG_PATH_MAX + 64] = "[vm.a]\ninstall_media = ";
    unsigned long long i;
    hype_cfg_t out;
    hype_cfg_result_t res;

    for (i = 0; i < HYPE_CFG_PATH_MAX; i++) {
        strncat(cfg, "x", sizeof(cfg) - strlen(cfg) - 1);
    }
    strncat(cfg, "\n", sizeof(cfg) - strlen(cfg) - 1);
    res = parse_copy(cfg, &out);
    CHECK_INT("overlong install_media rejected", HYPE_CFG_ERR_VALUE_TOO_LONG, res.status);
}

static void test_vm_name_too_long(void) {
    char cfg[HYPE_CFG_NAME_MAX + 32] = "[vm.";
    unsigned long long i;
    hype_cfg_t out;
    hype_cfg_result_t res;

    for (i = 0; i < HYPE_CFG_NAME_MAX; i++) {
        strncat(cfg, "y", sizeof(cfg) - strlen(cfg) - 1);
    }
    strncat(cfg, "]\n", sizeof(cfg) - strlen(cfg) - 1);
    res = parse_copy(cfg, &out);
    CHECK_INT("overlong vm name rejected", HYPE_CFG_ERR_VALUE_TOO_LONG, res.status);
}

static void test_comments_and_blank_lines_ignored(void) {
    const char *cfg =
        "; a leading comment\n"
        "\n"
        "[vm.a]  ; trailing comment on section\n"
        "\n"
        "vcpus = 1   ; inline comment\n"
        "mem_mb = 512\n"
        "boot = disk\n"
        "target_disk = file:x.img\n"
        "firmware = uefi\n"
        "os_hint = none\n";
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy(cfg, &out);

    CHECK_INT("comments/blank lines don't break parsing", HYPE_CFG_OK, res.status);
    CHECK_INT("vcpus survives inline comment stripping", 1, out.vms[0].vcpus);
}

static void test_no_vms_is_valid(void) {
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy("; nothing here\n", &out);
    CHECK_INT("empty config (no VMs) is valid", HYPE_CFG_OK, res.status);
    CHECK_INT("empty config has zero VMs", 0, out.vm_count);
}

static void test_error_reports_line_number(void) {
    const char *cfg = "[vm.a]\nvcpus = 1\nmem_mb = bogus\n";
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy(cfg, &out);

    CHECK_INT("bad mem_mb status", HYPE_CFG_ERR_BAD_VALUE, res.status);
    CHECK_INT("bad mem_mb line number", 3, res.line);
}

int main(void) {
    test_full_example_from_plan();
    test_cpu_set_comma_list();
    test_boot_disk_no_install_media_required();
    test_error_cases();
    test_too_many_vms();
    test_value_too_long();
    test_vm_name_too_long();
    test_net_peers_multiple_unique();
    test_net_peer_name_too_long();
    test_target_disk_path_too_long();
    test_no_trailing_newline();
    test_comments_and_blank_lines_ignored();
    test_no_vms_is_valid();
    test_error_reports_line_number();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
