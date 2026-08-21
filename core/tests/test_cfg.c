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

/* #357: a VM section is ignored entirely unless every required key is present. */
#define REQ "mem_mb = 512\nvcpus = 1\nfirmware = uefi\nboot = disk\nos_hint = linux\n" \
            "target_disk = file:\\hype\\disks\\a.img\n"

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

/* #444 (TERM-6): every key the operator actually wrote must be recorded in seen_fields, and
 * nothing they didn't. */
static void test_seen_fields_records_exactly_what_was_written(void) {
    hype_cfg_t out;
    (void)parse_copy(
        "[vm.win11]\n"
        "vcpus = 4\n"
        "cpu_set = 4-7\n"
        "mem_mb = 8192\n"
        "boot = installer\n"
        "install_media = \\EFI\\hype\\win11.iso\n"
        "target_disk = file:\\hype\\disks\\win11.img\n"
        "target_disk_size_gb = 128\n"
        "firmware = uefi\n"
        "os_hint = windows\n"
        "net_mode = nat\n",
        &out);

    CHECK_INT("vcpus was written", 1, (out.vms[0].seen_fields & HYPE_CFG_F_VCPUS) != 0);
    CHECK_INT("cpu_set was written", 1, (out.vms[0].seen_fields & HYPE_CFG_F_CPU_SET) != 0);
    CHECK_INT("mem_mb was written", 1, (out.vms[0].seen_fields & HYPE_CFG_F_MEM_MB) != 0);
    CHECK_INT("boot was written", 1, (out.vms[0].seen_fields & HYPE_CFG_F_BOOT) != 0);
    CHECK_INT("install_media was written", 1,
              (out.vms[0].seen_fields & HYPE_CFG_F_INSTALL_MEDIA) != 0);
    CHECK_INT("target_disk was written", 1, (out.vms[0].seen_fields & HYPE_CFG_F_TARGET_DISK) != 0);
    CHECK_INT("target_disk_size_gb was written", 1,
              (out.vms[0].seen_fields & HYPE_CFG_F_TARGET_DISK_SIZE_GB) != 0);
    CHECK_INT("firmware was written", 1, (out.vms[0].seen_fields & HYPE_CFG_F_FIRMWARE) != 0);
    CHECK_INT("os_hint was written", 1, (out.vms[0].seen_fields & HYPE_CFG_F_OS_HINT) != 0);
    CHECK_INT("net_mode was written", 1, (out.vms[0].seen_fields & HYPE_CFG_F_NET_MODE) != 0);

    /* Never written by this config -- must read as unset, not accidentally aliased onto
     * some other bit. */
    CHECK_INT("label was NOT written", 0, (out.vms[0].seen_fields & HYPE_CFG_F_LABEL) != 0);
    CHECK_INT("net_peers was NOT written", 0, (out.vms[0].seen_fields & HYPE_CFG_F_NET_PEERS) != 0);
    CHECK_INT("media_disk was NOT written", 0, (out.vms[0].seen_fields & HYPE_CFG_F_MEDIA_DISK) != 0);
    CHECK_INT("disks was NOT written (target_disk form used instead)", 0,
              (out.vms[0].seen_fields & HYPE_CFG_F_DISKS) != 0);
}

/* A VM using the disks=/cdroms= reference form instead of target_disk must show THAT choice in
 * seen_fields, not leave the reader guessing which storage form was used. */
static void test_seen_fields_distinguishes_storage_form(void) {
    hype_cfg_t out;
    (void)parse_copy(
        "[vm.a]\n"
        "vcpus = 1\nmem_mb = 512\nboot = disk\nfirmware = uefi\nos_hint = linux\n"
        "disks = sys\n",
        &out);
    CHECK_INT("disks was written", 1, (out.vms[0].seen_fields & HYPE_CFG_F_DISKS) != 0);
    CHECK_INT("target_disk was NOT written", 0,
              (out.vms[0].seen_fields & HYPE_CFG_F_TARGET_DISK) != 0);
}

/* seen_fields must survive VM compaction (a bad VM dropped, survivors reindexed) -- it lives
 * inside the struct that gets memmove'd, not a separate array keyed by the old index. */
static void test_seen_fields_survives_compaction(void) {
    hype_cfg_t out;
    hype_cfg_result_t r = parse_copy(
        "[vm.bad]\n"
        "vcpus = 1\n" /* missing mem_mb/boot/firmware/os_hint/storage: fails validate_required */
        "[vm.good]\n" REQ "label = kept\n",
        &out);
    CHECK_INT("parses (one bad VM isolates, does not fail the file)", HYPE_CFG_OK, r.status);
    CHECK_INT("only the good VM survives", 1, (int)out.vm_count);
    CHECK_STR("it is the right one", "good", out.vms[0].name);
    CHECK_INT("its seen_fields moved with it", 1, (out.vms[0].seen_fields & HYPE_CFG_F_LABEL) != 0);
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

/*
 * #565 / decision 49: `display` selects an EXTRA adapter and defaults to none.
 *
 * The default matters more than the setting: a VM that never mentions `display` must not acquire a
 * PCI display device, because a Linux guest with bochs-drm inbox would bind it and move its
 * console away from the ramfb surface hype renders and the serial console the input runner drives.
 */
#define DISPLAY_TEST_BASE                                                                          \
    "[vm.a]\n"                                                                                     \
    "vcpus = 1\n"                                                                                  \
    "mem_mb = 512\n"                                                                               \
    "boot = kernel\n"                                                                              \
    "kernel = \\k.bin\n"                                                                           \
    "os_hint = none\n"

/*
 * #405: hype's OWN uplink address, statically configured. The default matters most here too: a
 * config with no uplink keys is a supported configuration (a host running only offline guests), not
 * a broken one.
 */
#define UPLINK_TEST_BASE                                                                           \
    "[hype]\n"                                                                                      \
    "config_version = 1\n"

#define UPLINK_TEST_VM                                                                             \
    "[vm.a]\n"                                                                                      \
    "vcpus = 1\n"                                                                                    \
    "mem_mb = 512\n"                                                                                 \
    "boot = kernel\n"                                                                                \
    "kernel = \\k.bin\n"                                                                             \
    "os_hint = none\n"

static void test_uplink_static_address(void) {
    hype_cfg_t out;
    hype_cfg_result_t res;

    res = parse_copy(UPLINK_TEST_BASE UPLINK_TEST_VM, &out);
    CHECK_INT("no uplink keys parses", HYPE_CFG_OK, res.status);
    CHECK_INT("and means no uplink, which is a supported state", 0, out.hype.has_uplink);

    res = parse_copy(UPLINK_TEST_BASE
                     "uplink_ip = 10.0.2.15\n"
                     "uplink_mask = 255.255.255.0\n"
                     "uplink_gateway = 10.0.2.2\n" UPLINK_TEST_VM,
                     &out);
    CHECK_INT("a full uplink parses", HYPE_CFG_OK, res.status);
    CHECK_INT("and is marked present", 1, out.hype.has_uplink);
    CHECK_INT("address octet 0", 10, out.hype.uplink_ip[0]);
    CHECK_INT("address octet 3", 15, out.hype.uplink_ip[3]);
    CHECK_INT("mask octet 0", 255, out.hype.uplink_mask[0]);
    CHECK_INT("mask octet 3", 0, out.hype.uplink_mask[3]);
    CHECK_INT("gateway octet 3", 2, out.hype.uplink_gateway[3]);

    /* A PARTIAL uplink is not a usable configuration: an address with no gateway gives a NAT plane
     * that translates packets and has nowhere to send them. It parses, and has_uplink stays clear,
     * so the caller sees "no uplink" rather than a half one. */
    res = parse_copy(UPLINK_TEST_BASE "uplink_ip = 10.0.2.15\n" UPLINK_TEST_VM, &out);
    CHECK_INT("an address alone parses", HYPE_CFG_OK, res.status);
    CHECK_INT("but does not count as an uplink", 0, out.hype.has_uplink);

    res = parse_copy(UPLINK_TEST_BASE
                     "uplink_ip = 10.0.2.15\n"
                     "uplink_mask = 255.255.255.0\n" UPLINK_TEST_VM,
                     &out);
    CHECK_INT("address and mask without a gateway parses", HYPE_CFG_OK, res.status);
    CHECK_INT("and still does not count", 0, out.hype.has_uplink);

    /*
     * A duplicate does NOT fail the parse, and that is [hype]'s documented rule rather than a gap:
     * §4.3 says a malformed global falls back to defaults and sets `malformed`, because a bad global
     * cannot make a VM wrong -- it can only make hype behave as it did before the section existed.
     * My first version of this test asserted ERR_DUPLICATE_KEY and was wrong about the contract, not
     * about the code.
     *
     * What must hold is that the operator is not left believing a rejected uplink took effect.
     */
    res = parse_copy(UPLINK_TEST_BASE
                     "uplink_ip = 10.0.2.15\n"
                     "uplink_ip = 10.0.2.16\n" UPLINK_TEST_VM,
                     &out);
    CHECK_INT("a duplicate uplink_ip does not fail the parse", HYPE_CFG_OK, res.status);
    CHECK_INT("but the section is marked malformed", 1, out.hype.malformed);
    CHECK_INT("and no uplink is left configured", 0, out.hype.has_uplink);
    CHECK_INT("the VM still parsed -- a bad global cannot make a VM wrong", 1, out.vm_count);
}

/*
 * The address parser is strict, and every case here is one a lenient parser would have accepted --
 * silently giving hype the wrong address, whose only symptom is a network that does not work.
 */
static void test_uplink_address_parsing_is_strict(void) {
    hype_cfg_t out;
    hype_cfg_result_t res;
    unsigned int i;
    const char *bad[] = {
        "10.0.2",          /* three octets */
        "10.0.2.15.1",     /* five */
        "10.0.2.256",      /* octet out of range */
        "10.0.2.1500",     /* four digits */
        "10.0..15",        /* empty octet */
        ".10.0.2.15",      /* leading dot */
        "10.0.2.15.",      /* trailing dot */
        "10.0.2.15x",      /* trailing junk */
        "ten.0.2.15",      /* not a number */
        "",                /* empty */
        "10 .0.2.15"       /* embedded space */
    };

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        /* Assembled with strcpy/strcat rather than hype_snprintf: this file includes <string.h> and
         * NOT core/format.h, and using an undeclared hype_snprintf here once made `make test` fail
         * to BUILD while a grep for FAIL reported the suite clean. */
        char text[256];
        text[0] = '\0';
        strcat(text, UPLINK_TEST_BASE "uplink_ip = ");
        strcat(text, bad[i]);
        strcat(text, "\n");
        strcat(text, UPLINK_TEST_VM);
        res = parse_copy(text, &out);
        /*
         * Per §4.3 the parse still succeeds; what must be true is that the bad address DID NOT
         * become hype's uplink, and that the section is flagged so the rejection is not silent.
         * Checking `malformed` rather than the status is the difference between testing the parser's
         * error code and testing the thing that matters -- an unusable address must never reach the
         * NAT plane.
         */
        if (res.status != HYPE_CFG_OK || out.hype.malformed != 1 || out.hype.has_uplink != 0) {
            printf("FAIL: uplink_ip = '%s' -- status %d, malformed %d, has_uplink %d\n", bad[i],
                   (int)res.status, out.hype.malformed, out.hype.has_uplink);
            failures++;
        }
    }

    /* And the boundary values that ARE legal. */
    res = parse_copy(UPLINK_TEST_BASE
                     "uplink_ip = 0.0.0.0\n"
                     "uplink_mask = 255.255.255.255\n"
                     "uplink_gateway = 1.2.3.4\n" UPLINK_TEST_VM,
                     &out);
    CHECK_INT("0 and 255 octets are legal values", HYPE_CFG_OK, res.status);
    CHECK_INT("255 parsed", 255, out.hype.uplink_mask[0]);
    CHECK_INT("0 parsed", 0, out.hype.uplink_ip[0]);
}

static void test_display_key(void) {
    hype_cfg_t out;
    hype_cfg_result_t res;

    /* Absent -> none. This is the case that matters most: a config that never mentions `display`
     * must not acquire a PCI display device. */
    res = parse_copy(DISPLAY_TEST_BASE, &out);
    CHECK_INT("no display key parses", HYPE_CFG_OK, res.status);
    CHECK_INT("display defaults to none", (int)HYPE_CFG_DISPLAY_NONE, (int)out.vms[0].display);

    res = parse_copy(DISPLAY_TEST_BASE "display = none\n", &out);
    CHECK_INT("display = none parses", HYPE_CFG_OK, res.status);
    CHECK_INT("display none", (int)HYPE_CFG_DISPLAY_NONE, (int)out.vms[0].display);

    res = parse_copy(DISPLAY_TEST_BASE "display = bochs\n", &out);
    CHECK_INT("display = bochs parses", HYPE_CFG_OK, res.status);
    CHECK_INT("display bochs", (int)HYPE_CFG_DISPLAY_BOCHS, (int)out.vms[0].display);

    /* A wrong value is refused, not silently defaulted -- a typo must not quietly disable a
     * device the operator asked for. */
    res = parse_copy(DISPLAY_TEST_BASE "display = vga\n", &out);
    CHECK_INT("an unknown display value is refused", HYPE_CFG_ERR_BAD_VALUE, res.status);

    /* Duplicates are refused like every other key. */
    res = parse_copy(DISPLAY_TEST_BASE "display = bochs\ndisplay = none\n", &out);
    CHECK_INT("a duplicate display is refused", HYPE_CFG_ERR_DUPLICATE_KEY, res.status);
}

/*
 * #535: boot = kernel -- a VM with a raw kernel image and no storage and no firmware.
 * The waivers are the point: a microtest VM has nothing to boot from a disk, so requiring
 * target_disk and firmware would force every suite config to carry two inert lines.
 */
static void test_boot_kernel(void) {
    const char *cfg =
        "[vm.ram1]\n"
        "vcpus = 1\n"
        "mem_mb = 512\n"
        "boot = kernel\n"
        "kernel = \\EFI\\hype\\micro\\ram1.bin\n"
        "os_hint = none\n";
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy(cfg, &out);

    CHECK_INT("boot=kernel needs no storage or firmware", HYPE_CFG_OK, res.status);
    CHECK_INT("one VM parsed", 1, (int)out.vm_count);
    CHECK_INT("boot mode is kernel", (int)HYPE_CFG_BOOT_KERNEL, (int)out.vms[0].boot);
    CHECK_INT("has_kernel set", 1, out.vms[0].has_kernel);
    CHECK_STR("kernel path", "\\EFI\\hype\\micro\\ram1.bin", out.vms[0].kernel);
}

