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

/* #331: 64-bit byte counts do not fit CHECK_INT's int comparison. */
#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
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
    {"empty vm name", "[vm.]\nvcpus=1\n", HYPE_CFG_ERR_BAD_VALUE},
    {"duplicate vm name",
     "[vm.a]\nvcpus=1\nmem_mb=1\nboot=disk\ntarget_disk=file:x\nfirmware=uefi\nos_hint=none\n"
     "[vm.a]\nvcpus=1\nmem_mb=1\nboot=disk\ntarget_disk=file:x\nfirmware=uefi\nos_hint=none\n",
     HYPE_CFG_ERR_DUPLICATE_VM_NAME},
    {"key with no '='", "[vm.a]\nvcpus 1\n", HYPE_CFG_ERR_SYNTAX},
    {"empty key", "[vm.a]\n = 1\n", HYPE_CFG_ERR_SYNTAX},
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
    {"duplicate key media_disk", "[vm.a]\nmedia_disk=SN-A\nmedia_disk=SN-B\n",
     HYPE_CFG_ERR_DUPLICATE_KEY},
    {"empty media_disk", "[vm.a]\nmedia_disk=\n", HYPE_CFG_ERR_BAD_VALUE},
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

static void test_physical_disk_qualifiers(void) {
    /* physical target with partition + allow_overwrite (M10-4/#124 inputs) */
    const char *cfg =
        "[vm.a]\n"
        "vcpus = 1\nmem_mb = 1\nboot = disk\nfirmware = uefi\nos_hint = linux\n"
        "target_disk = physical:SN-ABC-999\n"
        "partition = 3\n"
        "allow_overwrite = true\n";
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy(cfg, &out);
    CHECK_INT("physical qualifiers parse OK", HYPE_CFG_OK, res.status);
    CHECK_INT("kind physical", (int)HYPE_CFG_DISK_PHYSICAL, (int)out.vms[0].target_disk.kind);
    CHECK_STR("id", "SN-ABC-999", out.vms[0].target_disk.path_or_id);
    CHECK_INT("partition 3", 3, (int)out.vms[0].target_disk.partition);
    CHECK_INT("allow_overwrite true", 1, out.vms[0].target_disk.allow_overwrite);

    /* defaults: no partition/allow_overwrite -> whole disk, no override */
    {
        const char *cfg2 =
            "[vm.a]\nvcpus=1\nmem_mb=1\nboot=disk\nfirmware=uefi\nos_hint=linux\n"
            "target_disk = physical:SN-XYZ\n";
        hype_cfg_t o2;
        hype_cfg_result_t r2 = parse_copy(cfg2, &o2);
        CHECK_INT("defaults parse OK", HYPE_CFG_OK, r2.status);
        CHECK_INT("partition default 0 (whole)", 0, (int)o2.vms[0].target_disk.partition);
        CHECK_INT("allow_overwrite default 0", 0, o2.vms[0].target_disk.allow_overwrite);
    }

    /* partition = whole is explicit 0 */
    {
        const char *cfg3 =
            "[vm.a]\nvcpus=1\nmem_mb=1\nboot=disk\nfirmware=uefi\nos_hint=linux\n"
            "target_disk = physical:SN-XYZ\npartition = whole\n";
        hype_cfg_t o3;
        hype_cfg_result_t r3 = parse_copy(cfg3, &o3);
        CHECK_INT("partition=whole OK", HYPE_CFG_OK, r3.status);
        CHECK_INT("partition whole -> 0", 0, (int)o3.vms[0].target_disk.partition);
    }

    /* bad values rejected */
    {
        hype_cfg_t o4;
        CHECK_INT("partition 0 rejected (1-based)", HYPE_CFG_ERR_BAD_VALUE,
                  parse_copy("[vm.a]\ntarget_disk=physical:x\npartition=0\n", &o4).status);
        CHECK_INT("allow_overwrite bad rejected", HYPE_CFG_ERR_BAD_VALUE,
                  parse_copy("[vm.a]\nallow_overwrite=maybe\n", &o4).status);
        CHECK_INT("duplicate partition rejected", HYPE_CFG_ERR_DUPLICATE_KEY,
                  parse_copy("[vm.a]\npartition=1\npartition=2\n", &o4).status);
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


/*
 * #290: guest-RAM resolution. The bug being fixed was not a bad number -- it was
 * that mem_mb was validated, echoed, and then discarded, so the operator's log
 * read as confirmation of a setting with no effect. These pin both the value AND
 * the status, because the status is what makes "applied" distinguishable from
 * "ignored" in a log.
 */
static void test_resolve_mem_mb(void) {
    unsigned mb;

    /* No config value -> built-in default, reported as such. */
    CHECK_INT("no cfg -> DEFAULTED", HYPE_CFG_RAM_DEFAULTED,
              (int)hype_cfg_resolve_mem_mb(0u, 2048u, 128u, 3072u, &mb));
    CHECK_INT("no cfg -> default applied", 2048, (int)mb);

    /* In-range config value -> used exactly, which is the whole point. */
    CHECK_INT("in range -> APPLIED", HYPE_CFG_RAM_APPLIED,
              (int)hype_cfg_resolve_mem_mb(512u, 2048u, 128u, 3072u, &mb));
    CHECK_INT("in range -> 512 applied", 512, (int)mb);

    /* Boundaries are inclusive: exactly min and exactly max are APPLIED, not clamped. */
    CHECK_INT("exactly min -> APPLIED", HYPE_CFG_RAM_APPLIED,
              (int)hype_cfg_resolve_mem_mb(128u, 2048u, 128u, 3072u, &mb));
    CHECK_INT("exactly min value", 128, (int)mb);
    CHECK_INT("exactly max -> APPLIED", HYPE_CFG_RAM_APPLIED,
              (int)hype_cfg_resolve_mem_mb(3072u, 2048u, 128u, 3072u, &mb));
    CHECK_INT("exactly max value", 3072, (int)mb);

    /* Out of range clamps, and SAYS it clamped -- silently honouring 8 MB would
     * hand the operator a guest that dies in its own boot. */
    CHECK_INT("too small -> CLAMPED_LOW", HYPE_CFG_RAM_CLAMPED_LOW,
              (int)hype_cfg_resolve_mem_mb(8u, 2048u, 128u, 3072u, &mb));
    CHECK_INT("too small -> raised to min", 128, (int)mb);
    CHECK_INT("too big -> CLAMPED_HIGH", HYPE_CFG_RAM_CLAMPED_HIGH,
              (int)hype_cfg_resolve_mem_mb(65536u, 2048u, 128u, 3072u, &mb));
    CHECK_INT("too big -> lowered to max", 3072, (int)mb);

    /* The DEFAULT is clamped too. -DHYPE_FW_1_GUEST_RAM_MB=N can set it to
     * anything, and a default past the platform ceiling is still an overrun. */
    CHECK_INT("oversized default still reported DEFAULTED", HYPE_CFG_RAM_DEFAULTED,
              (int)hype_cfg_resolve_mem_mb(0u, 99999u, 128u, 3072u, &mb));
    CHECK_INT("oversized default clamped to max", 3072, (int)mb);
    CHECK_INT("undersized default clamped to min", HYPE_CFG_RAM_DEFAULTED,
              (int)hype_cfg_resolve_mem_mb(0u, 1u, 128u, 3072u, &mb));
    CHECK_INT("undersized default value", 128, (int)mb);

    /* Inverted limits: the floor wins. A too-small guest fails visibly at its own
     * boot; one sized past the address-space hole corrupts what lives above it. */
    CHECK_INT("inverted limits -> floor wins", 512,
              (hype_cfg_resolve_mem_mb(4096u, 2048u, 512u, 128u, &mb), (int)mb));

    /* NULL out_mb must not deref -- callers that only want the status. */
    CHECK_INT("NULL out_mb still returns a status", HYPE_CFG_RAM_APPLIED,
              (int)hype_cfg_resolve_mem_mb(512u, 2048u, 128u, 3072u, 0));

    CHECK_STR("status string, applied", "from hype.cfg",
              hype_cfg_ram_status_str(HYPE_CFG_RAM_APPLIED));
}

static void test_size_gb_to_bytes(void) {
    /*
     * #331: target_disk_size_gb is GiB, not decimal GB -- the same unit
     * tools/make-disk-image.sh allocates in, so a config and the image it describes agree. That
     * unit choice is the whole point of pinning this: a GB/GiB mix-up would make every
     * correctly-sized image look 7% wrong, which is exactly the kind of warning an operator
     * learns to ignore.
     */
    CHECK_HEX("1 GiB", 1073741824ull, hype_cfg_size_gb_to_bytes(1));
    CHECK_HEX("64 GiB", 68719476736ull, hype_cfg_size_gb_to_bytes(64));
    CHECK_HEX("128 GiB (the spec's own example)", 137438953472ull, hype_cfg_size_gb_to_bytes(128));
    /* NOT the decimal-GB answer for the same input -- the mistake this guards. */
    CHECK_HEX("64 GiB is not 64e9", 1, hype_cfg_size_gb_to_bytes(64) != 64000000000ull);
    /* 0 means "not declared"; the parser already rejects an explicit 0. */
    CHECK_HEX("0 declares nothing", 0ull, hype_cfg_size_gb_to_bytes(0));
    /* A large value must not wrap: 4 TiB is a plausible modern disk. */
    CHECK_HEX("4096 GiB does not wrap", 4398046511104ull, hype_cfg_size_gb_to_bytes(4096));
}

/* #323: which host drive the media lives on -- optional, so its absence must stay the default. */
static void test_media_disk(void) {
    const char *with =
        "[vm.a]\n"
        "vcpus = 1\n"
        "mem_mb = 512\n"
        "boot = installer\n"
        "install_media = \\iso\\alpine.iso\n"
        "media_disk = SN-SAMSUNG-980-1TB\n"
        "target_disk = file:\\hype\\disks\\a.img\n"
        "firmware = uefi\n"
        "os_hint = linux\n";
    const char *without =
        "[vm.a]\n"
        "vcpus = 1\n"
        "mem_mb = 512\n"
        "boot = installer\n"
        "install_media = \\iso\\alpine.iso\n"
        "target_disk = file:\\hype\\disks\\a.img\n"
        "firmware = uefi\n"
        "os_hint = linux\n";
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy(with, &out);

    CHECK_INT("media_disk parses OK", HYPE_CFG_OK, res.status);
    CHECK_INT("media_disk sets has_media_disk", 1, out.vms[0].has_media_disk);
    CHECK_STR("media_disk value", "SN-SAMSUNG-980-1TB", out.vms[0].media_disk);

    res = parse_copy(without, &out);
    CHECK_INT("omitting media_disk still parses OK", HYPE_CFG_OK, res.status);
    CHECK_INT("omitted media_disk leaves has_media_disk clear", 0, out.vms[0].has_media_disk);
    CHECK_STR("omitted media_disk leaves the value empty", "", out.vms[0].media_disk);
}

static void test_media_disk_too_long(void) {
    char cfg[HYPE_CFG_PATH_MAX + 128];
    hype_cfg_t out;
    hype_cfg_result_t res;
    size_t i, n;

    n = (size_t)snprintf(cfg, sizeof(cfg), "[vm.a]\nvcpus=1\nmem_mb=512\nmedia_disk=");
    for (i = 0; i < HYPE_CFG_PATH_MAX; i++) {
        cfg[n + i] = 'S';
    }
    cfg[n + i] = '\n';
    cfg[n + i + 1] = '\0';

    res = parse_copy(cfg, &out);
    CHECK_INT("an over-long media_disk is rejected", HYPE_CFG_ERR_VALUE_TOO_LONG, res.status);
}


/* ---- #222 (CONFIG-2, spec §4.1): unknown keys and sections are RETAINED, not fatal ---- */

static const char *retained_text(const hype_cfg_t *c, unsigned int i) {
    return (i < c->retained_count) ? c->retained[i].text : "<none>";
}

static void test_unknown_key_is_retained_not_fatal(void) {
    const char *cfg =
        "[vm.a]\n"
        "vcpus = 1\n"
        "mem_mb = 512\n"
        "boot = disk\n"
        "target_disk = file:a.img\n"
        "firmware = uefi\n"
        "os_hint = linux\n"
        "future_key = whatever   ; a key only a newer hype knows\n";
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy(cfg, &out);

    CHECK_INT("an unknown key no longer fails the parse", HYPE_CFG_OK, res.status);
    CHECK_INT("the VM still loaded", 1, out.vm_count);
    CHECK_INT("the unknown line was counted", 1, out.unknown_count);
    CHECK_INT("the unknown line was retained", 1, out.retained_count);
    /* Retained VERBATIM, comment included -- a serializer must reproduce the operator's line. */
    CHECK_STR("retained verbatim, comment included",
              "future_key = whatever   ; a key only a newer hype knows", retained_text(&out, 0));
    CHECK_INT("retained line is attached to its section", 0, out.retained[0].section);
    CHECK_INT("no overflow", 0, out.retained_overflow);
}

static void test_unknown_section_and_its_keys_are_retained(void) {
    /* Exactly the forward-compatibility case: a [nic.*] section an older hype cannot model. */
    const char *cfg =
        "[vm.a]\n"
        "vcpus = 1\n"
        "mem_mb = 512\n"
        "boot = disk\n"
        "target_disk = file:a.img\n"
        "firmware = uefi\n"
        "os_hint = linux\n"
        "\n"
        "[nic.net0]\n"
        "mode = nat\n"
        "mac = 52:54:00:12:34:56\n";
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy(cfg, &out);

    CHECK_INT("an unknown section no longer fails the parse", HYPE_CFG_OK, res.status);
    CHECK_INT("the VM still loaded", 1, out.vm_count);
    CHECK_INT("two sections recorded in file order", 2, out.section_count);
    CHECK_INT("section 0 is the VM", (int)HYPE_CFG_SECTION_VM, (int)out.sections[0].kind);
    CHECK_STR("section 0 names the VM", "a", out.sections[0].name);
    CHECK_INT("section 0 points at vms[0]", 0, out.sections[0].index);
    CHECK_INT("section 1 is unknown", (int)HYPE_CFG_SECTION_UNKNOWN, (int)out.sections[1].kind);
    CHECK_STR("the unknown header is kept verbatim", "[nic.net0]", out.sections[1].raw);
    /* Both keys inside it survive, attached to that section rather than to the VM. */
    CHECK_INT("both keys inside it retained", 2, out.retained_count);
    CHECK_STR("first retained key", "mode = nat", retained_text(&out, 0));
    CHECK_STR("second retained key", "mac = 52:54:00:12:34:56", retained_text(&out, 1));
    CHECK_INT("attached to the unknown section", 1, out.retained[0].section);
    CHECK_INT("attached to the unknown section", 1, out.retained[1].section);
}

static void test_comments_are_retained_blank_lines_are_not(void) {
    const char *cfg =
        "; a header comment before any section\n"
        "\n"
        "[vm.a]\n"
        "vcpus = 1\n"
        "\n"
        "; why this VM has so little RAM\n"
        "mem_mb = 512\n"
        "boot = disk\n"
        "target_disk = file:a.img\n"
        "firmware = uefi\n"
        "os_hint = linux\n";
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy(cfg, &out);

    CHECK_INT("parses OK", HYPE_CFG_OK, res.status);
    CHECK_INT("both comments retained, neither blank line", 2, out.retained_count);
    CHECK_STR("comment kept verbatim", "; a header comment before any section",
              retained_text(&out, 0));
    /* Before any section header: -1, so a serializer can emit it above everything. */
    CHECK_INT("pre-section comment belongs to no section", -1, out.retained[0].section);
    CHECK_STR("in-section comment kept", "; why this VM has so little RAM", retained_text(&out, 1));
    CHECK_INT("in-section comment attached to the VM", 0, out.retained[1].section);
    CHECK_INT("comments are not counted as unknown keys", 0, out.unknown_count);
}

static void test_key_before_any_section_is_still_fatal(void) {
    /* Deliberately NOT relaxed: a key belonging to nothing is a typo'd header far more often than a
     * forward-compatible extension, and silently retaining it would hide the typo. */
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy("vcpus = 1\n[vm.a]\n", &out);

    CHECK_INT("a key before any section is still rejected", HYPE_CFG_ERR_KEY_BEFORE_SECTION,
              res.status);
}

static void test_retention_overflow_is_flagged_not_silent(void) {
    /* A serializer must be able to tell that it did NOT capture everything, or a write-back would
     * delete the operator's lines. Parsing still succeeds: a comment must never stop a boot. */
    char cfg[8192];
    hype_cfg_t out;
    hype_cfg_result_t res;
    int n;
    unsigned int i;

    n = snprintf(cfg, sizeof(cfg),
                 "[vm.a]\nvcpus=1\nmem_mb=512\nboot=disk\ntarget_disk=file:a.img\n"
                 "firmware=uefi\nos_hint=linux\n");
    for (i = 0; i < HYPE_CFG_MAX_RETAINED + 5u; i++) {
        n += snprintf(cfg + n, sizeof(cfg) - (size_t)n, "; filler comment %u\n", i);
    }
    res = parse_copy(cfg, &out);

    CHECK_INT("overflowing retention does not fail the parse", HYPE_CFG_OK, res.status);
    CHECK_INT("the VM still loaded", 1, out.vm_count);
    CHECK_INT("retention is capped", HYPE_CFG_MAX_RETAINED, (int)out.retained_count);
    CHECK_INT("and the loss is FLAGGED", 1, out.retained_overflow);
}

static void test_overlong_line_is_flagged_not_truncated(void) {
    /* A truncated line written back would corrupt the file, so it is dropped AND flagged rather
     * than kept in part. */
    char cfg[HYPE_CFG_LINE_MAX + 512];
    hype_cfg_t out;
    hype_cfg_result_t res;
    int n;
    unsigned int i;

    n = snprintf(cfg, sizeof(cfg),
                 "[vm.a]\nvcpus=1\nmem_mb=512\nboot=disk\ntarget_disk=file:a.img\n"
                 "firmware=uefi\nos_hint=linux\n;");
    for (i = 0; i < HYPE_CFG_LINE_MAX + 10u; i++) {
        cfg[n++] = 'x';
    }
    cfg[n++] = '\n';
    cfg[n] = '\0';
    res = parse_copy(cfg, &out);

    CHECK_INT("an over-long comment does not fail the parse", HYPE_CFG_OK, res.status);
    CHECK_INT("it is not retained in truncated form", 0, (int)out.retained_count);
    CHECK_INT("and the loss is FLAGGED", 1, out.retained_overflow);
}


/* ---- #222 (CONFIG-2, spec §5.3): [disk.<id>] named devices ---- */

static const char *VM_A =
    "[vm.a]\n"
    "vcpus = 1\n"
    "mem_mb = 512\n"
    "boot = disk\n"
    "target_disk = file:a.img\n"
    "firmware = uefi\n"
    "os_hint = linux\n";

static void test_disk_section_all_keys(void) {
    char cfg[2048];
    hype_cfg_t out;
    hype_cfg_result_t res;

    snprintf(cfg, sizeof(cfg), "%s%s", VM_A,
             "[disk.data1]\n"
             "type = disk\n"
             "backing = file\n"
             "path = \\hype\\disks\\data1.qcow2\n"
             "source_disk = SN-SAMSUNG-980\n"
             "format = qcow2\n"
             "size_gb = 512\n"
             "bus = nvme\n"
             "read_only = false\n");
    res = parse_copy(cfg, &out);

    CHECK_INT("a [disk.*] section parses", HYPE_CFG_OK, res.status);
    CHECK_INT("one disk", 1, out.disk_count);
    CHECK_INT("none skipped", 0, out.skipped_disks);
    CHECK_INT("nothing retained as unknown", 0, out.unknown_count);
    CHECK_STR("id", "data1", out.disks[0].id);
    CHECK_INT("type disk", (int)HYPE_CFG_DISK_TYPE_DISK, (int)out.disks[0].type);
    CHECK_INT("backing file", (int)HYPE_CFG_BACKING_FILE, (int)out.disks[0].backing);
    CHECK_STR("path", "\\hype\\disks\\data1.qcow2", out.disks[0].path);
    CHECK_STR("source_disk", "SN-SAMSUNG-980", out.disks[0].source_disk);
    CHECK_INT("format qcow2", (int)HYPE_CFG_FORMAT_QCOW2, (int)out.disks[0].format);
    CHECK_INT("size_gb", 512, out.disks[0].size_gb);
    CHECK_INT("bus nvme", (int)HYPE_CFG_BUS_NVME, (int)out.disks[0].bus);
    CHECK_INT("read_only false", 0, out.disks[0].read_only);
    /* The section table must place it after the VM, in file order. */
    CHECK_INT("two sections", 2, out.section_count);
    CHECK_INT("section 1 is the disk", (int)HYPE_CFG_SECTION_DISK, (int)out.sections[1].kind);
    CHECK_INT("section 1 points at disks[0]", 0, out.sections[1].index);
}

static void test_disk_defaults(void) {
    char cfg[1024];
    hype_cfg_t out;
    hype_cfg_result_t res;

    snprintf(cfg, sizeof(cfg), "%s%s", VM_A, "[disk.sys]\nbacking = file\npath = sys.img\n");
    res = parse_copy(cfg, &out);

    CHECK_INT("parses", HYPE_CFG_OK, res.status);
    CHECK_INT("type defaults to disk", (int)HYPE_CFG_DISK_TYPE_DISK, (int)out.disks[0].type);
    CHECK_INT("format defaults to raw", (int)HYPE_CFG_FORMAT_RAW, (int)out.disks[0].format);
    CHECK_INT("partition defaults to whole (0)", 0, out.disks[0].partition);
    CHECK_INT("read_only defaults to false", 0, out.disks[0].read_only);
    /* NOT collapsed to virtio-blk: §5.6 derives it from the owning VM's os_hint, which a
     * [disk.*] cannot know at parse time. Pinning it here would silently give a Windows VM a bus it
     * cannot boot from. */
    CHECK_INT("bus stays DEFAULT for later os_hint resolution", (int)HYPE_CFG_BUS_DEFAULT,
              (int)out.disks[0].bus);
}

static void test_disk_cdrom_is_always_read_only(void) {
    char cfg[1024];
    hype_cfg_t out;
    hype_cfg_result_t res;

    /* read_only=false is deliberately IGNORED for a cdrom: a writable ISO is not a thing, and
     * honouring it would arm a write path against installer media. */
    snprintf(cfg, sizeof(cfg), "%s%s", VM_A,
             "[disk.inst]\ntype = cdrom\npath = \\iso\\win11.iso\nread_only = false\n");
    res = parse_copy(cfg, &out);

    CHECK_INT("parses", HYPE_CFG_OK, res.status);
    CHECK_INT("one disk", 1, out.disk_count);
    CHECK_INT("cdrom", (int)HYPE_CFG_DISK_TYPE_CDROM, (int)out.disks[0].type);
    CHECK_INT("read_only forced on despite read_only=false", 1, out.disks[0].read_only);
    CHECK_INT("backing defaults to file for a cdrom", (int)HYPE_CFG_BACKING_FILE,
              (int)out.disks[0].backing);
}

static void test_disk_physical_requires_id_match(void) {
    char cfg[1024];
    hype_cfg_t out;
    hype_cfg_result_t res;

    /* Without id_match the phys_guard has nothing to match the enumerated drive against, so the
     * device is unusable -- and "matches any drive" is the worst possible reading of the omission. */
    snprintf(cfg, sizeof(cfg), "%s%s", VM_A, "[disk.raw]\nbacking = physical\n");
    res = parse_copy(cfg, &out);

    CHECK_INT("the config still loads (§4.3)", HYPE_CFG_OK, res.status);
    CHECK_INT("the VM survives", 1, out.vm_count);
    CHECK_INT("the bad disk is dropped", 0, out.disk_count);
    CHECK_INT("and the drop is COUNTED, not silent", 1, out.skipped_disks);
}

static void test_disk_physical_rejects_path(void) {
    char cfg[1024];
    hype_cfg_t out;
    hype_cfg_result_t res;

    snprintf(cfg, sizeof(cfg), "%s%s", VM_A,
             "[disk.raw]\nbacking = physical\nid_match = SN-1\npath = nonsense.img\n");
    res = parse_copy(cfg, &out);
    CHECK_INT("a path on a physical device is contradictory -- dropped", 0, out.disk_count);
    CHECK_INT("counted", 1, out.skipped_disks);
    CHECK_INT("config still loads", HYPE_CFG_OK, res.status);
}

static void test_disk_partition_whole_and_numeric(void) {
    char cfg[1024];
    hype_cfg_t out;
    hype_cfg_result_t res;

    snprintf(cfg, sizeof(cfg), "%s%s", VM_A,
             "[disk.p]\nbacking = physical\nid_match = SN-1\npartition = 3\n"
             "[disk.w]\nbacking = physical\nid_match = SN-2\npartition = whole\n");
    res = parse_copy(cfg, &out);

    CHECK_INT("parses", HYPE_CFG_OK, res.status);
    CHECK_INT("two disks", 2, out.disk_count);
    CHECK_INT("numeric partition kept 1-based", 3, out.disks[0].partition);
    CHECK_INT("`whole` means 0, matching target_disk", 0, out.disks[1].partition);
}

static void test_disk_bad_and_good_compact_correctly(void) {
    char cfg[1536];
    hype_cfg_t out;
    hype_cfg_result_t res;

    /* The ordering that breaks a naive compaction: the FIRST disk is dropped, so the good one moves
     * into slot 0 and the bad section's stale index would alias it. */
    snprintf(cfg, sizeof(cfg), "%s%s", VM_A,
             "[disk.bad]\nbacking = physical\n"
             "[disk.good]\nbacking = file\npath = good.img\n");
    res = parse_copy(cfg, &out);

    CHECK_INT("config loads", HYPE_CFG_OK, res.status);
    CHECK_INT("only the good disk remains", 1, out.disk_count);
    CHECK_INT("one skipped", 1, out.skipped_disks);
    CHECK_STR("and it is the RIGHT one (dense, not a corpse)", "good", out.disks[0].id);
    /* Sections: 0=vm, 1=bad disk (now -1), 2=good disk (now 0). The bad one must NOT point at 0. */
    CHECK_INT("three sections", 3, out.section_count);
    CHECK_INT("the dropped disk's section points at nothing", -1, out.sections[1].index);
    CHECK_INT("the surviving disk's section was retargeted", 0, out.sections[2].index);
}

static void test_disk_duplicate_id_and_unknown_key(void) {
    char cfg[1536];
    hype_cfg_t out;
    hype_cfg_result_t res;

    snprintf(cfg, sizeof(cfg), "%s%s", VM_A,
             "[disk.d]\nbacking = file\npath = a.img\n[disk.d]\nbacking = file\npath = b.img\n");
    res = parse_copy(cfg, &out);
    CHECK_INT("a duplicate disk id is fatal -- `disks = d` would be ambiguous",
              HYPE_CFG_ERR_DUPLICATE_VM_NAME, res.status);

    snprintf(cfg, sizeof(cfg), "%s%s", VM_A,
             "[disk.d]\nbacking = file\npath = a.img\ntrim_support = yes\n");
    res = parse_copy(cfg, &out);
    CHECK_INT("an unknown key inside a disk section is retained, not fatal", HYPE_CFG_OK, res.status);
    CHECK_INT("the disk still loads", 1, out.disk_count);
    CHECK_INT("the line is retained", 1, out.retained_count);
    CHECK_STR("verbatim", "trim_support = yes", out.retained[0].text);
}

static void test_disk_bad_values_and_duplicates(void) {
    struct { const char *desc; const char *tail; hype_cfg_status_t want; } cases[] = {
        {"bad type", "[disk.d]\ntype = tape\n", HYPE_CFG_ERR_BAD_VALUE},
        {"bad backing", "[disk.d]\nbacking = magic\n", HYPE_CFG_ERR_BAD_VALUE},
        {"bad format", "[disk.d]\nformat = vmdk\n", HYPE_CFG_ERR_BAD_VALUE},
        {"bad bus", "[disk.d]\nbus = scsi\n", HYPE_CFG_ERR_BAD_VALUE},
        {"bad read_only", "[disk.d]\nread_only = maybe\n", HYPE_CFG_ERR_BAD_VALUE},
        {"zero size_gb", "[disk.d]\nsize_gb = 0\n", HYPE_CFG_ERR_BAD_VALUE},
        {"zero partition", "[disk.d]\npartition = 0\n", HYPE_CFG_ERR_BAD_VALUE},
        {"empty path", "[disk.d]\npath =\n", HYPE_CFG_ERR_BAD_VALUE},
        {"duplicate backing", "[disk.d]\nbacking = file\nbacking = file\n",
         HYPE_CFG_ERR_DUPLICATE_KEY},
        {"duplicate path", "[disk.d]\npath = a\npath = b\n", HYPE_CFG_ERR_DUPLICATE_KEY},
        {"duplicate bus", "[disk.d]\nbus = nvme\nbus = nvme\n", HYPE_CFG_ERR_DUPLICATE_KEY},
        {"duplicate source_disk", "[disk.d]\nsource_disk = a\nsource_disk = b\n",
         HYPE_CFG_ERR_DUPLICATE_KEY},
        {"empty disk id", "[disk.]\nbacking = file\n", HYPE_CFG_ERR_BAD_VALUE}
    };
    unsigned int i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char cfg[1536];
        hype_cfg_t out;
        hype_cfg_result_t res;
        snprintf(cfg, sizeof(cfg), "%s%s", VM_A, cases[i].tail);
        res = parse_copy(cfg, &out);
        CHECK_INT(cases[i].desc, (int)cases[i].want, (int)res.status);
    }
}

static void test_disk_capacity(void) {
    char cfg[8192];
    hype_cfg_t out;
    hype_cfg_result_t res;
    int n;
    unsigned int i;

    n = snprintf(cfg, sizeof(cfg), "%s", VM_A);
    for (i = 0; i < HYPE_CFG_MAX_DISKS + 1u; i++) {
        n += snprintf(cfg + n, sizeof(cfg) - (size_t)n,
                      "[disk.d%u]\nbacking = file\npath = d%u.img\n", i, i);
    }
    res = parse_copy(cfg, &out);
    CHECK_INT("one disk too many is reported, not silently dropped",
              HYPE_CFG_ERR_TOO_MANY_ENTRIES, res.status);
}


static void test_disk_remaining_domains(void) {
    struct { const char *desc; const char *tail; hype_cfg_status_t want; } cases[] = {
        {"duplicate type", "[disk.d]\ntype = disk\ntype = disk\n", HYPE_CFG_ERR_DUPLICATE_KEY},
        {"duplicate format", "[disk.d]\nformat = raw\nformat = raw\n", HYPE_CFG_ERR_DUPLICATE_KEY},
        {"duplicate size_gb", "[disk.d]\nsize_gb = 1\nsize_gb = 2\n", HYPE_CFG_ERR_DUPLICATE_KEY},
        {"duplicate partition", "[disk.d]\npartition = 1\npartition = 2\n",
         HYPE_CFG_ERR_DUPLICATE_KEY},
        {"duplicate read_only", "[disk.d]\nread_only = yes\nread_only = no\n",
         HYPE_CFG_ERR_DUPLICATE_KEY},
        {"duplicate allow_overwrite", "[disk.d]\nallow_overwrite = yes\nallow_overwrite = no\n",
         HYPE_CFG_ERR_DUPLICATE_KEY},
        {"duplicate id_match", "[disk.d]\nid_match = a\nid_match = b\n", HYPE_CFG_ERR_DUPLICATE_KEY},
        {"bad allow_overwrite", "[disk.d]\nallow_overwrite = perhaps\n", HYPE_CFG_ERR_BAD_VALUE},
        {"non-numeric partition", "[disk.d]\npartition = half\n", HYPE_CFG_ERR_BAD_VALUE},
        {"non-numeric size_gb", "[disk.d]\nsize_gb = big\n", HYPE_CFG_ERR_BAD_VALUE},
        {"empty source_disk", "[disk.d]\nsource_disk =\n", HYPE_CFG_ERR_BAD_VALUE},
        {"empty id_match", "[disk.d]\nid_match =\n", HYPE_CFG_ERR_BAD_VALUE}
    };
    unsigned int i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char cfg[1536];
        hype_cfg_t out;
        hype_cfg_result_t res;
        snprintf(cfg, sizeof(cfg), "%s%s", VM_A, cases[i].tail);
        res = parse_copy(cfg, &out);
        CHECK_INT(cases[i].desc, (int)cases[i].want, (int)res.status);
    }
}

