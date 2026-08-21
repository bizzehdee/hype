#include <stdio.h>
#include <string.h>
#include "../vm_delete.h"
#include "../cfg.h"

static int failures = 0;

#define CHECK(msg, cond) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; } \
} while (0)

/* --- the two-step confirmation flow --- */

static void test_full_confirmation_in_order(void) {
    hype_vm_delete_t d;
    char buf[256];

    hype_vm_delete_begin(&d, "web", "\\hype\\disks\\web.img");
    CHECK("starts at step 1", d.step == HYPE_VMD_CONFIRM1);
    hype_vm_delete_render(&d, buf, sizeof(buf));
    CHECK("step 1 is a prompt", strncmp(buf, "delete> ", 8) == 0);
    CHECK("step 1 asks are-you-sure", strstr(buf, "are you sure? (yes|no)") != 0);
    CHECK("step 1 names the vm", strstr(buf, "'web'") != 0);

    CHECK("yes advances to step 2", hype_vm_delete_feed(&d, "yes") == HYPE_VMD_CONFIRM2);
    hype_vm_delete_render(&d, buf, sizeof(buf));
    CHECK("step 2 says cannot be undone", strstr(buf, "cannot be undone") != 0);
    CHECK("step 2 lists the disks left in place", strstr(buf, "\\hype\\disks\\web.img") != 0);
    CHECK("step 2 wants the word delete", strstr(buf, "(delete|cancel)") != 0);

    CHECK("delete confirms", hype_vm_delete_feed(&d, "delete") == HYPE_VMD_CONFIRMED);
}

static void test_no_at_step_1_aborts(void) {
    hype_vm_delete_t d;
    hype_vm_delete_begin(&d, "web", "");
    CHECK("no aborts", hype_vm_delete_feed(&d, "no") == HYPE_VMD_ABORTED);
}

static void test_cancel_at_step_2_aborts(void) {
    hype_vm_delete_t d;
    hype_vm_delete_begin(&d, "web", "");
    (void)hype_vm_delete_feed(&d, "yes");
    CHECK("cancel aborts", hype_vm_delete_feed(&d, "cancel") == HYPE_VMD_ABORTED);
}

static void test_yes_at_step_2_does_not_confirm(void) {
    /* The point of the second gate: a habitual double-yes must not delete anything. */
    hype_vm_delete_t d;
    char buf[256];
    hype_vm_delete_begin(&d, "web", "");
    (void)hype_vm_delete_feed(&d, "yes");
    CHECK("a second yes holds the step", hype_vm_delete_feed(&d, "yes") == HYPE_VMD_CONFIRM2);
    hype_vm_delete_render(&d, buf, sizeof(buf));
    CHECK("and explains", strncmp(buf, "INVALID -- ", 11) == 0);
    CHECK("naming the required word", strstr(buf, "type delete") != 0);
}

static void test_garbage_and_empty_hold_the_step(void) {
    hype_vm_delete_t d;
    hype_vm_delete_begin(&d, "web", "");
    CHECK("empty line holds step 1", hype_vm_delete_feed(&d, "") == HYPE_VMD_CONFIRM1);
    CHECK("garbage holds step 1", hype_vm_delete_feed(&d, "y") == HYPE_VMD_CONFIRM1);
    CHECK("null holds step 1", hype_vm_delete_feed(&d, 0) == HYPE_VMD_CONFIRM1);
    (void)hype_vm_delete_feed(&d, "yes");
    CHECK("garbage holds step 2", hype_vm_delete_feed(&d, "DELETE") == HYPE_VMD_CONFIRM2);
}

static void test_terminal_states_are_inert(void) {
    hype_vm_delete_t d;
    char buf[256];
    hype_vm_delete_begin(&d, "web", "");
    (void)hype_vm_delete_feed(&d, "no");
    CHECK("aborted stays aborted", hype_vm_delete_feed(&d, "yes") == HYPE_VMD_ABORTED);
    hype_vm_delete_render(&d, buf, sizeof(buf));
    CHECK("aborted says nothing changed", strstr(buf, "nothing changed") != 0);

    hype_vm_delete_begin(&d, "web", "");
    (void)hype_vm_delete_feed(&d, "yes");
    (void)hype_vm_delete_feed(&d, "delete");
    CHECK("confirmed stays confirmed", hype_vm_delete_feed(&d, "cancel") == HYPE_VMD_CONFIRMED);
}