/* A kernel VM may still be handed a disk -- the keys are no longer REQUIRED, not forbidden. */
static void test_boot_kernel_with_disk_allowed(void) {
    const char *cfg =
        "[vm.ram1]\n"
        "vcpus = 1\n"
        "mem_mb = 512\n"
        "boot = kernel\n"
        "kernel = \\micro\\k.bin\n"
        "target_disk = file:x.img\n"
        "firmware = uefi\n"
        "os_hint = none\n";
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy(cfg, &out);

    CHECK_INT("boot=kernel with a disk is still valid", HYPE_CFG_OK, res.status);
    CHECK_INT("boot mode is kernel", (int)HYPE_CFG_BOOT_KERNEL, (int)out.vms[0].boot);
}

/* #535: the serializer must round-trip the new mode and its key (#486's write-back path). */
static void test_boot_kernel_write_back(void) {
    const char *cfg =
        "[vm.ram1]\n"
        "vcpus = 1\n"
        "mem_mb = 512\n"
        "boot = kernel\n"
        "kernel = \\micro\\k.bin\n"
        "os_hint = none\n";
    hype_cfg_t out;
    hype_cfg_t back;
    static char written[16384];
    hype_cfg_serialize_result_t ser;
    hype_cfg_result_t res = parse_copy(cfg, &out);

    CHECK_INT("parse ok", HYPE_CFG_OK, res.status);
    ser = hype_cfg_serialize(&out, written, sizeof(written));
    CHECK_INT("serialize not truncated", 0, ser.truncated);
    CHECK_INT("boot = kernel was written", 1, strstr(written, "boot = kernel") != NULL);
    CHECK_INT("kernel path was written", 1, strstr(written, "kernel = \\micro\\k.bin") != NULL);

    res = parse_copy(written, &back);
    CHECK_INT("re-parse of written config ok", HYPE_CFG_OK, res.status);
    CHECK_INT("boot mode survived the round trip", (int)HYPE_CFG_BOOT_KERNEL, (int)back.vms[0].boot);
    CHECK_STR("kernel path survived the round trip", "\\micro\\k.bin", back.vms[0].kernel);
}

/*
 * #546: the kernel command line. Three states that must stay distinguishable -- absent, explicitly
 * empty, and set -- because the loader turns them into cmd_line_ptr == 0, a pointer to a NUL, and a
 * pointer to a string, and a kernel reads all three differently.
 */
static void test_cmdline(void) {
    const char *cfg =
        "[vm.k]\n"
        "vcpus = 1\n"
        "mem_mb = 512\n"
        "boot = kernel\n"
        "kernel = \\k.bin\n"
        "cmdline = console=ttyS0 acpi=off\n"
        "os_hint = none\n";
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy(cfg, &out);

    CHECK_INT("cmdline parses on a kernel VM", HYPE_CFG_OK, res.status);
    CHECK_INT("has_cmdline set", 1, out.vms[0].has_cmdline);
    CHECK_STR("cmdline value", "console=ttyS0 acpi=off", out.vms[0].cmdline);
}

static void test_cmdline_absent_vs_empty(void) {
    const char *absent =
        "[vm.k]\nvcpus=1\nmem_mb=512\nboot=kernel\nkernel=\\k.bin\nos_hint=none\n";
    const char *empty =
        "[vm.k]\nvcpus=1\nmem_mb=512\nboot=kernel\nkernel=\\k.bin\ncmdline=\nos_hint=none\n";
    hype_cfg_t a, b;

    CHECK_INT("no cmdline key is valid", HYPE_CFG_OK, parse_copy(absent, &a).status);
    CHECK_INT("and has_cmdline stays clear", 0, a.vms[0].has_cmdline);

    CHECK_INT("an EMPTY cmdline is valid", HYPE_CFG_OK, parse_copy(empty, &b).status);
    CHECK_INT("and has_cmdline is SET -- explicitly nothing is not nothing", 1,
              b.vms[0].has_cmdline);
    CHECK_STR("with an empty string", "", b.vms[0].cmdline);
}

/* A cmdline longer than the key can hold must be refused, not truncated: `console=ttyS0` cut to
 * `console=tty` is a valid, wrong setting. */
static void test_cmdline_too_long(void) {
    static char cfg[HYPE_CFG_CMDLINE_MAX + 256];
    hype_cfg_t out;
    unsigned i, n;

    strcpy(cfg, "[vm.k]\nvcpus=1\nmem_mb=512\nboot=kernel\nkernel=\\k.bin\ncmdline=");
    n = (unsigned)strlen(cfg);
    for (i = 0; i < HYPE_CFG_CMDLINE_MAX; i++) {
        cfg[n + i] = 'x';
    }
    strcpy(cfg + n + i, "\nos_hint=none\n");

    CHECK_INT("an over-long cmdline is refused", HYPE_CFG_ERR_VALUE_TOO_LONG,
              parse_copy(cfg, &out).status);
}

/* #546: it must round-trip, or #486's create/write-back would silently drop it. */
static void test_cmdline_write_back(void) {
    const char *cfg =
        "[vm.k]\nvcpus=1\nmem_mb=512\nboot=kernel\nkernel=\\k.bin\n"
        "cmdline=console=ttyS0 pages=all\nos_hint=none\n";
    hype_cfg_t out, back;
    static char written[16384];
    hype_cfg_serialize_result_t ser;

    CHECK_INT("parse ok", HYPE_CFG_OK, parse_copy(cfg, &out).status);
    ser = hype_cfg_serialize(&out, written, sizeof(written));
    CHECK_INT("serialize not truncated", 0, ser.truncated);
    CHECK_INT("cmdline was written", 1,
              strstr(written, "cmdline = console=ttyS0 pages=all") != NULL);
    CHECK_INT("re-parse ok", HYPE_CFG_OK, parse_copy(written, &back).status);
    CHECK_STR("cmdline survived the round trip", "console=ttyS0 pages=all", back.vms[0].cmdline);
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
    /* #535 */
    {"missing kernel when boot=kernel",
     "[vm.a]\nvcpus=1\nmem_mb=1\nboot=kernel\nos_hint=none\n",
     HYPE_CFG_ERR_MISSING_REQUIRED},
    {"kernel set but boot=disk",
     "[vm.a]\nvcpus=1\nmem_mb=1\nboot=disk\ntarget_disk=file:x\nfirmware=uefi\nos_hint=none\n"
     "kernel=\\k.bin\n",
     HYPE_CFG_ERR_BAD_VALUE},
    {"boot=kernel with install_media",
     "[vm.a]\nvcpus=1\nmem_mb=1\nboot=kernel\nkernel=\\k.bin\ninstall_media=\\a.iso\n"
     "os_hint=none\n",
     HYPE_CFG_ERR_BAD_VALUE},
    {"missing os_hint when boot=kernel",
     "[vm.a]\nvcpus=1\nmem_mb=1\nboot=kernel\nkernel=\\k.bin\n",
     HYPE_CFG_ERR_MISSING_REQUIRED},
    {"duplicate key kernel", "[vm.a]\nkernel=x\nkernel=y\n", HYPE_CFG_ERR_DUPLICATE_KEY},
    {"empty kernel", "[vm.a]\nkernel=\n", HYPE_CFG_ERR_BAD_VALUE},
    /* #546 */
    {"duplicate key cmdline", "[vm.a]\ncmdline=x\ncmdline=y\n", HYPE_CFG_ERR_DUPLICATE_KEY},
    {"cmdline on a boot=disk VM has no kernel to give it to",
     "[vm.a]\nvcpus=1\nmem_mb=1\nboot=disk\ntarget_disk=file:x\nfirmware=uefi\nos_hint=none\n"
     "cmdline=console=ttyS0\n",
     HYPE_CFG_ERR_BAD_VALUE},
    {"cmdline on a boot=installer VM likewise",
     "[vm.a]\nvcpus=1\nmem_mb=1\nboot=installer\ninstall_media=\\a.iso\ntarget_disk=file:x\n"
     "firmware=uefi\nos_hint=none\ncmdline=console=ttyS0\n",
     HYPE_CFG_ERR_BAD_VALUE},
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
    /* #450: one VM too many bounds the list instead of discarding the file -- see
     * test_parse_into_refuses_past_bound_storage for the same property against caller storage. */
    CHECK_INT("a config one VM past capacity still parses", HYPE_CFG_OK, res.status);
    CHECK_INT("exactly the capacity is kept", HYPE_CFG_MAX_VMS, (int)out.vm_count);
    CHECK_INT("the extra VM is reported, not dropped silently", 1, (int)out.skipped_vms);
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
/*
 * SMP-1 (#185): vcpus resolution. Same contract as mem_mb above -- the STATUS is the part
 * worth testing, because it is what lets a log distinguish "you asked for 4" from "you asked
 * for 64 and got 8".
 */
static void test_resolve_vcpus(void) {
    unsigned n;

    /* Unconfigured -> one vCPU, which is hype's behaviour from before SMP existed. */
    CHECK_INT("no cfg -> DEFAULTED", HYPE_CFG_RAM_DEFAULTED,
              (int)hype_cfg_resolve_vcpus(0u, 8u, &n));
    CHECK_INT("no cfg -> 1 vCPU", 1, (int)n);

    /* In range -> used exactly. */
    CHECK_INT("in range -> APPLIED", HYPE_CFG_RAM_APPLIED,
              (int)hype_cfg_resolve_vcpus(4u, 8u, &n));
    CHECK_INT("in range -> 4 applied", 4, (int)n);

    /* Boundaries inclusive: 1 and exactly max are APPLIED, not clamped. */
    CHECK_INT("one vCPU -> APPLIED", HYPE_CFG_RAM_APPLIED,
              (int)hype_cfg_resolve_vcpus(1u, 8u, &n));
    CHECK_INT("one vCPU value", 1, (int)n);
    CHECK_INT("exactly max -> APPLIED", HYPE_CFG_RAM_APPLIED,
              (int)hype_cfg_resolve_vcpus(8u, 8u, &n));
    CHECK_INT("exactly max value", 8, (int)n);

    /* Over the cap clamps and SAYS so. A silent clamp here would size the vCPU pools for
     * fewer contexts than the MADT later advertises -- #237's shared-VMCB failure, arrived at
     * by a different route. */
    CHECK_INT("over cap -> CLAMPED_HIGH", HYPE_CFG_RAM_CLAMPED_HIGH,
              (int)hype_cfg_resolve_vcpus(64u, 8u, &n));
    CHECK_INT("over cap -> clamped to max", 8, (int)n);

    /* A caller passing a zero cap still gets a runnable VM rather than zero vCPUs. */
    CHECK_INT("zero cap -> still one vCPU", 1, (int)(hype_cfg_resolve_vcpus(4u, 0u, &n), n));

    /* NULL out is safe -- the status alone is a legitimate use. */
    (void)hype_cfg_resolve_vcpus(2u, 8u, (unsigned *)0);
}

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
    /* Exactly the forward-compatibility case: a section kind an older hype cannot model.
     * This used [nic.*], which #583 implemented -- so it now needs a kind that is still
     * genuinely unknown, or the test would be asserting forward compatibility against a
     * feature that had arrived. */
    const char *cfg =
        "[vm.a]\n"
        "vcpus = 1\n"
        "mem_mb = 512\n"
        "boot = disk\n"
        "target_disk = file:a.img\n"
        "firmware = uefi\n"
        "os_hint = linux\n"
        "\n"
        "[future.net0]\n"
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
    CHECK_STR("the unknown header is kept verbatim", "[future.net0]", out.sections[1].raw);
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


/* ---- #222 (CONFIG-2, spec §5.2/§5.4/§5.7): disks / cdroms / boot_order lists ---- */

static void test_vm_disk_lists(void) {
    const char *cfg =
        "[vm.win11]\n"
        "vcpus = 4\n"
        "mem_mb = 8192\n"
        "boot = installer\n"
        "install_media = \\iso\\win11.iso\n"
        "firmware = uefi\n"
        "os_hint = windows\n"
        "disks = win-sys, win-data1, win-data2\n"
        "cdroms = inst, drivers\n"
        "boot_order = inst, win-sys\n"
        "\n"
        "[disk.win-sys]\nbacking = file\npath = sys.img\nbus = ahci-sata\n"
        "[disk.win-data1]\nbacking = file\npath = d1.img\nbus = nvme\n"
        "[disk.win-data2]\nbacking = file\npath = d2.img\nbus = nvme\n"
        "[disk.inst]\ntype = cdrom\npath = \\iso\\win11.iso\n"
        "[disk.drivers]\ntype = cdrom\npath = \\iso\\virtio.iso\n";
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy(cfg, &out);

    CHECK_INT("the spec's mixed-bus example parses", HYPE_CFG_OK, res.status);
    CHECK_INT("one VM", 1, out.vm_count);
    CHECK_INT("five disks defined", 5, out.disk_count);
    CHECK_INT("three disks attached", 3, out.vms[0].disks_count);
    /* Order is meaningful: it is the order the guest enumerates them. */
    CHECK_STR("disks[0]", "win-sys", out.vms[0].disks[0]);
    CHECK_STR("disks[1]", "win-data1", out.vms[0].disks[1]);
    CHECK_STR("disks[2]", "win-data2", out.vms[0].disks[2]);
    CHECK_INT("two cdroms attached", 2, out.vms[0].cdroms_count);
    CHECK_STR("cdroms[0]", "inst", out.vms[0].cdroms[0]);
    CHECK_STR("cdroms[1]", "drivers", out.vms[0].cdroms[1]);
    CHECK_INT("boot_order has two entries", 2, out.vms[0].boot_order_count);
    CHECK_STR("boot_order[0] is the installer CD", "inst", out.vms[0].boot_order[0]);
    CHECK_STR("boot_order[1] is the system disk", "win-sys", out.vms[0].boot_order[1]);
    /* No target_disk at all: the reference form replaces it entirely. */
    CHECK_INT("mixed buses survive per device", (int)HYPE_CFG_BUS_AHCI_SATA, (int)out.disks[0].bus);
    CHECK_INT("nvme on the data disk", (int)HYPE_CFG_BUS_NVME, (int)out.disks[1].bus);
}

static void test_vm_lists_are_ordered_not_sets(void) {
    const char *a =
        "[vm.a]\nvcpus=1\nmem_mb=512\nboot=disk\nfirmware=uefi\nos_hint=linux\ndisks = x, y\n";
    const char *b =
        "[vm.a]\nvcpus=1\nmem_mb=512\nboot=disk\nfirmware=uefi\nos_hint=linux\ndisks = y, x\n";
    hype_cfg_t oa, ob;

    CHECK_INT("both parse", HYPE_CFG_OK, parse_copy(a, &oa).status);
    CHECK_INT("both parse", HYPE_CFG_OK, parse_copy(b, &ob).status);
    /* Different machines: enumeration order differs, so the order must be preserved as written. */
    CHECK_STR("first config keeps x first", "x", oa.vms[0].disks[0]);
    CHECK_STR("second config keeps y first", "y", ob.vms[0].disks[0]);
}

static void test_vm_inline_and_lists_are_mutually_exclusive(void) {
    /* Two answers to "which disk" cannot be reconciled, and silently picking one would be the #285
     * failure over again. */
    const char *cfg =
        "[vm.a]\nvcpus=1\nmem_mb=512\nboot=disk\nfirmware=uefi\nos_hint=linux\n"
        "target_disk = file:a.img\ndisks = x\n";
    hype_cfg_t out;

    CHECK_INT("inline target_disk together with disks= is refused", HYPE_CFG_ERR_BAD_VALUE,
              parse_copy(cfg, &out).status);
}

static void test_vm_storage_form_is_required(void) {
    /* Neither form: refused. §5.2 permits a diskless VM in principle, but accepting it here would
     * turn a misspelled target_disk into a silently disk-less guest. */
    const char *cfg = "[vm.a]\nvcpus=1\nmem_mb=512\nboot=disk\nfirmware=uefi\nos_hint=linux\n";
    hype_cfg_t out;

    CHECK_INT("a VM with no storage form at all is refused", HYPE_CFG_ERR_MISSING_REQUIRED,
              parse_copy(cfg, &out).status);
}

static void test_vm_cdroms_alone_satisfies_the_requirement(void) {
    /* A boot-from-CD VM with no hard disk is legitimate -- a live ISO needs no target. */
    const char *cfg =
        "[vm.live]\nvcpus=1\nmem_mb=512\nboot=installer\ninstall_media=\\iso\\a.iso\n"
        "firmware=uefi\nos_hint=linux\ncdroms = live\n"
        "[disk.live]\ntype=cdrom\npath=\\iso\\a.iso\n";
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy(cfg, &out);

    CHECK_INT("cdroms alone is a valid storage form", HYPE_CFG_OK, res.status);
    CHECK_INT("one cdrom attached", 1, out.vms[0].cdroms_count);
    CHECK_INT("no disks attached", 0, out.vms[0].disks_count);
}

static void test_vm_list_errors(void) {
    struct { const char *desc; const char *tail; hype_cfg_status_t want; } cases[] = {
        {"repeated id in disks", "disks = x, x\n", HYPE_CFG_ERR_BAD_VALUE},
        {"repeated id in cdroms", "cdroms = c, c\n", HYPE_CFG_ERR_BAD_VALUE},
        {"repeated id in boot_order", "disks = x\nboot_order = x, x\n", HYPE_CFG_ERR_BAD_VALUE},
        {"empty disks value", "disks =\n", HYPE_CFG_ERR_BAD_VALUE},
        {"empty piece in the middle", "disks = x, , y\n", HYPE_CFG_ERR_BAD_VALUE},
        {"trailing comma", "disks = x,\n", HYPE_CFG_ERR_BAD_VALUE},
        {"duplicate disks key", "disks = x\ndisks = y\n", HYPE_CFG_ERR_DUPLICATE_KEY},
        {"duplicate cdroms key", "cdroms = c\ncdroms = d\n", HYPE_CFG_ERR_DUPLICATE_KEY},
        {"duplicate boot_order key", "disks = x\nboot_order = x\nboot_order = x\n",
         HYPE_CFG_ERR_DUPLICATE_KEY},
        {"too many disks", "disks = a,b,c,d,e,f,g,h,i\n", HYPE_CFG_ERR_TOO_MANY_ENTRIES}
    };
    unsigned int i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char cfg[1024];
        hype_cfg_t out;
        snprintf(cfg, sizeof(cfg),
                 "[vm.a]\nvcpus=1\nmem_mb=512\nboot=disk\nfirmware=uefi\nos_hint=linux\n%s",
                 cases[i].tail);
        CHECK_INT(cases[i].desc, (int)cases[i].want, (int)parse_copy(cfg, &out).status);
    }
}