static void test_disk_allow_overwrite_and_ro_disk(void) {
    char cfg[1024];
    hype_cfg_t out;
    hype_cfg_result_t res;

    snprintf(cfg, sizeof(cfg), "%s%s", VM_A,
             "[disk.p]\nbacking = physical\nid_match = SN-9\nallow_overwrite = yes\n"
             "[disk.r]\nbacking = file\npath = r.img\nread_only = yes\n");
    res = parse_copy(cfg, &out);

    CHECK_INT("parses", HYPE_CFG_OK, res.status);
    CHECK_INT("two disks", 2, out.disk_count);
    CHECK_INT("allow_overwrite set", 1, out.disks[0].allow_overwrite);
    CHECK_STR("id_match kept", "SN-9", out.disks[0].id_match);
    /* A read-only HARD disk is legitimate (a shared reference image), unlike a writable cdrom. */
    CHECK_INT("read_only honoured on a disk", 1, out.disks[1].read_only);
}

static void test_disk_overlong_id_and_path(void) {
    char cfg[2048];
    hype_cfg_t out;
    hype_cfg_result_t res;
    int n;
    unsigned int i;

    n = snprintf(cfg, sizeof(cfg), "%s[disk.", VM_A);
    for (i = 0; i < HYPE_CFG_NAME_MAX + 4u; i++) cfg[n++] = 'i';
    n += snprintf(cfg + n, sizeof(cfg) - (size_t)n, "]\nbacking = file\npath = a.img\n");
    cfg[n] = '\0';
    res = parse_copy(cfg, &out);
    CHECK_INT("an over-long disk id is rejected", HYPE_CFG_ERR_VALUE_TOO_LONG, res.status);

    n = snprintf(cfg, sizeof(cfg), "%s[disk.d]\nbacking = file\npath = ", VM_A);
    for (i = 0; i < HYPE_CFG_PATH_MAX + 4u; i++) cfg[n++] = 'p';
    cfg[n++] = '\n';
    cfg[n] = '\0';
    res = parse_copy(cfg, &out);
    CHECK_INT("an over-long path is rejected", HYPE_CFG_ERR_VALUE_TOO_LONG, res.status);
}