static void test_render_bounds(void) {
    hype_vm_delete_t d;
    char tiny[8];
    char buf[256];
    hype_vm_delete_begin(&d, "a-vm-with-a-long-name", "many disks");
    CHECK("render truncates without overrun",
          strlen(hype_vm_delete_render(&d, tiny, sizeof(tiny))) < sizeof(tiny));
    CHECK("null delete renders empty", hype_vm_delete_render(0, tiny, sizeof(tiny))[0] == 0);
    CHECK("null out renders empty string", hype_vm_delete_render(&d, 0, 8u)[0] == 0);
    CHECK("zero cap renders empty string", hype_vm_delete_render(&d, tiny, 0u)[0] == 0);
    CHECK("null feed aborts harmlessly", hype_vm_delete_feed(0, "yes") == HYPE_VMD_ABORTED);
    /* CONFIRMED renders its own status with no prompt. */
    hype_vm_delete_begin(&d, "x", "");
    (void)hype_vm_delete_feed(&d, "yes");
    (void)hype_vm_delete_feed(&d, "delete");
    hype_vm_delete_render(&d, buf, sizeof(buf));
    CHECK("confirmed renders its status", strstr(buf, "confirmed") != 0);
    CHECK("confirmed offers no prompt", strstr(buf, "delete> ") == 0);
    /* begin with null name/note: safe empties, not a crash. */
    hype_vm_delete_begin(&d, 0, 0);
    CHECK("null name copies as empty", d.vm_name[0] == 0);
    hype_vm_delete_render(&d, buf, sizeof(buf));
    CHECK("step-2 empty disks note says none", 1); /* covered below */
    (void)hype_vm_delete_feed(&d, "yes");
    hype_vm_delete_render(&d, buf, sizeof(buf));
    CHECK("empty disks note renders as none", strstr(buf, "in place: none") != 0);
}

/* --- hype_cfg_delete_vm: the config half --- */

static const char CFG2[] =
    "[hype]\n"
    "config_version = 1\n"
    "\n"
    "[switch.lan]\n"
    "uplink = none\n"
    "\n"
    "[vm.alpha]\n"
    "; alpha's comment\n"
    "vcpus = 1\n"
    "mem_mb = 512\n"
    "boot = disk\n"
    "firmware = uefi\n"
    "os_hint = linux\n"
    "net_mode = none\n"
    "disks = ad\n"
    "nics = an\n"
    "\n"
    "[disk.ad]\n"
    "type = disk\n"
    "backing = file\n"
    "path = \\hype\\disks\\alpha.img\n"
    "bus = virtio-blk\n"
    "\n"
    "[nic.an]\n"
    "switch = lan\n"
    "\n"
    "[vm.beta]\n"
    "; beta's comment\n"
    "vcpus = 1\n"
    "mem_mb = 512\n"
    "boot = disk\n"
    "firmware = uefi\n"
    "os_hint = linux\n"
    "net_mode = none\n"
    "disks = bd\n"
    "\n"
    "[disk.bd]\n"
    "type = disk\n"
    "backing = file\n"
    "path = \\hype\\disks\\beta.img\n"
    "bus = virtio-blk\n";

static void parse2(hype_cfg_t *cfg, char *buf, unsigned cap) {
    hype_cfg_result_t r;
    snprintf(buf, cap, "%s", CFG2);
    r = hype_cfg_parse(buf, cfg);
    CHECK("fixture parses", r.status == HYPE_CFG_OK);
    CHECK("fixture has 2 VMs", cfg->vm_count == 2u);
}