static void test_vm_list_id_too_long(void) {
    char cfg[1024];
    hype_cfg_t out;
    int n;
    unsigned int i;

    n = snprintf(cfg, sizeof(cfg),
                 "[vm.a]\nvcpus=1\nmem_mb=512\nboot=disk\nfirmware=uefi\nos_hint=linux\ndisks = ");
    for (i = 0; i < HYPE_CFG_NAME_MAX + 4u; i++) cfg[n++] = 'd';
    cfg[n++] = '\n';
    cfg[n] = '\0';
    CHECK_INT("an over-long id in a list is rejected", HYPE_CFG_ERR_VALUE_TOO_LONG,
              (int)parse_copy(cfg, &out).status);
}


/* ---- #222 (spec §5.1): the [hype] master section ---- */

static void test_hype_section_all_keys(void) {
    char cfg[2048];
    hype_cfg_t out;
    hype_cfg_result_t res;

    snprintf(cfg, sizeof(cfg), "%s%s",
             "[hype]\n"
             "config_version = 1\n"
             "host_cpu_budget = 2-5, 8\n"
             "default_net_mode = nat\n"
             "dashboard_default_view = vm:a\n"
             "autostart = a\n", VM_A);
    res = parse_copy(cfg, &out);

    CHECK_INT("[hype] parses", HYPE_CFG_OK, res.status);
    CHECK_INT("not malformed", 0, out.hype.malformed);
    CHECK_INT("config_version", 1, out.hype.config_version);
    /* The cpu-list grammar is SHARED with cpu_set: a range plus a single value. */
    CHECK_INT("host_cpu_budget expanded", 5, out.hype.host_cpu_budget_count);
    CHECK_INT("budget[0]", 2, out.hype.host_cpu_budget[0]);
    CHECK_INT("budget[3]", 5, out.hype.host_cpu_budget[3]);
    CHECK_INT("budget[4]", 8, out.hype.host_cpu_budget[4]);
    CHECK_INT("default_net_mode nat", (int)HYPE_CFG_NET_NAT, (int)out.hype.default_net_mode);
    CHECK_INT("view vm", (int)HYPE_CFG_VIEW_VM, (int)out.hype.dashboard_default_view);
    CHECK_STR("view names the VM", "a", out.hype.dashboard_default_vm);
    CHECK_INT("autostart list", (int)HYPE_CFG_AUTOSTART_LIST, (int)out.hype.autostart);
    CHECK_INT("one VM autostarted", 1, out.hype.autostart_count);
    CHECK_STR("autostart[0]", "a", out.hype.autostart_vms[0]);
    /* The section is recorded in file order like any other. */
    CHECK_INT("section 0 is [hype]", (int)HYPE_CFG_SECTION_HYPE, (int)out.sections[0].kind);
}

static void test_hype_section_absent_gives_todays_behaviour(void) {
    hype_cfg_t out;
    hype_cfg_result_t res = parse_copy(VM_A, &out);

    CHECK_INT("parses without [hype]", HYPE_CFG_OK, res.status);
    CHECK_INT("config_version defaults to 1", 1, out.hype.config_version);
    CHECK_INT("no cpu budget means all cores", 0, out.hype.has_host_cpu_budget);
    CHECK_INT("net defaults to none", (int)HYPE_CFG_NET_NONE, (int)out.hype.default_net_mode);
    CHECK_INT("view defaults to dashboard", (int)HYPE_CFG_VIEW_DASHBOARD,
              (int)out.hype.dashboard_default_view);
    /* ALL, not NONE: hype starts every VM today, and a default that stopped doing so would be a
     * silent behaviour change for every existing config. */
    CHECK_INT("autostart defaults to all", (int)HYPE_CFG_AUTOSTART_ALL, (int)out.hype.autostart);
    CHECK_INT("not malformed", 0, out.hype.malformed);
}

/* #429 --------------------------------------------------------------------------------- */

static void test_hype_cpu_avg_window_default_is_one(void) {
    hype_cfg_t c;
    (void)parse_copy("[vm.a]\n" REQ, &c);
    CHECK_INT("defaults to 1 second when [hype] is absent entirely", 1,
              (int)c.hype.cpu_avg_window_secs);
}

static void test_hype_cpu_avg_window_configured_value_is_kept(void) {
    char cfg[1024];
    hype_cfg_t c;
    hype_cfg_result_t r;

    snprintf(cfg, sizeof(cfg), "[hype]\ncpu_avg_window_secs = 5\n[vm.a]\n" REQ);
    r = parse_copy(cfg, &c);
    CHECK_INT("parses OK", HYPE_CFG_OK, r.status);
    CHECK_INT("not malformed", 0, c.hype.malformed);
    CHECK_INT("configured value is kept", 5, (int)c.hype.cpu_avg_window_secs);
}

/* A 0 is a successfully-parsed NUMBER that is merely out of range -- clamped to the floor,
 * not treated as a syntax error that resets the whole [hype] section. */
static void test_hype_cpu_avg_window_zero_is_clamped_not_rejected(void) {
    char cfg[1024];
    hype_cfg_t c;
    hype_cfg_result_t r;

    snprintf(cfg, sizeof(cfg), "[hype]\ncpu_avg_window_secs = 0\ndefault_net_mode = nat\n[vm.a]\n" REQ);
    r = parse_copy(cfg, &c);
    CHECK_INT("parses OK", HYPE_CFG_OK, r.status);
    CHECK_INT("NOT flagged malformed -- 0 is clamped, not a parse error", 0, c.hype.malformed);
    CHECK_INT("clamped up to the 1-second floor", 1, (int)c.hype.cpu_avg_window_secs);
    CHECK_INT("the REST of [hype] still applied normally", (int)HYPE_CFG_NET_NAT,
              (int)c.hype.default_net_mode);
}

/* A non-numeric value, by contrast, IS a syntax error -- §4.3's usual whole-section fallback. */
static void test_hype_cpu_avg_window_non_numeric_falls_back(void) {
    char cfg[1024];
    hype_cfg_t c;
    hype_cfg_result_t r;

    snprintf(cfg, sizeof(cfg), "[hype]\ncpu_avg_window_secs = five\n[vm.a]\n" REQ);
    r = parse_copy(cfg, &c);
    CHECK_INT("parses OK (malformed [hype], not a fatal error)", HYPE_CFG_OK, r.status);
    CHECK_INT("flagged malformed", 1, c.hype.malformed);
    CHECK_INT("reset to the default", 1, (int)c.hype.cpu_avg_window_secs);
}

static void test_hype_cpu_avg_window_duplicate_is_rejected(void) {
    char cfg[1024];
    hype_cfg_t c;
    hype_cfg_result_t r;

    snprintf(cfg, sizeof(cfg),
             "[hype]\ncpu_avg_window_secs = 3\ncpu_avg_window_secs = 7\n[vm.a]\n" REQ);
    r = parse_copy(cfg, &c);
    CHECK_INT("parses OK (malformed [hype], not fatal)", HYPE_CFG_OK, r.status);
    CHECK_INT("flagged malformed", 1, c.hype.malformed);
}

static void test_hype_malformed_falls_back_to_defaults(void) {
    char cfg[1024];
    hype_cfg_t out;
    hype_cfg_result_t res;

    /* §4.3: a bad global cannot make a VM wrong, so it must not fail the parse -- but it must not be
     * silent either, or a rejected setting looks applied. */
    snprintf(cfg, sizeof(cfg), "%s%s", "[hype]\ndefault_net_mode = carrier-pigeon\n", VM_A);
    res = parse_copy(cfg, &out);

    CHECK_INT("a malformed [hype] does not fail the parse", HYPE_CFG_OK, res.status);
    CHECK_INT("the VM still loads", 1, out.vm_count);
    CHECK_INT("and the fallback is FLAGGED", 1, out.hype.malformed);
    CHECK_INT("globals are back at their defaults", (int)HYPE_CFG_NET_NONE,
              (int)out.hype.default_net_mode);
}

static void test_hype_unknown_key_still_retained(void) {
    char cfg[1024];
    hype_cfg_t out;
    hype_cfg_result_t res;

    /* An UNKNOWN key is forward-compatibility (§4.1), not malformation -- so it is retained and does
     * NOT trigger the defaults fallback. */
    snprintf(cfg, sizeof(cfg), "%s%s", "[hype]\nfuture_global = 7\ndefault_net_mode = nat\n", VM_A);
    res = parse_copy(cfg, &out);

    CHECK_INT("parses", HYPE_CFG_OK, res.status);
    CHECK_INT("not treated as malformed", 0, out.hype.malformed);
    CHECK_INT("the known key still applied", (int)HYPE_CFG_NET_NAT, (int)out.hype.default_net_mode);
    CHECK_INT("the unknown one retained", 1, out.retained_count);
    CHECK_STR("verbatim", "future_global = 7", out.retained[0].text);
}

static void test_hype_section_errors(void) {
    struct { const char *desc; const char *head; hype_cfg_status_t want; } cases[] = {
        {"duplicate [hype] section", "[hype]\nautostart = all\n[hype]\nautostart = none\n",
         HYPE_CFG_ERR_DUPLICATE_KEY},
        /* A duplicate KEY inside [hype] is malformation like any other, so §4.3 turns it into a
         * flagged defaults fallback -- NOT fatal, unlike a duplicate key in a [vm.*]. The asymmetry
         * is the point of §4.3: a bad global can only cost you the global. */
        {"duplicate key", "[hype]\nautostart = all\nautostart = none\n", HYPE_CFG_OK},
        {"empty vm: view", "[hype]\ndashboard_default_view = vm:\n", HYPE_CFG_OK},
        {"repeated autostart id", "[hype]\nautostart = a, a\n", HYPE_CFG_OK}
    };
    unsigned int i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char cfg[1024];
        hype_cfg_t out;
        hype_cfg_result_t res;
        snprintf(cfg, sizeof(cfg), "%s%s", cases[i].head, VM_A);
        res = parse_copy(cfg, &out);
        CHECK_INT(cases[i].desc, (int)cases[i].want, (int)res.status);
        /* The two "OK" rows are BAD VALUES that §4.3 converts into a flagged defaults fallback
         * rather than a parse failure -- so assert the flag, or the OK would mean nothing. */
        if (cases[i].want == HYPE_CFG_OK) {
            CHECK_INT("...and it is flagged malformed", 1, out.hype.malformed);
        }
    }
}

static void test_hype_duplicate_after_malformed_is_still_caught(void) {
    /* The malformed fallback clears the seen mask, so a naive duplicate check would stop noticing a
     * second [hype] afterwards. */
    char cfg[1024];
    hype_cfg_t out;

    snprintf(cfg, sizeof(cfg), "%s%s", "[hype]\nautostart = nonsense-value\n[hype]\nautostart = all\n",
             VM_A);
    CHECK_INT("a second [hype] after a malformed one is still a duplicate",
              HYPE_CFG_ERR_DUPLICATE_KEY, (int)parse_copy(cfg, &out).status);
}

/* ---- #222 (spec §5.6): bus default resolution ---- */