static void test_too_many_sections_flags_overflow(void) {
    /* Section capacity is smaller than VM+disk capacity combined, so it can be reached with valid
     * sections. Losing section ORDER breaks write-back, so it must set the same flag. */
    char cfg[8192];
    hype_cfg_t out;
    hype_cfg_result_t res;
    int n = 0;
    unsigned int i;

    for (i = 0; i < HYPE_CFG_MAX_SECTIONS + 2u; i++) {
        n += snprintf(cfg + n, sizeof(cfg) - (size_t)n, "[future.s%u]\nk = v\n", i);
    }
    res = parse_copy(cfg, &out);
    CHECK_INT("unknown sections past capacity do not fail the parse", HYPE_CFG_OK, res.status);
    CHECK_INT("but the loss is flagged", 1, out.retained_overflow);
}


int main(void) {
    test_size_gb_to_bytes();
    test_resolve_mem_mb();
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
    test_physical_disk_qualifiers();
    test_no_trailing_newline();
    test_comments_and_blank_lines_ignored();
    test_no_vms_is_valid();
    test_error_reports_line_number();
    test_media_disk();
    test_media_disk_too_long();
    test_unknown_key_is_retained_not_fatal();
    test_unknown_section_and_its_keys_are_retained();
    test_comments_are_retained_blank_lines_are_not();
    test_key_before_any_section_is_still_fatal();
    test_retention_overflow_is_flagged_not_silent();
    test_overlong_line_is_flagged_not_truncated();
    test_disk_section_all_keys();
    test_disk_defaults();
    test_disk_cdrom_is_always_read_only();
    test_disk_physical_requires_id_match();
    test_disk_physical_rejects_path();
    test_disk_partition_whole_and_numeric();
    test_disk_bad_and_good_compact_correctly();
    test_disk_duplicate_id_and_unknown_key();
    test_disk_bad_values_and_duplicates();
    test_disk_capacity();
    test_disk_remaining_domains();
    test_disk_allow_overwrite_and_ro_disk();
    test_disk_overlong_id_and_path();
    test_too_many_sections_flags_overflow();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