static void test_delete_removes_vm_and_its_devices_only(void) {
    static hype_cfg_t cfg;
    static char buf[4096], out[8192];
    hype_cfg_serialize_result_t sr;

    parse2(&cfg, buf, sizeof(buf));
    CHECK("delete alpha ok", hype_cfg_delete_vm(&cfg, 0) == 0);
    CHECK("alpha flagged deleted", cfg.vms[0].deleted == 1);
    CHECK("beta NOT flagged", cfg.vms[1].deleted == 0);
    CHECK("beta keeps its index", strcmp(cfg.vms[1].name, "beta") == 0);

    sr = hype_cfg_serialize(&cfg, out, sizeof(out));
    CHECK("serialize ok", !sr.refused_overflow && !sr.truncated);
    CHECK("no [vm.alpha]", strstr(out, "[vm.alpha]") == 0);
    CHECK("no [disk.ad]", strstr(out, "[disk.ad]") == 0);
    CHECK("no [nic.an]", strstr(out, "[nic.an]") == 0);
    /* Retained lines anchor to the section they are INSIDE (a comment above a header belongs to
     * the previous section), so these sit inside [vm.alpha] / [vm.beta] respectively. */
    CHECK("alpha's comment leaves with its section", strstr(out, "alpha's comment") == 0);
    CHECK("[vm.beta] survives", strstr(out, "[vm.beta]") != 0);
    CHECK("[disk.bd] survives", strstr(out, "[disk.bd]") != 0);
    CHECK("[switch.lan] survives (switches are shared)", strstr(out, "[switch.lan]") != 0);
    CHECK("beta's comment keeps its anchor", strstr(out, "beta's comment") != 0);
    CHECK("[hype] survives", strstr(out, "[hype]") != 0);

    /* The written config must reparse to exactly the survivor. */
    {
        static hype_cfg_t re;
        hype_cfg_result_t rr = hype_cfg_parse(out, &re);
        CHECK("output reparses", rr.status == HYPE_CFG_OK);
        CHECK("one VM remains", re.vm_count == 1u);
        CHECK("and it is beta", strcmp(re.vms[0].name, "beta") == 0);
    }
}

static void test_delete_keeps_a_disk_another_vm_references(void) {
    /* Admission forbids sharing, but a config can still SAY it -- deleting a section out from
     * under the other VM would be the worse failure. */
    static hype_cfg_t cfg;
    static char buf[4096], out[8192];

    parse2(&cfg, buf, sizeof(buf));
    /* Point beta at alpha's disk too. */
    snprintf(cfg.vms[1].disks[0], HYPE_CFG_NAME_MAX, "ad");
    CHECK("delete alpha ok", hype_cfg_delete_vm(&cfg, 0) == 0);
    hype_cfg_serialize(&cfg, out, sizeof(out));
    CHECK("[disk.ad] SURVIVES while beta names it", strstr(out, "[disk.ad]") != 0);
}

static void test_delete_refuses_bad_input(void) {
    static hype_cfg_t cfg;
    static char buf[4096];

    parse2(&cfg, buf, sizeof(buf));
    CHECK("null cfg refused", hype_cfg_delete_vm(0, 0) == -1);
    CHECK("out-of-range refused", hype_cfg_delete_vm(&cfg, 7) == -1);
    CHECK("delete ok", hype_cfg_delete_vm(&cfg, 1) == 0);
    CHECK("double delete refused", hype_cfg_delete_vm(&cfg, 1) == -1);
}

static void test_delete_both_then_config_is_devices_free(void) {
    static hype_cfg_t cfg;
    static char buf[4096], out[8192];

    parse2(&cfg, buf, sizeof(buf));
    CHECK("delete beta", hype_cfg_delete_vm(&cfg, 1) == 0);
    CHECK("delete alpha", hype_cfg_delete_vm(&cfg, 0) == 0);
    hype_cfg_serialize(&cfg, out, sizeof(out));
    CHECK("no vm sections", strstr(out, "[vm.") == 0);
    CHECK("no disk sections", strstr(out, "[disk.") == 0);
    CHECK("switch still present", strstr(out, "[switch.lan]") != 0);
}