static void test_resolve_bus(void) {
    hype_cfg_disk_t d;

    memset(&d, 0, sizeof(d));
    d.type = HYPE_CFG_DISK_TYPE_DISK;
    d.bus = HYPE_CFG_BUS_DEFAULT;
    /* The one that matters: Windows has no inbox virtio-blk, so a virtio system disk is INVISIBLE at
     * install time. Defaulting a Windows disk to virtio would look like a hype bug, not a config one. */
    CHECK_INT("windows defaults to ahci-sata", (int)HYPE_CFG_BUS_AHCI_SATA,
              (int)hype_cfg_resolve_bus(&d, HYPE_CFG_OS_WINDOWS));
    CHECK_INT("linux defaults to virtio-blk", (int)HYPE_CFG_BUS_VIRTIO_BLK,
              (int)hype_cfg_resolve_bus(&d, HYPE_CFG_OS_LINUX));
    CHECK_INT("bsd defaults to virtio-blk", (int)HYPE_CFG_BUS_VIRTIO_BLK,
              (int)hype_cfg_resolve_bus(&d, HYPE_CFG_OS_BSD));
    CHECK_INT("none defaults to virtio-blk", (int)HYPE_CFG_BUS_VIRTIO_BLK,
              (int)hype_cfg_resolve_bus(&d, HYPE_CFG_OS_NONE));

    /* Explicit always wins, even where the default would disagree. */
    d.bus = HYPE_CFG_BUS_NVME;
    CHECK_INT("explicit nvme wins over the windows default", (int)HYPE_CFG_BUS_NVME,
              (int)hype_cfg_resolve_bus(&d, HYPE_CFG_OS_WINDOWS));
    d.bus = HYPE_CFG_BUS_VIRTIO_BLK;
    CHECK_INT("explicit virtio-blk wins even for windows", (int)HYPE_CFG_BUS_VIRTIO_BLK,
              (int)hype_cfg_resolve_bus(&d, HYPE_CFG_OS_WINDOWS));

    /* A cdrom is ATAPI regardless -- it is the only optical front-end hype has. */
    d.type = HYPE_CFG_DISK_TYPE_CDROM;
    d.bus = HYPE_CFG_BUS_NVME;
    CHECK_INT("a cdrom is ahci-atapi whatever bus says", (int)HYPE_CFG_BUS_AHCI_ATAPI,
              (int)hype_cfg_resolve_bus(&d, HYPE_CFG_OS_LINUX));

    CHECK_INT("a NULL disk resolves to DEFAULT rather than crashing", (int)HYPE_CFG_BUS_DEFAULT,
              (int)hype_cfg_resolve_bus(0, HYPE_CFG_OS_LINUX));
}


/* ---- #336: `format` is an ASSERTION over the sniffed format, not a selector ---- */

static void test_format_assertion(void) {
    hype_cfg_disk_t d;

    memset(&d, 0, sizeof(d));

    /* Nothing declared: detection decides, exactly as before the key existed. This is the case that
     * keeps "swap a raw image for a qcow2 without editing hype.cfg" working, and note that with
     * format defaulting to RAW it is ONLY has_format that distinguishes it. */
    CHECK_INT("absent key agrees with a raw image", (int)HYPE_CFG_FORMAT_AGREES,
              (int)hype_cfg_check_format(&d, 0));
    CHECK_INT("absent key agrees with a qcow2 image too", (int)HYPE_CFG_FORMAT_AGREES,
              (int)hype_cfg_check_format(&d, 1));

    d.has_format = 1;
    d.format = HYPE_CFG_FORMAT_QCOW2;
    CHECK_INT("qcow2 declared, qcow2 file: agrees", (int)HYPE_CFG_FORMAT_AGREES,
              (int)hype_cfg_check_format(&d, 1));
    CHECK_INT("qcow2 declared, RAW file: mismatch", (int)HYPE_CFG_FORMAT_MISMATCH_WANTED_QCOW2,
              (int)hype_cfg_check_format(&d, 0));

    d.format = HYPE_CFG_FORMAT_RAW;
    CHECK_INT("raw declared, raw file: agrees", (int)HYPE_CFG_FORMAT_AGREES,
              (int)hype_cfg_check_format(&d, 0));
    /* The dangerous direction: writing a qcow2 through the raw path would corrupt it. */
    CHECK_INT("raw declared, QCOW2 file: mismatch", (int)HYPE_CFG_FORMAT_MISMATCH_WANTED_RAW,
              (int)hype_cfg_check_format(&d, 1));

    CHECK_INT("a NULL disk agrees rather than crashing", (int)HYPE_CFG_FORMAT_AGREES,
              (int)hype_cfg_check_format(0, 1));
}

static void test_format_has_flag_is_set_only_when_written(void) {
    char cfg[1024];
    hype_cfg_t out;

    /* Parsed `format = raw` must be distinguishable from an omitted key, or an un-annotated qcow2
     * image would be refused as a mismatch. */
    snprintf(cfg, sizeof(cfg), "%s%s", VM_A, "[disk.d]\nbacking = file\npath = a.img\n");
    CHECK_INT("omitted", HYPE_CFG_OK, (int)parse_copy(cfg, &out).status);
    CHECK_INT("has_format clear when omitted", 0, out.disks[0].has_format);
    CHECK_INT("...and the default is still raw", (int)HYPE_CFG_FORMAT_RAW, (int)out.disks[0].format);

    snprintf(cfg, sizeof(cfg), "%s%s", VM_A,
             "[disk.d]\nbacking = file\npath = a.img\nformat = raw\n");
    CHECK_INT("explicit raw", HYPE_CFG_OK, (int)parse_copy(cfg, &out).status);
    CHECK_INT("has_format SET when written explicitly", 1, out.disks[0].has_format);
}


/* ---- #341 (§4.3): a malformed [vm.*] is skipped, the rest of the config still loads ---- */

static const char *GOOD_VM =
    "[vm.good]\nvcpus=1\nmem_mb=512\nboot=disk\ntarget_disk=file:g.img\nfirmware=uefi\n"
    "os_hint=linux\n";

static void test_one_bad_vm_does_not_cost_the_others(void) {
    char cfg[2048];
    hype_cfg_t out;
    hype_cfg_result_t res;

    /* The bad VM is FIRST, which is the ordering a naive compaction gets wrong. */
    snprintf(cfg, sizeof(cfg), "%s%s",
             "[vm.bad]\nvcpus=1\nmem_mb=512\nboot=disk\ntarget_disk=file:b.img\nfirmware=uefi\n"
             "os_hint=linux\nvcpus=9\n", /* duplicate key */
             GOOD_VM);
    res = parse_copy(cfg, &out);

    CHECK_INT("the config still loads", HYPE_CFG_OK, res.status);
    CHECK_INT("only the good VM survives", 1, out.vm_count);
    CHECK_STR("...and it is the RIGHT one (dense, not a corpse)", "good", out.vms[0].name);
    CHECK_INT("the drop is counted", 1, out.skipped_vms);
    /* A bare count does not tell an operator WHICH machine is missing. */
    CHECK_STR("the dropped VM is named", "bad", out.skipped_vm_name);
    CHECK_INT("...with the offending line", 8, (int)out.skipped_vm_line);
}

static void test_all_vms_bad_still_fails_like_before(void) {
    hype_cfg_t out;
    hype_cfg_result_t res;

    /* This is what keeps every pre-#341 error test valid: with nothing left to isolate, the parse
     * fails with the first error exactly as it always did. "hype running with zero VMs" is never
     * what anyone wanted. */
    res = parse_copy("[vm.a]\nvcpus=1\nvcpus=2\n", &out);
    CHECK_INT("a single bad VM is still a hard error", HYPE_CFG_ERR_DUPLICATE_KEY, res.status);
    CHECK_INT("...reported at its line", 3, (int)res.line);

    res = parse_copy("[vm.a]\nvcpus=nonsense\n[vm.b]\nmem_mb=oops\n", &out);
    CHECK_INT("two bad VMs and nothing else: still fatal", HYPE_CFG_ERR_BAD_VALUE, res.status);
    CHECK_INT("no VMs survive", 0, out.vm_count);
    CHECK_INT("both counted", 2, out.skipped_vms);
}

static void test_missing_required_field_also_isolates(void) {
    char cfg[2048];
    hype_cfg_t out;
    hype_cfg_result_t res;

    /* A VM missing a required key is malformed just as much as one with a bad value. */
    snprintf(cfg, sizeof(cfg), "%s%s", "[vm.nodisk]\nvcpus=1\nmem_mb=512\nboot=disk\n"
                                       "firmware=uefi\nos_hint=linux\n", GOOD_VM);
    res = parse_copy(cfg, &out);

    CHECK_INT("config loads", HYPE_CFG_OK, res.status);
    CHECK_INT("the incomplete VM is dropped", 1, out.vm_count);
    CHECK_STR("the survivor", "good", out.vms[0].name);
    CHECK_INT("counted", 1, out.skipped_vms);
    CHECK_STR("named", "nodisk", out.skipped_vm_name);
}

static void test_section_level_errors_stay_fatal(void) {
    hype_cfg_t out;

    /* Not attributable to any one VM, so there is nothing to isolate -- and blaming the VM that
     * happened to be current would drop a VM that was perfectly fine. */
    CHECK_INT("duplicate VM name is still fatal", HYPE_CFG_ERR_DUPLICATE_VM_NAME,
              (int)parse_copy("[vm.a]\nvcpus=1\n[vm.a]\nvcpus=1\n", &out).status);
    CHECK_INT("key before any section is still fatal", HYPE_CFG_ERR_KEY_BEFORE_SECTION,
              (int)parse_copy("vcpus=1\n", &out).status);
    CHECK_INT("a broken section header is still fatal", HYPE_CFG_ERR_SYNTAX,
              (int)parse_copy("[vm.a\nvcpus=1\n", &out).status);
}

static void test_skipped_vm_section_entry_does_not_alias(void) {
    char cfg[2048];
    hype_cfg_t out;

    /* Same aliasing trap the disk compaction had: with vms[0] dropped, the good VM moves into slot 0
     * and the bad section's stale index would point at it. */
    snprintf(cfg, sizeof(cfg), "%s%s",
             "[vm.bad]\nvcpus=1\nmem_mb=512\nboot=disk\ntarget_disk=file:b.img\nfirmware=uefi\n"
             "os_hint=linux\nvcpus=9\n", GOOD_VM);
    CHECK_INT("loads", HYPE_CFG_OK, (int)parse_copy(cfg, &out).status);
    CHECK_INT("two VM sections recorded", 2, (int)out.section_count);
    CHECK_INT("the dropped VM's section points at nothing", -1, out.sections[0].index);
    CHECK_INT("the survivor's section was retargeted to slot 0", 0, out.sections[1].index);
}



/*
 * #357: `label` appeared in the spec's own worked examples and was not parsed, so a config copied
 * out of the documentation reported "line(s) not understood" and the setting did nothing.
 */
static void test_label_from_the_spec_example_is_accepted(void) {
    hype_cfg_t c;
    hype_cfg_result_t r = parse_copy("[hype]\nconfig_version = 1\n"
                                     "[vm.win11]\nlabel = Windows 11 Workstation\n" REQ,
                                     &c);
    CHECK_INT("the spec's own example parses", HYPE_CFG_OK, r.status);
    CHECK_INT("and produces no unknown lines", 0, (int)c.unknown_count);
    CHECK_INT("one vm", 1, (int)c.vm_count);
    CHECK_STR("label is stored verbatim, spaces included", "Windows 11 Workstation", c.vms[0].label);
    CHECK_STR("the section id is untouched", "win11", c.vms[0].name);
}

static void test_label_absent_leaves_an_empty_string(void) {
    hype_cfg_t c;
    hype_cfg_result_t r = parse_copy("[hype]\nconfig_version = 1\n[vm.a]\n" REQ, &c);
    CHECK_INT("parses", HYPE_CFG_OK, r.status);
    /* Empty, not absent-and-undefined: callers fall back to the section id for display. */
    CHECK_STR("label is empty when unset", "", c.vms[0].label);
}

static void test_label_rejects_empty_and_duplicate(void) {
    hype_cfg_t c;
    hype_cfg_result_t r = parse_copy("[hype]\nconfig_version = 1\n[vm.a]\n" REQ "label =\n", &c);
    /* `label =` with nothing after it is a mistake, not a way to clear it. */
    CHECK_INT("an empty label is a bad value", HYPE_CFG_ERR_BAD_VALUE, r.status);
    r = parse_copy("[hype]\nconfig_version = 1\n[vm.a]\n" REQ "label = One\nlabel = Two\n", &c);
    CHECK_INT("a repeated label is a duplicate key", HYPE_CFG_ERR_DUPLICATE_KEY, r.status);
}

/*
 * #357: parsing `label` was only half the fix -- nothing READ it, so a config copied out of the
 * spec still had no visible effect. These cover the resolution every display site now shares.
 */
/*
 * #357: spec section 3 documents BOTH ';' and '#' as comment characters; only ';' was implemented.
 * A '#' comment above the first section parsed as a key before any section -- a hard error that
 * discarded the entire config, silently falling back to built-in defaults. Found by writing a
 * validation config with an explanatory '#' header, exactly as an operator would.
 */
static void test_hash_begins_a_comment(void) {
    hype_cfg_t c;
    hype_cfg_result_t r = parse_copy("# what this config is for\n"
                                     "[hype]\nconfig_version = 1\n"
                                     "[vm.a]\n" REQ "# trailing note\n",
                                     &c);
    CHECK_INT("a leading '#' comment does not discard the config", HYPE_CFG_OK, r.status);
    CHECK_INT("one vm still parses", 1, (int)c.vm_count);
    CHECK_INT("and the comments are not counted as unknown lines", 0, (int)c.unknown_count);
}

static void test_hash_comment_after_a_value_is_stripped(void) {
    hype_cfg_t c;
    hype_cfg_result_t r = parse_copy("[hype]\nconfig_version = 1\n"
                                     "[vm.a]\n" REQ "label = Prod Box # not part of the name\n",
                                     &c);
    CHECK_INT("parses", HYPE_CFG_OK, r.status);
    /* The trailing comment must not become part of the displayed name. */
    CHECK_STR("value stops at the '#'", "Prod Box", c.vms[0].label);
}

static void test_whichever_comment_char_comes_first_wins(void) {
    hype_cfg_t c;
    hype_cfg_result_t r = parse_copy("[hype]\nconfig_version = 1\n"
                                     "[vm.a]\n" REQ "label = A ; b # c\n",
                                     &c);
    CHECK_INT("parses", HYPE_CFG_OK, r.status);
    CHECK_STR("';' first, so the '#' is inside the comment", "A", c.vms[0].label);
    r = parse_copy("[hype]\nconfig_version = 1\n[vm.a]\n" REQ "label = A # b ; c\n", &c);
    CHECK_INT("parses", HYPE_CFG_OK, r.status);
    CHECK_STR("'#' first, so the ';' is inside the comment", "A", c.vms[0].label);
}

static void test_display_name_prefers_the_label(void) {
    hype_cfg_t c;
    hype_cfg_result_t r = parse_copy("[hype]\nconfig_version = 1\n"
                                     "[vm.win11]\nlabel = Windows 11 Workstation\n" REQ,
                                     &c);
    CHECK_INT("parses", HYPE_CFG_OK, r.status);
    CHECK_STR("a labelled VM is displayed by its label", "Windows 11 Workstation",
              hype_cfg_vm_display_name(&c.vms[0]));
}

static void test_display_name_falls_back_to_the_section_id(void) {
    hype_cfg_t c;
    hype_cfg_result_t r = parse_copy("[hype]\nconfig_version = 1\n[vm.twodisk]\n" REQ, &c);
    CHECK_INT("parses", HYPE_CFG_OK, r.status);
    CHECK_STR("an unlabelled VM keeps the historical section-id name", "twodisk",
              hype_cfg_vm_display_name(&c.vms[0]));
    /* A display path must never be handed NULL and print nothing identifiable. */
    CHECK_STR("a NULL vm yields an empty string, not a crash", "", hype_cfg_vm_display_name(0));
}

/*
 * #357: the VM summary printed `target=file:` for every VM, so storage-by-reference read as a
 * configured-but-EMPTY file target -- an empty path looks like a truncated value, which is worse
 * than saying nothing.
 */
static void test_disks_by_reference_is_not_a_target_disk(void) {
    hype_cfg_t c;
    hype_cfg_result_t r = parse_copy("[hype]\nconfig_version = 1\n"
                                     "[disk.a]\ntype = disk\npath = \\a.img\n"
                                     "[disk.b]\ntype = disk\npath = \\b.img\n"
                                     "[vm.twodisk]\nvcpus = 1\nmem_mb = 1024\n"
                                     "boot = disk\nfirmware = uefi\nos_hint = linux\n"
                                     "net_mode = none\ndisks = a, b\n",
                                     &c);
    CHECK_INT("parses", HYPE_CFG_OK, r.status);
    CHECK_INT("two disks referenced", 2, (int)c.vms[0].disks_count);
    CHECK_INT("and NO target_disk is configured", 0, hype_cfg_vm_has_target_disk(&c.vms[0]));
}

/* A cdrom-only VM is legal (§7), and it is storage by reference too -- so "has no target disk"
 * must not be read as "has no storage". */
static void test_a_cdrom_only_vm_has_no_target_disk_but_has_storage(void) {
    hype_cfg_t c;
    hype_cfg_result_t r = parse_copy("[hype]\nconfig_version = 1\n"
                                     "[disk.live]\ntype = cdrom\npath = \\iso\\test.iso\n"
                                     "[vm.livecd]\nvcpus = 1\nmem_mb = 1024\n"
                                     "boot = disk\nfirmware = uefi\nos_hint = linux\n"
                                     "cdroms = live\n",
                                     &c);
    CHECK_INT("parses", HYPE_CFG_OK, r.status);
    CHECK_INT("no target_disk", 0, hype_cfg_vm_has_target_disk(&c.vms[0]));
    CHECK_INT("no disks", 0, (int)c.vms[0].disks_count);
    CHECK_INT("but one cdrom", 1, (int)c.vms[0].cdroms_count);
}

static void test_a_configured_target_disk_is_reported_as_one(void) {
    hype_cfg_t c;
    hype_cfg_result_t r = parse_copy("[hype]\nconfig_version = 1\n[vm.a]\n" REQ, &c);
    CHECK_INT("parses", HYPE_CFG_OK, r.status);
    CHECK_INT("the REQ fixture's target_disk is seen", 1, hype_cfg_vm_has_target_disk(&c.vms[0]));
    CHECK_INT("a NULL vm has no target disk", 0, hype_cfg_vm_has_target_disk(0));
}

static void test_over_long_label_is_refused_not_truncated(void) {
    hype_cfg_t c;
    char text[512];
    unsigned i;
    strcpy(text, "[hype]\nconfig_version = 1\n[vm.a]\n" REQ "label = ");
    for (i = 0; i < HYPE_CFG_LABEL_MAX + 8u; i++) strcat(text, "x");
    strcat(text, "\n");
    /* Silently truncating a display name would show the operator something they did not write. */
    CHECK_INT("an over-long label is refused", HYPE_CFG_ERR_VALUE_TOO_LONG,
              parse_copy(text, &c).status);
}

/*
 * #357 part 2: the old warning told the operator "a misspelled key looks exactly like this" and
 * then did not say which line, which is unusable in a long config.
 */
static void test_first_unknown_line_is_named_with_its_number(void) {
    hype_cfg_t c;
    hype_cfg_result_t r = parse_copy("[hype]\nconfig_version = 1\n"
                                     "[vm.a]\n" REQ "vcpuss = 2\nnonsense = 1\n",
                                     &c);
    CHECK_INT("unknown keys are retained, not fatal", HYPE_CFG_OK, r.status);
    CHECK_INT("both are counted", 2, (int)c.unknown_count);
    CHECK_INT("the FIRST one is reported", 10, (int)c.unknown_first_line);
    CHECK_STR("and named verbatim", "vcpuss = 2", c.unknown_first);
}

static void test_no_unknown_lines_leaves_the_report_empty(void) {
    hype_cfg_t c;
    (void)parse_copy("[hype]\nconfig_version = 1\n[vm.a]\n" REQ, &c);
    CHECK_INT("no unknown lines", 0, (int)c.unknown_count);
    CHECK_INT("so no line number", 0, (int)c.unknown_first_line);
    CHECK_STR("and no text", "", c.unknown_first);
}

/* The comment retained with an unknown line must not be lost from the report. */
static void test_unknown_line_is_reported_with_its_comment(void) {
    hype_cfg_t c;
    (void)parse_copy("[hype]\nconfig_version = 1\n[vm.a]\n" REQ "bogus = 1 ; why\n", &c);
    CHECK_INT("counted", 1, (int)c.unknown_count);
    CHECK_STR("reported as the operator wrote it", "bogus = 1 ; why", c.unknown_first);
}

/* ==================== CONFIG-3 (#221): serializer ==================== */

static void test_serialize_round_trip_full_example(void) {
    const char *cfg =
        "[vm.win11]\n"
        "vcpus = 4\n"
        "cpu_set = 4-7\n"
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
    hype_cfg_t before, after;
    hype_cfg_serialize_result_t sr;
    hype_cfg_result_t pr;
    char buf[8192];

    pr = parse_copy(cfg, &before);
    CHECK_INT("original parses OK", HYPE_CFG_OK, pr.status);

    sr = hype_cfg_serialize(&before, buf, sizeof(buf));
    CHECK_INT("did not refuse", 0, sr.refused_overflow);
    CHECK_INT("did not truncate", 0, sr.truncated);

    pr = hype_cfg_parse(buf, &after);
    CHECK_INT("serialized text re-parses OK", HYPE_CFG_OK, pr.status);
    CHECK_INT("same VM count", (int)before.vm_count, (int)after.vm_count);

    CHECK_STR("vm0 name survives", "win11", after.vms[0].name);
    CHECK_INT("vm0 vcpus survives", 4, after.vms[0].vcpus);
    CHECK_INT("vm0 cpu_set_count survives", 4, after.vms[0].cpu_set_count);
    CHECK_INT("vm0 cpu_set[0] survives", 4, after.vms[0].cpu_set[0]);
    CHECK_INT("vm0 cpu_set[3] survives", 7, after.vms[0].cpu_set[3]);
    CHECK_INT("vm0 mem_mb survives", 8192, after.vms[0].mem_mb);
    CHECK_STR("vm0 install_media survives", "\\EFI\\hype\\win11.iso", after.vms[0].install_media);
    CHECK_INT("vm0 target_disk kind survives", (int)HYPE_CFG_DISK_FILE,
              (int)after.vms[0].target_disk.kind);
    CHECK_STR("vm0 target_disk path survives", "\\hype\\disks\\win11.img",
              after.vms[0].target_disk.path_or_id);
    CHECK_INT("vm0 target_disk_size_gb survives", 128, after.vms[0].target_disk_size_gb);
    CHECK_INT("vm0 os_hint survives", (int)HYPE_CFG_OS_WINDOWS, (int)after.vms[0].os_hint);
    CHECK_INT("vm0 net_mode survives", (int)HYPE_CFG_NET_NAT, (int)after.vms[0].net_mode);

    CHECK_STR("vm1 name survives", "debian", after.vms[1].name);
    CHECK_INT("vm1 target_disk kind physical survives", (int)HYPE_CFG_DISK_PHYSICAL,
              (int)after.vms[1].target_disk.kind);
    CHECK_STR("vm1 target_disk id survives", "SN-WDC-1234567890",
              after.vms[1].target_disk.path_or_id);
    CHECK_INT("vm1 net_peers_count survives", 1, after.vms[1].net_peers_count);
    CHECK_STR("vm1 net_peers[0] survives", "freebsd", after.vms[1].net_peers[0]);
    CHECK_INT("vm1 has_cpu_set stays false", 0, after.vms[1].has_cpu_set);

    CHECK_STR("vm2 name survives", "freebsd", after.vms[2].name);
    CHECK_INT("vm2 os_hint survives", (int)HYPE_CFG_OS_BSD, (int)after.vms[2].os_hint);
}

/* An edit lands as a struct-value change before serializing -- the whole reason CONFIG-3 exists is
 * the GUI/TUI editing the in-memory model and saving it, not just re-emitting an unchanged parse. */
static void test_serialize_reflects_an_in_memory_edit(void) {
    hype_cfg_t before, after;
    hype_cfg_serialize_result_t sr;
    char buf[4096];

    (void)parse_copy("[vm.a]\n" REQ, &before);
    before.vms[0].mem_mb = 2048;
    before.vms[0].vcpus = 4;

    sr = hype_cfg_serialize(&before, buf, sizeof(buf));
    CHECK_INT("did not refuse", 0, sr.refused_overflow);
    (void)hype_cfg_parse(buf, &after);
    CHECK_INT("edited mem_mb round-trips", 2048, after.vms[0].mem_mb);
    CHECK_INT("edited vcpus round-trips", 4, after.vms[0].vcpus);
}

static void test_serialize_preserves_comments_section_order_and_unknown_content(void) {
    const char *cfg =
        "; a leading comment before any section\n"
        "[hype]\n"
        "config_version = 1\n"
        "future_hype_key = surprise ; an unknown [hype] key, retained\n"
        "[future.eth0] ; an entirely unknown section kind\n"
        "mode = bridge\n"
        "[vm.a] ; trailing comment on a section header\n"
        "; a comment inside the VM section\n"
        REQ
        "future_vm_key = 1 ; an unknown VM key\n";
    hype_cfg_t before, after;
    hype_cfg_serialize_result_t sr;
    hype_cfg_result_t pr;
    char buf[4096];

    pr = parse_copy(cfg, &before);
    CHECK_INT("parses OK (unknown content is retained, not fatal)", HYPE_CFG_OK, pr.status);
    /* The unknown SECTION header itself counts too, alongside its one key and the two unknown
     * keys inside known sections: [future.eth0] + mode=bridge + future_hype_key + future_vm_key. */
    CHECK_INT("four unknown lines counted", 4, (int)before.unknown_count);

    sr = hype_cfg_serialize(&before, buf, sizeof(buf));
    CHECK_INT("did not refuse", 0, sr.refused_overflow);
    CHECK_INT("did not truncate", 0, sr.truncated);

    CHECK_INT("leading comment survives", 1, strstr(buf, "; a leading comment before any section") != 0);
    CHECK_INT("unknown [hype] key survives verbatim",
              1, strstr(buf, "future_hype_key = surprise ; an unknown [hype] key, retained") != 0);
    CHECK_INT("unknown section header survives",
              1, strstr(buf, "[future.eth0] ; an entirely unknown section kind") != 0);
    CHECK_INT("unknown section's key survives", 1, strstr(buf, "mode = bridge") != 0);
    CHECK_INT("VM section header comment survives",
              1, strstr(buf, "[vm.a] ; trailing comment on a section header") != 0);
    CHECK_INT("comment inside the VM section survives",
              1, strstr(buf, "; a comment inside the VM section") != 0);
    CHECK_INT("unknown VM key survives verbatim",
              1, strstr(buf, "future_vm_key = 1 ; an unknown VM key") != 0);

    /* Section order itself: [hype] before [future.eth0] before [vm.a]. */
    {
        const char *p_hype = strstr(buf, "[hype]");
        const char *p_nic = strstr(buf, "[future.eth0]");
        const char *p_vm = strstr(buf, "[vm.a]");
        CHECK_INT("all three sections found", 1, p_hype != 0 && p_nic != 0 && p_vm != 0);
        CHECK_INT("hype before nic", 1, p_hype < p_nic);
        CHECK_INT("nic before vm", 1, p_nic < p_vm);
    }

    pr = hype_cfg_parse(buf, &after);
    CHECK_INT("re-parses OK", HYPE_CFG_OK, pr.status);
    CHECK_INT("same unknown-line count after round trip", 4, (int)after.unknown_count);
}

static void test_serialize_refuses_on_retained_overflow(void) {
    hype_cfg_t c;
    hype_cfg_serialize_result_t sr;
    char buf[64];

    (void)parse_copy("[vm.a]\n" REQ, &c);
    c.retained_overflow = 1; /* simulate the original parse having lost content */
    buf[0] = 'X'; /* poison, so "untouched" is distinguishable from "happens to be empty" */

    sr = hype_cfg_serialize(&c, buf, sizeof(buf));
    CHECK_INT("refuses rather than writing an incomplete file", 1, sr.refused_overflow);
    CHECK_INT("out buffer left untouched", 'X', buf[0]);
}

static void test_serialize_truncation_is_reported_not_silent(void) {
    hype_cfg_t c;
    hype_cfg_serialize_result_t sr;
    char tiny[8];

    (void)parse_copy("[vm.a]\n" REQ, &c);
    sr = hype_cfg_serialize(&c, tiny, sizeof(tiny));
    CHECK_INT("a too-small buffer is reported truncated", 1, sr.truncated);
}