static void test_delete_shared_cdrom_and_nic_survive(void) {
    static hype_cfg_t cfg;
    static char buf[4096], out[8192];

    parse2(&cfg, buf, sizeof(buf));
    /* Both VMs reference alpha's devices as cdrom/nic too. */
    snprintf(cfg.vms[1].cdroms[0], HYPE_CFG_NAME_MAX, "ad");
    cfg.vms[1].cdroms_count = 1;
    snprintf(cfg.vms[1].nics[0], HYPE_CFG_NAME_MAX, "an");
    cfg.vms[1].nics_count = 1;
    CHECK("delete alpha ok", hype_cfg_delete_vm(&cfg, 0) == 0);
    hype_cfg_serialize(&cfg, out, sizeof(out));
    CHECK("[disk.ad] survives as beta's cdrom", strstr(out, "[disk.ad]") != 0);
    CHECK("[nic.an] survives as beta's nic", strstr(out, "[nic.an]") != 0);
}

static void test_delete_second_vm_releases_shared_device(void) {
    static hype_cfg_t cfg;
    static char buf[4096], out[8192];

    parse2(&cfg, buf, sizeof(buf));
    snprintf(cfg.vms[1].disks[0], HYPE_CFG_NAME_MAX, "ad");
    CHECK("delete alpha ok", hype_cfg_delete_vm(&cfg, 0) == 0);
    hype_cfg_serialize(&cfg, out, sizeof(out));
    CHECK("shared disk held by beta", strstr(out, "[disk.ad]") != 0);
    /* Beta goes too: the deleted-alpha ghost must NOT keep the disk alive. */
    CHECK("delete beta ok", hype_cfg_delete_vm(&cfg, 1) == 0);
    hype_cfg_serialize(&cfg, out, sizeof(out));
    CHECK("shared disk released once both are gone", strstr(out, "[disk.ad]") == 0);
}

static void test_delete_with_no_matching_section_is_refused(void) {
    static hype_cfg_t cfg;
    unsigned char *b = (unsigned char *)&cfg;
    unsigned long long i;
    for (i = 0; i < sizeof(cfg); i++) b[i] = 0;
    cfg.vms = cfg.vms_default;
    cfg.vm_cap = HYPE_CFG_MAX_VMS;
    cfg.vm_count = 1;
    snprintf(cfg.vms[0].name, HYPE_CFG_NAME_MAX, "ghost");
    /* No sections at all: the config and the caller disagree; refuse, change nothing. */
    CHECK("vm with no section refused", hype_cfg_delete_vm(&cfg, 0) == -1);
    CHECK("not flagged deleted on refusal", cfg.vms[0].deleted == 0);
}

static void test_delete_keeps_pre_section_comments(void) {
    static hype_cfg_t cfg;
    static char buf[4096], out[8192];
    hype_cfg_result_t r;

    snprintf(buf, sizeof(buf), "; file header comment\n%s", CFG2);
    r = hype_cfg_parse(buf, &cfg);
    CHECK("fixture with header parses", r.status == HYPE_CFG_OK);
    CHECK("delete alpha ok", hype_cfg_delete_vm(&cfg, 0) == 0);
    hype_cfg_serialize(&cfg, out, sizeof(out));
    CHECK("pre-section comment survives (anchor -1)", strstr(out, "file header comment") != 0);
}

int main(void) {
    test_full_confirmation_in_order();
    test_no_at_step_1_aborts();
    test_cancel_at_step_2_aborts();
    test_yes_at_step_2_does_not_confirm();
    test_garbage_and_empty_hold_the_step();
    test_terminal_states_are_inert();
    test_render_bounds();

    test_delete_removes_vm_and_its_devices_only();
    test_delete_keeps_a_disk_another_vm_references();
    test_delete_refuses_bad_input();
    test_delete_both_then_config_is_devices_free();
    test_delete_shared_cdrom_and_nic_survive();
    test_delete_second_vm_releases_shared_device();
    test_delete_with_no_matching_section_is_refused();
    test_delete_keeps_pre_section_comments();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