static void test_serialize_disk_section_round_trips_optional_fields(void) {
    const char *cfg =
        "[disk.sys]\n"
        "type = disk\n"
        "backing = file\n"
        "path = \\hype\\disks\\a.img\n"
        "format = qcow2\n"
        "size_gb = 40\n"
        "bus = virtio-blk\n"
        "read_only = false\n"
        "[disk.phys]\n"
        "type = disk\n"
        "backing = physical\n"
        "id_match = SN-1234\n"
        "allow_overwrite = true\n"
        "partition = 2\n";
    hype_cfg_t before, after;
    hype_cfg_serialize_result_t sr;
    char buf[4096];

    (void)parse_copy(cfg, &before);
    CHECK_INT("two disks parsed", 2, (int)before.disk_count);

    sr = hype_cfg_serialize(&before, buf, sizeof(buf));
    CHECK_INT("did not refuse", 0, sr.refused_overflow);
    (void)hype_cfg_parse(buf, &after);

    CHECK_INT("disk0 format survives", (int)HYPE_CFG_FORMAT_QCOW2, (int)after.disks[0].format);
    CHECK_INT("disk0 size_gb survives", 40, after.disks[0].size_gb);
    CHECK_INT("disk0 bus survives", (int)HYPE_CFG_BUS_VIRTIO_BLK, (int)after.disks[0].bus);
    CHECK_STR("disk0 path survives", "\\hype\\disks\\a.img", after.disks[0].path);

    CHECK_INT("disk1 backing physical survives", (int)HYPE_CFG_BACKING_PHYSICAL,
              (int)after.disks[1].backing);
    CHECK_STR("disk1 id_match survives", "SN-1234", after.disks[1].id_match);
    CHECK_INT("disk1 allow_overwrite survives", 1, after.disks[1].allow_overwrite);
    CHECK_INT("disk1 partition survives", 2, after.disks[1].partition);
    /* disk1 never set `bus` -- HYPE_CFG_BUS_DEFAULT has no textual form, so the key must be
     * omitted rather than emitted as something that would fail to reparse. */
    CHECK_INT("disk1 bus stays default", (int)HYPE_CFG_BUS_DEFAULT, (int)after.disks[1].bus);
}

static void test_serialize_hype_section_lists(void) {
    const char *cfg = "[hype]\n"
                      "host_cpu_budget = 1-3\n"
                      "default_net_mode = nat\n"
                      "dashboard_default_view = vm:alpine\n"
                      "autostart = alpine,beta\n"
                      "cpu_avg_window_secs = 5\n"
                      "[vm.alpine]\n" REQ "[vm.beta]\n" REQ;
    hype_cfg_t before, after;
    hype_cfg_serialize_result_t sr;
    char buf[4096];

    (void)parse_copy(cfg, &before);
    sr = hype_cfg_serialize(&before, buf, sizeof(buf));
    CHECK_INT("did not refuse", 0, sr.refused_overflow);
    (void)hype_cfg_parse(buf, &after);

    CHECK_INT("host_cpu_budget count survives (expanded)", 3, after.hype.host_cpu_budget_count);
    CHECK_INT("host_cpu_budget[0] survives", 1, after.hype.host_cpu_budget[0]);
    CHECK_INT("host_cpu_budget[2] survives", 3, after.hype.host_cpu_budget[2]);
    CHECK_INT("default_net_mode survives", (int)HYPE_CFG_NET_NAT, (int)after.hype.default_net_mode);
    CHECK_INT("dashboard_default_view survives", (int)HYPE_CFG_VIEW_VM,
              (int)after.hype.dashboard_default_view);
    CHECK_STR("dashboard_default_vm survives", "alpine", after.hype.dashboard_default_vm);
    CHECK_INT("autostart survives as a list", (int)HYPE_CFG_AUTOSTART_LIST,
              (int)after.hype.autostart);
    CHECK_INT("autostart_count survives", 2, after.hype.autostart_count);
    CHECK_STR("autostart_vms[0] survives", "alpine", after.hype.autostart_vms[0]);
    CHECK_STR("autostart_vms[1] survives", "beta", after.hype.autostart_vms[1]);
    CHECK_INT("cpu_avg_window_secs survives", 5, (int)after.hype.cpu_avg_window_secs);
}

/* A config that never set `resolution` at all must not gain one from serializing -- the key
 * itself must be omitted, not written as some zero/default value. */

/* ---- #393: no compile-time VM cap ---- */

static void test_count_vms_counts_declarations(void) {
    const char *cfg =
        "[hype]\nconfig_version = 1\n"
        "[vm.a]\n" REQ
        "[disk.d0]\npath = file:\\x.img\n"
        "  [vm.b]\n" REQ          /* leading whitespace still counts */
        "[vm.c]\n" REQ;
    CHECK_INT("three [vm.*] sections are counted", 3, (int)hype_cfg_count_vms(cfg));
    CHECK_INT("a config with no VMs counts zero", 0, (int)hype_cfg_count_vms("[hype]\n"));
}

/* The old parser refused VM #17 because its array was 16 long. With storage bound by the
 * caller, the bound is whatever the caller sized -- which is what lets hype size from the
 * host's real topology instead of a constant. */
static void test_parse_into_accepts_more_vms_than_the_default(void) {
    static char buf[32768];
    static hype_cfg_vm_t storage[24];
    hype_cfg_t out;
    hype_cfg_result_t res;
    unsigned i;
    unsigned pos = 0;
    for (i = 0; i < 20; i++) {
        pos += (unsigned)snprintf(buf + pos, sizeof(buf) - pos, "[vm.v%u]\n%s", i, REQ);
    }
    CHECK_INT("twenty declared", 20, (int)hype_cfg_count_vms(buf));
    res = hype_cfg_parse_into(buf, &out, storage, 24u);
    CHECK_INT("parse succeeds past the old 16 cap", (int)HYPE_CFG_OK, (int)res.status);
    CHECK_INT("all twenty VMs parsed", 20, (int)out.vm_count);
    CHECK_STR("the seventeenth VM is real", "v16", out.vms[16].name);
}

/* Bounded storage still refuses rather than overrunning -- the cap moved, it did not vanish. */
static void test_parse_into_refuses_past_bound_storage(void) {
    static char buf[32768];
    static hype_cfg_vm_t storage[3];
    hype_cfg_t out;
    hype_cfg_result_t res;
    unsigned i, pos = 0;
    for (i = 0; i < 5; i++) {
        pos += (unsigned)snprintf(buf + pos, sizeof(buf) - pos, "[vm.v%u]\n%s", i, REQ);
    }
    res = hype_cfg_parse_into(buf, &out, storage, 3u);
    /*
     * #450: BOUNDED, not rejected. Returning an error here aborted the whole parse, so a config
     * with one VM too many was discarded entirely and hype fell back to its built-in two --
     * measured as 'parse error (status=7, line=148)' on a 20-VM config against 16 slots. The VMs
     * that fit are kept and the rest are named through the skipped-VM channel.
     */
    CHECK_INT("a config past capacity still parses", (int)HYPE_CFG_OK, (int)res.status);
    CHECK_INT("the VMs that fit are kept", 3, (int)out.vm_count);
    CHECK_INT("the two that do not fit are reported", 2, (int)out.skipped_vms);
    CHECK_STR("and the first of them is named", "v3", out.skipped_vm_name);
}

/* #533: hype_cfg_init() must apply SAFE defaults, not zeroes. HYPE_LOG_ERROR is 0, so a plain
 * memset made the quietest log level the default -- on exactly the hosts with no readable config,
 * whose logs matter most. */
static void test_cfg_init_gives_safe_defaults_not_zeroes(void) {
    hype_cfg_t c;
    hype_cfg_init(&c);
    CHECK_INT("log_level defaults to debug, not the zero enum", (int)HYPE_LOG_DEBUG,
              (int)c.hype.log_level);
    CHECK_INT("config_version defaults to 1", 1, (int)c.hype.config_version);
    CHECK_INT("cpu_avg_window_secs defaults to 1", 1, (int)c.hype.cpu_avg_window_secs);
    CHECK_INT("no VMs yet", 0, (int)c.vm_count);
    CHECK_INT("but storage is bound", (int)HYPE_CFG_MAX_VMS, (int)c.vm_cap);
}

/* And the key itself round-trips. */
static void test_log_level_key(void) {
    hype_cfg_t c;
    hype_cfg_result_t r;
    char buf[2048];
    snprintf(buf, sizeof(buf), "[hype]\nlog_level = info\n%s", VM_A);
    r = parse_copy(buf, &c);
    CHECK_INT("info parses", HYPE_CFG_OK, r.status);
    CHECK_INT("and takes", (int)HYPE_LOG_INFO, (int)c.hype.log_level);

    snprintf(buf, sizeof(buf), "[hype]\nlog_level = shouty\n%s", VM_A);
    r = parse_copy(buf, &c);
    CHECK_INT("a bad level is malformation, like any other bad [hype] value", 1,
              (int)c.hype.malformed);
    CHECK_INT("and the level stays at the loudest", (int)HYPE_LOG_DEBUG, (int)c.hype.log_level);

    snprintf(buf, sizeof(buf), "[hype]\n%s", VM_A);
    r = parse_copy(buf, &c);
    CHECK_INT("an absent key means debug", (int)HYPE_LOG_DEBUG, (int)c.hype.log_level);
}


/*
 * #567: write-back was refused on hype's OWN shipped config, and the operator was told the config
 * was "too large" when it was not. Two independent bugs behind one message.
 *
 * Comment-only lines are retained because a lossless serializer must preserve them, and the cap was
 * 64 -- roughly one screen. tools/hwstick/hype.cfg has 68 comment lines, so `retained_overflow` was
 * set at parse time on the validation stick and create/set/any write-back could never succeed
 * there.
 */
static char g_big[262144];
static char g_out[262144];

/* Build a config with `comments` comment-only lines plus one valid VM. */
static void build_commented_cfg(unsigned comments) {
    char *p = g_big;
    unsigned i;
    p += sprintf(p, "[hype]\nconfig_version = 1\n");
    for (i = 0; i < comments; i++) {
        p += sprintf(p, "; comment line number %u, of the kind an operator is supposed to write\n", i);
    }
    p += sprintf(p, "[vm.one]\n" REQ);
    *p = '\0';
}

static void test_retained_overflow_is_not_a_size_problem(void) {
    static hype_cfg_t cfg;
    hype_cfg_serialize_result_t sr;

    /* One MORE than the cap, so the parser cannot capture every line. */
    build_commented_cfg((unsigned)HYPE_CFG_MAX_RETAINED + 1u);
    (void)hype_cfg_parse(g_big, &cfg);
    CHECK_INT("over the cap sets retained_overflow", 1, cfg.retained_overflow);

    sr = hype_cfg_serialize(&cfg, g_out, sizeof(g_out));
    /*
     * THE DISTINCTION THAT WAS MISSING. refused_overflow means "saving would delete content the
     * parser never captured"; truncated means "the output buffer was too small". Only the second is
     * a size problem, and the buffer here is 256 KiB, so it must NOT be set. Reporting these as one
     * message is what told the operator to shrink a config that fitted comfortably.
     */
    CHECK_INT("serialize refuses", 1, sr.refused_overflow);
    CHECK_INT("and NOT because of size", 0, sr.truncated);
    /* `out` is untouched rather than half-written, so a caller cannot save a partial file. */
    CHECK_INT("out is left empty", 0, (int)strlen(g_out));
}

static void test_truncated_is_reported_separately_from_refusal(void) {
    static hype_cfg_t cfg;
    static char tiny[64];
    hype_cfg_serialize_result_t sr;

    /* Well under the cap, so nothing is refused -- only the buffer is too small. */
    build_commented_cfg(8u);
    (void)hype_cfg_parse(g_big, &cfg);
    CHECK_INT("under the cap retains everything", 0, cfg.retained_overflow);

    sr = hype_cfg_serialize(&cfg, tiny, sizeof(tiny));
    CHECK_INT("a small buffer sets truncated", 1, sr.truncated);
    CHECK_INT("and NOT refused_overflow", 0, sr.refused_overflow);
}

/*
 * The shipped validation-stick config's shape: 68 comment lines. This is the case that could never
 * be written back, and it is asserted by COUNT rather than by reading the file, so the test does not
 * depend on tools/hwstick/hype.cfg staying exactly 68 lines long -- what matters is that a config
 * documented at that scale round-trips.
 */
static void test_a_documented_config_round_trips(void) {
    static hype_cfg_t cfg;
    static hype_cfg_t again;
    hype_cfg_serialize_result_t sr;
    unsigned i;
    unsigned comments_out = 0u;

    build_commented_cfg(68u);
    (void)hype_cfg_parse(g_big, &cfg);
    CHECK_INT("68 comment lines are all retained", 68, (int)cfg.retained_count);
    CHECK_INT("no overflow at the shipped config's scale", 0, cfg.retained_overflow);

    sr = hype_cfg_serialize(&cfg, g_out, sizeof(g_out));
    CHECK_INT("it serializes", 0, sr.refused_overflow);
    CHECK_INT("without truncation", 0, sr.truncated);

    /* Every comment must come back out, or "lossless" is not what happened. */
    for (i = 0; g_out[i] != '\0'; i++) {
        if (g_out[i] == ';' && (i == 0u || g_out[i - 1u] == '\n')) {
            comments_out++;
        }
    }
    CHECK_INT("every comment survives the write-back", 68, (int)comments_out);

    /* Re-parsing the output must give the same thing: a serializer that is not idempotent loses
     * content on the second save rather than the first, which is worse to diagnose. */
    (void)hype_cfg_parse(g_out, &again);
    CHECK_INT("reparse retains the same count", (int)cfg.retained_count, (int)again.retained_count);
    CHECK_INT("reparse does not overflow", 0, again.retained_overflow);
}

/*
 * The wizard's actual path: take a documented config, append a VM, serialize. This is what
 * term_create_finish() does, and it is the step that failed on bare metal.
 */
static void test_appending_a_vm_to_a_documented_config_writes_back(void) {
    static hype_cfg_t cfg;
    hype_cfg_vm_t nv;
    hype_cfg_serialize_result_t sr;

    build_commented_cfg(68u);
    (void)hype_cfg_parse(g_big, &cfg);
    CHECK_INT("baseline has no overflow", 0, cfg.retained_overflow);

    memset(&nv, 0, sizeof nv);
    strncpy(nv.name, "created", sizeof nv.name - 1);
    nv.mem_mb = 512u;
    nv.vcpus = 1u;
    nv.boot = HYPE_CFG_BOOT_DISK;
    nv.os_hint = HYPE_CFG_OS_LINUX;
    nv.target_disk.kind = HYPE_CFG_DISK_FILE;
    strncpy(nv.target_disk.path_or_id, "\\hype\\disks\\new.img",
            sizeof nv.target_disk.path_or_id - 1);

    CHECK_INT("the VM is appended", 0, hype_cfg_append_vm(&cfg, &nv));
    sr = hype_cfg_serialize(&cfg, g_out, sizeof(g_out));
    CHECK_INT("the wizard's write-back is not refused", 0, sr.refused_overflow);
    CHECK_INT("nor truncated", 0, sr.truncated);
    CHECK_INT("and the new VM is in the config", 2, (int)cfg.vm_count);
    CHECK_INT("the serialized text names it", 1, strstr(g_out, "[vm.created]") != 0);
}

/*
 * The cap must stay consistent with the buffer size cfg.h tells callers to provide. A config built
 * to every structural maximum has to serialize into 64 KiB, or the guidance is a lie and the next
 * caller sizes a buffer from it. This is the check that pins the 256 chosen in #567: at 384 the
 * worst case is 76.9 KiB and this fails.
 */
static void test_worst_case_config_fits_the_documented_buffer(void) {
    static hype_cfg_t cfg;
    static char buf64k[65536];
    hype_cfg_serialize_result_t sr;
    char *p = g_big;
    unsigned i;

    p += sprintf(p, "[hype]\nconfig_version = 1\n");
    for (i = 0; i < (unsigned)HYPE_CFG_MAX_RETAINED; i++) {
        unsigned k;
        *p++ = ';';
        for (k = 1u; k < (unsigned)HYPE_CFG_LINE_MAX - 2u; k++) {
            *p++ = 'x';
        }
        *p++ = '\n';
    }
    for (i = 0; i < (unsigned)HYPE_CFG_MAX_VMS; i++) {
        p += sprintf(p, "[vm.vmnamenumber%02u]\n" REQ, i);
    }
    *p = '\0';

    (void)hype_cfg_parse(g_big, &cfg);
    CHECK_INT("the maximal config retains without overflow", 0, cfg.retained_overflow);
    sr = hype_cfg_serialize(&cfg, buf64k, sizeof(buf64k));
    CHECK_INT("the worst case fits the 64 KiB cfg.h documents", 0, sr.truncated);
    CHECK_INT("and is not refused", 0, sr.refused_overflow);
}

/* ---- #583 (§5.5): [nic.*] and [switch.*] ---- */

static void test_nic_and_switch_sections_parse(void) {
    char cfg[2048];
    hype_cfg_t out;
    hype_cfg_result_t res;

    snprintf(cfg, sizeof(cfg), "%s%s", VM_A,
             "[switch.lan0]\n"
             "uplink = nat\n"
             "[nic.a-eth0]\n"
             "switch = lan0\n"
             "mac = 52:54:00:12:34:56\n"
             "[nic.private]\n");
    res = parse_copy(cfg, &out);

    CHECK_INT("the sections parse", HYPE_CFG_OK, res.status);
    CHECK_INT("nothing retained as unknown -- these are KNOWN kinds now", 0, out.unknown_count);
    CHECK_INT("one switch", 1, out.switch_count);
    CHECK_STR("switch id", "lan0", out.switches[0].id);
    CHECK_INT("uplink nat", (int)HYPE_CFG_UPLINK_NAT, (int)out.switches[0].uplink);
    CHECK_INT("two nics", 2, out.nic_count);
    CHECK_STR("nic id", "a-eth0", out.nics[0].id);
    CHECK_INT("nic has a switch", 1, out.nics[0].has_switch);
    CHECK_STR("nic switch id", "lan0", out.nics[0].switch_id);
    CHECK_INT("nic has a mac", 1, out.nics[0].has_mac);
    CHECK_INT("mac byte 0", 0x52, out.nics[0].mac[0]);
    CHECK_INT("mac byte 5", 0x56, out.nics[0].mac[5]);
    /* §5.5: an empty [nic.*] is a complete configuration -- its own private isolated segment. */
    CHECK_INT("the switchless nic has no switch", 0, out.nics[1].has_switch);
    CHECK_INT("and no mac either", 0, out.nics[1].has_mac);
}

/* §5.1's default: a switch with no `uplink` is PRIVATE. Isolation by default (§6e), so the absence
 * of the key must not read as "nat". */
static void test_switch_uplink_defaults_to_none(void) {
    char cfg[1024];
    hype_cfg_t out;

    snprintf(cfg, sizeof(cfg), "%s%s", VM_A, "[switch.priv]\n");
    (void)parse_copy(cfg, &out);
    CHECK_INT("one switch", 1, out.switch_count);
    CHECK_INT("uplink defaults to none", (int)HYPE_CFG_UPLINK_NONE, (int)out.switches[0].uplink);
    CHECK_INT("and says it was not set", 0, out.switches[0].has_uplink);
}

static void test_vm_nics_list(void) {
    char cfg[2048];
    hype_cfg_t out;
    hype_cfg_result_t res;

    snprintf(cfg, sizeof(cfg), "%s%s", VM_A,
             "nics = eth0, eth1\n"
             "[nic.eth0]\n[nic.eth1]\n");
    res = parse_copy(cfg, &out);
    CHECK_INT("parses", HYPE_CFG_OK, res.status);
    CHECK_INT("two nics attached", 2, out.vms[0].nics_count);
    CHECK_STR("first", "eth0", out.vms[0].nics[0]);
    CHECK_STR("second", "eth1", out.vms[0].nics[1]);
    /* Whether those ids EXIST is admission's question, not the parser's -- the same split `disks`
     * follows, and it is what lets a config name a device defined further down the file. */
    CHECK_INT("nothing retained", 0, out.unknown_count);
}

/*
 * A MAC is an identity the forwarding plane keys guests off (#81), so a half-parsed one that
 * silently became 00:00:00:00:00:00 would make two guests indistinguishable to every mapping. Every
 * malformed form is refused, and the multicast bit by name -- a multicast source address is not an
 * identity at all.
 */
static void test_bad_mac_is_refused(void) {
    static const char *bad[] = {
        "52:54:00:12:34",        /* too few octets */
        "52:54:00:12:34:56:78",  /* too many */
        "52-54-00-12-34-56",     /* wrong separator */
        "52:54:00:12:34:5g",     /* not hex */
        "525400123456",          /* no separators */
        "53:54:00:12:34:56",     /* multicast bit set in the first octet */
    };
    unsigned int i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char cfg[1024];
        hype_cfg_t out;
        snprintf(cfg, sizeof(cfg), "%s[nic.e]\nmac = %s\n", VM_A, bad[i]);
        (void)parse_copy(cfg, &out);
        /* §4.3: a malformed [nic.*] key is a BAD VALUE inside a known section, so the section is
         * kept and the key is not applied -- what must never happen is a MAC that looks accepted. */
        CHECK_INT("a malformed mac is never applied", 0, out.nics[0].has_mac);
    }
    /* And the good form still works, so the rejections are not a blanket refusal. */
    {
        char cfg[1024];
        hype_cfg_t out;
        snprintf(cfg, sizeof(cfg), "%s[nic.e]\nmac = 02:00:00:00:00:01\n", VM_A);
        (void)parse_copy(cfg, &out);
        CHECK_INT("a valid locally-administered unicast mac is applied", 1, out.nics[0].has_mac);
        CHECK_INT("first octet", 0x02, out.nics[0].mac[0]);
    }
}

static void test_duplicate_nic_and_switch_ids_refused(void) {
    char cfg[1024];
    hype_cfg_t out;
    hype_cfg_result_t res;

    snprintf(cfg, sizeof(cfg), "%s[nic.e]\n[nic.e]\n", VM_A);
    res = parse_copy(cfg, &out);
    CHECK_INT("a duplicated nic id is refused", HYPE_CFG_ERR_DUPLICATE_VM_NAME, res.status);

    snprintf(cfg, sizeof(cfg), "%s[switch.w]\n[switch.w]\n", VM_A);
    res = parse_copy(cfg, &out);
    CHECK_INT("a duplicated switch id is refused", HYPE_CFG_ERR_DUPLICATE_VM_NAME, res.status);
}

static void test_nameless_nic_section_refused(void) {
    char cfg[1024];
    hype_cfg_t out;
    hype_cfg_result_t res;

    snprintf(cfg, sizeof(cfg), "%s[nic.]\n", VM_A);
    res = parse_copy(cfg, &out);
    CHECK_INT("an id-less [nic.] is refused -- nothing could reference it",
              HYPE_CFG_ERR_BAD_VALUE, res.status);
}

/* An unknown key inside a KNOWN [nic.*] is retained, not fatal -- §4.1's keystone, so a NIC key from
 * a newer hype does not stop an older one booting. */
static void test_unknown_nic_key_is_retained(void) {
    char cfg[1024];
    hype_cfg_t out;
    hype_cfg_result_t res;

    snprintf(cfg, sizeof(cfg), "%s[nic.e]\nswitch = w\nfuture_nic_key = 7\n[switch.w]\n", VM_A);
    res = parse_copy(cfg, &out);
    CHECK_INT("parses", HYPE_CFG_OK, res.status);
    CHECK_INT("the unknown key is retained", 1, out.unknown_count);
    CHECK_INT("and the known one still applied", 1, out.nics[0].has_switch);
}

/* Round trip: what the serializer writes must parse back to the same model, or a GUI write-back
 * silently changes the network. */
static void test_nic_switch_round_trip(void) {
    char cfg[2048];
    char buf[4096];
    hype_cfg_t a, b;
    hype_cfg_serialize_result_t sr;

    snprintf(cfg, sizeof(cfg), "%s%s", VM_A,
             "nics = e0\n"
             "[switch.lan0]\nuplink = nat\n"
             "[nic.e0]\nswitch = lan0\nmac = 52:54:00:ab:cd:ef\n");
    (void)parse_copy(cfg, &a);
    sr = hype_cfg_serialize(&a, buf, sizeof(buf));
    CHECK_INT("serialized", 0, sr.truncated);
    (void)parse_copy(buf, &b);
    CHECK_INT("same nic count", (int)a.nic_count, (int)b.nic_count);
    CHECK_INT("same switch count", (int)a.switch_count, (int)b.switch_count);
    CHECK_STR("switch survives", a.nics[0].switch_id, b.nics[0].switch_id);
    CHECK_INT("mac survives byte 3", a.nics[0].mac[3], b.nics[0].mac[3]);
    CHECK_INT("uplink survives", (int)a.switches[0].uplink, (int)b.switches[0].uplink);
    CHECK_INT("the VM's nics list survives", (int)a.vms[0].nics_count, (int)b.vms[0].nics_count);
    CHECK_STR("by id", a.vms[0].nics[0], b.vms[0].nics[0]);
}

/* The parser's own ceilings. Refused, not silently truncated: a NIC that parses and is never
 * attached is the failure mode every cap in this file exists to avoid. */
static void test_nic_and_switch_caps(void) {
    char cfg[8192];
    hype_cfg_t out;
    hype_cfg_result_t res;
    unsigned int i;
    int n;

    n = snprintf(cfg, sizeof(cfg), "%s", VM_A);
    for (i = 0; i < HYPE_CFG_MAX_NICS + 1u; i++) {
        n += snprintf(cfg + n, sizeof(cfg) - (unsigned)n, "[nic.e%u]\n", i);
    }
    res = parse_copy(cfg, &out);
    CHECK_INT("one NIC past the cap is refused", HYPE_CFG_ERR_TOO_MANY_ENTRIES, res.status);

    n = snprintf(cfg, sizeof(cfg), "%s", VM_A);
    for (i = 0; i < HYPE_CFG_MAX_SWITCHES + 1u; i++) {
        n += snprintf(cfg + n, sizeof(cfg) - (unsigned)n, "[switch.w%u]\n", i);
    }
    res = parse_copy(cfg, &out);
    CHECK_INT("one switch past the cap is refused", HYPE_CFG_ERR_TOO_MANY_ENTRIES, res.status);

    /* And a VM attaching more NICs than the per-VM list holds. */
    n = snprintf(cfg, sizeof(cfg), "%s", VM_A);
    n += snprintf(cfg + n, sizeof(cfg) - (unsigned)n, "nics = a, b, c, d, e\n");
    res = parse_copy(cfg, &out);
    CHECK_INT("more attached NICs than the list holds is refused",
              HYPE_CFG_ERR_TOO_MANY_ENTRIES, res.status);
}

/* An id longer than the field. Refused rather than truncated, because a truncated id resolves to a
 * DIFFERENT device -- or to none -- and nothing would say so. */
static void test_over_long_nic_id_refused(void) {
    char cfg[1024];
    char id[HYPE_CFG_NAME_MAX + 8];
    hype_cfg_t out;
    hype_cfg_result_t res;
    unsigned int i;

    for (i = 0; i + 1u < sizeof(id); i++) id[i] = 'n';
    id[sizeof(id) - 1u] = '\0';
    snprintf(cfg, sizeof(cfg), "%s[nic.%s]\n", VM_A, id);
    res = parse_copy(cfg, &out);
    CHECK_INT("an over-long nic id is refused", HYPE_CFG_ERR_VALUE_TOO_LONG, res.status);

    snprintf(cfg, sizeof(cfg), "%s[nic.e]\nswitch = %s\n", VM_A, id);
    (void)parse_copy(cfg, &out);
    CHECK_INT("an over-long switch reference is never applied", 0, out.nics[0].has_switch);
}

/* A nameless [switch.] mirrors [nic.]: nothing could reference it. */
static void test_nameless_switch_section_refused(void) {
    char cfg[1024];
    hype_cfg_t out;
    hype_cfg_result_t res;
    snprintf(cfg, sizeof(cfg), "%s[switch.]\n", VM_A);
    res = parse_copy(cfg, &out);
    CHECK_INT("an id-less [switch.] is refused", HYPE_CFG_ERR_BAD_VALUE, res.status);
}

/* A duplicated key inside a [nic.*] or [switch.*]. Refused, so which value wins never depends on
 * file order -- the same rule every other section follows. */
static void test_duplicate_nic_keys_refused(void) {
    char cfg[1024];
    hype_cfg_t out;
    hype_cfg_result_t res;

    snprintf(cfg, sizeof(cfg), "%s[switch.w]\n[nic.e]\nswitch = w\nswitch = w\n", VM_A);
    res = parse_copy(cfg, &out);
    CHECK_INT("a repeated switch key is refused", HYPE_CFG_ERR_DUPLICATE_KEY, res.status);

    snprintf(cfg, sizeof(cfg), "%s[switch.w]\nuplink = nat\nuplink = none\n", VM_A);
    res = parse_copy(cfg, &out);
    CHECK_INT("a repeated uplink key is refused", HYPE_CFG_ERR_DUPLICATE_KEY, res.status);
}

/* A bad uplink VALUE is a bad value, not an unknown key -- it must not be quietly retained as
 * forward compatibility and leave the switch on its default. */
static void test_bad_uplink_value(void) {
    char cfg[1024];
    hype_cfg_t out;

    snprintf(cfg, sizeof(cfg), "%s[switch.w]\nuplink = bridge\n", VM_A);
    (void)parse_copy(cfg, &out);
    CHECK_INT("an unrecognised uplink is never applied", 0, out.switches[0].has_uplink);
}

/* The uplink=none spelling, and a MAC on a switchless NIC -- the two remaining shapes a real config
 * uses that nothing above exercises. */
static void test_uplink_none_and_switchless_mac(void) {
    char cfg[1024];
    hype_cfg_t out;

    snprintf(cfg, sizeof(cfg), "%s[switch.priv]\nuplink = none\n[nic.e]\nmac = 02:11:22:33:44:55\n",
             VM_A);
    (void)parse_copy(cfg, &out);
    CHECK_INT("uplink none is applied explicitly", 1, out.switches[0].has_uplink);
    CHECK_INT("and is NONE", (int)HYPE_CFG_UPLINK_NONE, (int)out.switches[0].uplink);
    CHECK_INT("a MAC without a switch is fine", 1, out.nics[0].has_mac);
    CHECK_INT("still its own private segment", 0, out.nics[0].has_switch);
    CHECK_INT("mac last octet", 0x55, out.nics[0].mac[5]);
}


/* --- #545: the initrd key -- same rules as kernel/cmdline, minus the legitimate-empty case --- */

static void test_initrd(void) {
    const char *cfg =
        "[vm.k]\nvcpus=1\nmem_mb=512\nboot=kernel\nkernel=\\k.bin\n"
        "initrd=\\EFI\\hype\\micro\\initramfs-virt\nos_hint=none\n";
    hype_cfg_t out;

    CHECK_INT("initrd parses on a kernel VM", HYPE_CFG_OK, parse_copy(cfg, &out).status);
    CHECK_INT("has_initrd set", 1, out.vms[0].has_initrd);
    CHECK_STR("initrd value", "\\EFI\\hype\\micro\\initramfs-virt", out.vms[0].initrd);
}

static void test_initrd_rules(void) {
    const char *nonkernel =
        "[vm.a]\nvcpus=1\nmem_mb=512\nboot=installer\ninstall_media=\\i.iso\n"
        "target_disk=file:\\a.img\nfirmware=uefi\ninitrd=\\x\nos_hint=linux\n";
    const char *empty =
        "[vm.k]\nvcpus=1\nmem_mb=512\nboot=kernel\nkernel=\\k.bin\ninitrd=\nos_hint=none\n";
    const char *dup =
        "[vm.k]\nvcpus=1\nmem_mb=512\nboot=kernel\nkernel=\\k.bin\n"
        "initrd=\\a\ninitrd=\\b\nos_hint=none\n";
    hype_cfg_t out;

    /* A firmware-boot VM has no kernel to hand an initrd to -- same rule as cmdline. */
    CHECK_INT("initrd on a non-kernel VM is refused", HYPE_CFG_ERR_BAD_VALUE,
              parse_copy(nonkernel, &out).status);
    /* A path, so empty is meaningless -- unlike cmdline's legitimate empty. */
    CHECK_INT("empty initrd refused", HYPE_CFG_ERR_BAD_VALUE, parse_copy(empty, &out).status);
    CHECK_INT("duplicate initrd refused", HYPE_CFG_ERR_DUPLICATE_KEY,
              parse_copy(dup, &out).status);
}

static void test_initrd_too_long_and_write_back(void) {
    static char cfg[HYPE_CFG_PATH_MAX + 256];
    static char out_text[8192];
    hype_cfg_t out;
    hype_cfg_serialize_result_t sr;
    unsigned i, n;

    strcpy(cfg, "[vm.k]\nvcpus=1\nmem_mb=512\nboot=kernel\nkernel=\\k.bin\ninitrd=");
    n = (unsigned)strlen(cfg);
    for (i = 0; i < HYPE_CFG_PATH_MAX; i++) {
        cfg[n + i] = 'x';
    }
    strcpy(cfg + n + i, "\nos_hint=none\n");
    CHECK_INT("an over-long initrd path is refused", HYPE_CFG_ERR_VALUE_TOO_LONG,
              parse_copy(cfg, &out).status);

    /* Round-trip, or write-back would silently drop it. */
    {
        const char *rt =
            "[vm.k]\nvcpus=1\nmem_mb=512\nboot=kernel\nkernel=\\k.bin\n"
            "initrd=\\ird.img\nos_hint=none\n";
        hype_cfg_t parsed;
        CHECK_INT("rt parses", HYPE_CFG_OK, parse_copy(rt, &parsed).status);
        sr = hype_cfg_serialize(&parsed, out_text, sizeof(out_text));
        CHECK_INT("serializes", 0, sr.refused_overflow || sr.truncated);
        CHECK_INT("initrd emitted", 1, strstr(out_text, "initrd = \\ird.img") != 0);
    }
}


static void test_tpm_key(void) {
    const char *on =
        "[vm.k]\nvcpus=1\nmem_mb=512\nboot=installer\ninstall_media=\\i.iso\n"
        "target_disk=file:\\a.img\nfirmware=uefi\ntpm=on\nos_hint=linux\n";
    const char *bad =
        "[vm.k]\nvcpus=1\nmem_mb=512\nboot=installer\ninstall_media=\\i.iso\n"
        "target_disk=file:\\a.img\nfirmware=uefi\ntpm=maybe\nos_hint=linux\n";
    const char *dup =
        "[vm.k]\nvcpus=1\nmem_mb=512\nboot=installer\ninstall_media=\\i.iso\n"
        "target_disk=file:\\a.img\nfirmware=uefi\ntpm=on\ntpm=off\nos_hint=linux\n";
    hype_cfg_t out;
    static char text[4096];
    hype_cfg_serialize_result_t sr;

    CHECK_INT("tpm=on parses", HYPE_CFG_OK, parse_copy(on, &out).status);
    CHECK_INT("tpm set", 1, out.vms[0].tpm);
    /* round-trips */
    sr = hype_cfg_serialize(&out, text, sizeof(text));
    CHECK_INT("serializes", 0, sr.refused_overflow || sr.truncated);
    CHECK_INT("tpm emitted", 1, strstr(text, "tpm = on") != 0);
    CHECK_INT("bad tpm value refused", HYPE_CFG_ERR_BAD_VALUE, parse_copy(bad, &out).status);
    CHECK_INT("duplicate tpm refused", HYPE_CFG_ERR_DUPLICATE_KEY, parse_copy(dup, &out).status);
}

static void test_bus_usb_msc(void) {
    /* #593: bus = usb-msc parses to the new enum and round-trips. */
    const char *cfg =
        "[disk.stick]\ntype = disk\nbacking = file\npath = \\u.img\nbus = usb-msc\n";
    hype_cfg_t out;
    static char text[2048];
    hype_cfg_serialize_result_t sr;
    CHECK_INT("usb-msc parses", HYPE_CFG_OK, parse_copy(cfg, &out).status);
    CHECK_INT("bus usb-msc", (int)HYPE_CFG_BUS_USB_MSC, (int)out.disks[0].bus);
    sr = hype_cfg_serialize(&out, text, sizeof(text));
    CHECK_INT("serializes", 0, sr.refused_overflow || sr.truncated);
    CHECK_INT("usb-msc emitted", 1, strstr(text, "bus = usb-msc") != 0);
}

static void test_attach_detach_disk(void) {
    /* #488: attach a usb-msc image and a sata disk to a VM, then detach one. The VM uses the
     * disks= model (target_disk and disks= are mutually exclusive per §7), which is the model
     * attach/detach manage. */
    const char *base =
        "[vm.k]\nvcpus=1\nmem_mb=512\nboot=disk\ndisks=boot\nfirmware=uefi\nos_hint=linux\n"
        "[disk.boot]\ntype=disk\nbacking=file\npath=\\boot.img\nbus=virtio-blk\n";
    hype_cfg_t out;
    static char text[4096];
    hype_cfg_serialize_result_t sr;
    CHECK_INT("base parses", HYPE_CFG_OK, parse_copy(base, &out).status);
    /* attach an image as removable USB media */
    CHECK_INT("attach usb-msc", 0,
              hype_cfg_attach_disk(&out, "k", "stick", HYPE_CFG_DISK_TYPE_DISK,
                                   HYPE_CFG_BACKING_FILE, "\\u.img", HYPE_CFG_BUS_USB_MSC));
    /* attach a SATA disk */
    CHECK_INT("attach sata", 0,
              hype_cfg_attach_disk(&out, "k", "data", HYPE_CFG_DISK_TYPE_DISK,
                                   HYPE_CFG_BACKING_FILE, "\\d.img", HYPE_CFG_BUS_AHCI_SATA));
    CHECK_INT("three disks now", 3, out.disk_count);
    CHECK_INT("vm has three devices", 3, out.vms[0].disks_count);
    /* it serializes with both sections + the disks= list */
    sr = hype_cfg_serialize(&out, text, sizeof(text));
    CHECK_INT("serializes", 0, sr.refused_overflow || sr.truncated);
    CHECK_INT("usb-msc section emitted", 1, strstr(text, "[disk.stick]") != 0);
    CHECK_INT("usb-msc bus emitted", 1, strstr(text, "bus = usb-msc") != 0);
    CHECK_INT("sata section emitted", 1, strstr(text, "[disk.data]") != 0);
    /* re-parsing the serialized text yields a valid config with both devices on the VM */
    {
        hype_cfg_t rt;
        CHECK_INT("round-trip parses", HYPE_CFG_OK, parse_copy(text, &rt).status);
        CHECK_INT("round-trip three vm devices", 3, rt.vms[0].disks_count);
    }
    /* duplicate id refused */
    CHECK_INT("duplicate id refused", -1,
              hype_cfg_attach_disk(&out, "k", "stick", HYPE_CFG_DISK_TYPE_DISK,
                                   HYPE_CFG_BACKING_FILE, "\\x.img", HYPE_CFG_BUS_USB_MSC));
    /* unknown VM refused */
    CHECK_INT("unknown vm refused", -1,
              hype_cfg_attach_disk(&out, "nope", "z", HYPE_CFG_DISK_TYPE_DISK,
                                   HYPE_CFG_BACKING_FILE, "\\z.img", HYPE_CFG_BUS_USB_MSC));
    /* detach the usb stick */
    CHECK_INT("detach stick", 0, hype_cfg_detach_disk(&out, "k", "stick"));
    CHECK_INT("vm back to two devices", 2, out.vms[0].disks_count);
    sr = hype_cfg_serialize(&out, text, sizeof(text));
    CHECK_INT("detached section gone", 0, strstr(text, "[disk.stick]") != 0);
    CHECK_INT("kept section remains", 1, strstr(text, "[disk.data]") != 0);
    /* detach a device the VM does not have */
    CHECK_INT("detach missing refused", -1, hype_cfg_detach_disk(&out, "k", "stick"));
}

int main(void) {
    test_bus_usb_msc();
    test_attach_detach_disk();
    test_label_from_the_spec_example_is_accepted();
    test_label_absent_leaves_an_empty_string();
    test_label_rejects_empty_and_duplicate();
    test_hash_begins_a_comment();
    test_hash_comment_after_a_value_is_stripped();
    test_whichever_comment_char_comes_first_wins();
    test_display_name_prefers_the_label();
    test_display_name_falls_back_to_the_section_id();
    test_disks_by_reference_is_not_a_target_disk();
    test_a_cdrom_only_vm_has_no_target_disk_but_has_storage();
    test_a_configured_target_disk_is_reported_as_one();
    test_over_long_label_is_refused_not_truncated();
    test_first_unknown_line_is_named_with_its_number();
    test_no_unknown_lines_leaves_the_report_empty();
    test_unknown_line_is_reported_with_its_comment();
    test_serialize_round_trip_full_example();
    test_serialize_reflects_an_in_memory_edit();
    test_serialize_preserves_comments_section_order_and_unknown_content();
    test_serialize_refuses_on_retained_overflow();
    test_serialize_truncation_is_reported_not_silent();
    test_serialize_disk_section_round_trips_optional_fields();
    test_nic_and_switch_sections_parse();
    test_switch_uplink_defaults_to_none();
    test_vm_nics_list();
    test_bad_mac_is_refused();
    test_duplicate_nic_and_switch_ids_refused();
    test_nameless_nic_section_refused();
    test_unknown_nic_key_is_retained();
    test_nic_switch_round_trip();
    test_nic_and_switch_caps();
    test_over_long_nic_id_refused();
    test_nameless_switch_section_refused();
    test_duplicate_nic_keys_refused();
    test_bad_uplink_value();
    test_uplink_none_and_switchless_mac();
    test_serialize_hype_section_lists();
    test_cfg_init_gives_safe_defaults_not_zeroes();
    test_log_level_key();
    test_count_vms_counts_declarations();
    test_parse_into_accepts_more_vms_than_the_default();
    test_parse_into_refuses_past_bound_storage();
    test_size_gb_to_bytes();
    test_resolve_mem_mb();
    test_full_example_from_plan();
    test_seen_fields_records_exactly_what_was_written();
    test_seen_fields_distinguishes_storage_form();
    test_seen_fields_survives_compaction();
    test_cpu_set_comma_list();
    test_boot_disk_no_install_media_required();
    test_display_key();
    test_uplink_static_address();
    test_uplink_address_parsing_is_strict();
    test_boot_kernel();
    test_boot_kernel_with_disk_allowed();
    test_boot_kernel_write_back();
    test_cmdline();
    test_cmdline_absent_vs_empty();
    test_cmdline_too_long();
    test_initrd();
    test_initrd_rules();
    test_initrd_too_long_and_write_back();
    test_tpm_key();
    test_cmdline_write_back();
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
    test_vm_disk_lists();
    test_vm_lists_are_ordered_not_sets();
    test_vm_inline_and_lists_are_mutually_exclusive();
    test_vm_storage_form_is_required();
    test_vm_cdroms_alone_satisfies_the_requirement();
    test_vm_list_errors();
    test_vm_list_id_too_long();
    test_hype_section_all_keys();
    test_hype_section_absent_gives_todays_behaviour();
    test_hype_malformed_falls_back_to_defaults();
    test_hype_unknown_key_still_retained();
    test_hype_section_errors();
    test_hype_cpu_avg_window_default_is_one();
    test_hype_cpu_avg_window_configured_value_is_kept();
    test_hype_cpu_avg_window_zero_is_clamped_not_rejected();
    test_hype_cpu_avg_window_non_numeric_falls_back();
    test_hype_cpu_avg_window_duplicate_is_rejected();
    test_hype_duplicate_after_malformed_is_still_caught();
    test_resolve_bus();
    test_format_assertion();
    test_format_has_flag_is_set_only_when_written();
    test_one_bad_vm_does_not_cost_the_others();
    test_all_vms_bad_still_fails_like_before();
    test_missing_required_field_also_isolates();
    test_section_level_errors_stay_fatal();
    test_skipped_vm_section_entry_does_not_alias();
    test_resolve_vcpus();
    test_retained_overflow_is_not_a_size_problem();          /* #567 */
    test_truncated_is_reported_separately_from_refusal();    /* #567 */
    test_a_documented_config_round_trips();                  /* #567 */
    test_appending_a_vm_to_a_documented_config_writes_back(); /* #567 */
    test_worst_case_config_fits_the_documented_buffer();      /* #567 */

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
