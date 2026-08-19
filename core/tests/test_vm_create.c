#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../vm_create.h"
#include "../strutil.h"

static int failures;

#define CHECK(msg, cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

/* Walk the wizard with a list of answers, returning the step it ends on. */
static hype_vmw_step_t drive(hype_vm_wizard_t *w, const hype_cfg_t *cfg, const char **answers,
                            unsigned n) {
    unsigned i;
    hype_vmw_step_t st = w->step;
    for (i = 0; i < n; i++) {
        st = hype_vm_wizard_feed(w, answers[i], cfg);
    }
    return st;
}

static void test_all_defaults_creates_a_linux_installer_vm(void) {
    hype_vm_wizard_t w;
    const char *answers[] = {"web", "", "", "", "", "", "", "", "yes"};
    hype_vm_wizard_begin(&w);
    CHECK("starts at the name", w.step == HYPE_VMW_NAME);
    CHECK("done after every prompt is accepted",
          drive(&w, 0, answers, 9) == HYPE_VMW_DONE);
    /* Accepting every default must produce a VM that actually works, or the defaults are wrong. */
    CHECK("name", strcmp(w.vm.name, "web") == 0);
    CHECK("os_hint defaults to linux", w.vm.os_hint == HYPE_CFG_OS_LINUX);
    CHECK("vcpus defaults to 1", w.vm.vcpus == 1u);
    CHECK("mem_mb defaults to 1024", w.vm.mem_mb == 1024u);
    CHECK("firmware is uefi", w.vm.firmware == HYPE_CFG_FW_UEFI);
    CHECK("boot defaults to installer", w.vm.boot == HYPE_CFG_BOOT_INSTALLER);
    CHECK("the disk path is derived from the name",
          strcmp(w.vm.target_disk.path_or_id, "\\hype\\disks\\web.img") == 0);
    CHECK("so is the media path", strcmp(w.vm.install_media, "\\iso\\web.iso") == 0);
    CHECK("and hype picks the core", w.vm.cpu_set_count == 0u);
}

static void test_a_bad_answer_holds_the_step_and_explains(void) {
    hype_vm_wizard_t w;
    hype_vm_wizard_begin(&w);

    CHECK("an empty name is refused", hype_vm_wizard_feed(&w, "", 0) == HYPE_VMW_NAME);
    CHECK("and says why", w.have_error && w.error[0] != '\0');
    CHECK("a name with a space is refused", hype_vm_wizard_feed(&w, "my vm", 0) == HYPE_VMW_NAME);
    CHECK("a name with a slash is refused", hype_vm_wizard_feed(&w, "a/b", 0) == HYPE_VMW_NAME);
    CHECK("a good name advances", hype_vm_wizard_feed(&w, "ok-name_1", 0) == HYPE_VMW_OS_HINT);
    CHECK("and clears the error", !w.have_error && w.error[0] == '\0');

    CHECK("a bad os_hint is refused", hype_vm_wizard_feed(&w, "plan9", 0) == HYPE_VMW_OS_HINT);
    CHECK("bsd is accepted", hype_vm_wizard_feed(&w, "bsd", 0) == HYPE_VMW_VCPUS);
    CHECK("os_hint took", w.vm.os_hint == HYPE_CFG_OS_BSD);

    CHECK("zero vcpus is refused", hype_vm_wizard_feed(&w, "0", 0) == HYPE_VMW_VCPUS);
    CHECK("non-numeric vcpus is refused", hype_vm_wizard_feed(&w, "two", 0) == HYPE_VMW_VCPUS);
    CHECK("4 vcpus is accepted", hype_vm_wizard_feed(&w, "4", 0) == HYPE_VMW_MEM_MB);
    CHECK("vcpus took", w.vm.vcpus == 4u);

    CHECK("zero mem_mb is refused", hype_vm_wizard_feed(&w, "0", 0) == HYPE_VMW_MEM_MB);
    CHECK("2048 MiB is accepted", hype_vm_wizard_feed(&w, "2048", 0) == HYPE_VMW_DISK);
    CHECK("mem took", w.vm.mem_mb == 2048u);
}

/* Two sections with one name is a config the PARSER refuses outright, so the whole file would stop
 * loading on the next boot. Catching it at the prompt is the difference between a typo and a host
 * that comes back with no VMs. */
static void test_duplicate_name_is_refused_against_the_live_config(void) {
    hype_vm_wizard_t w;
    hype_cfg_t cfg;
    hype_cfg_init(&cfg);
    cfg.vm_count = 1;
    (void)hype_strlcpy(cfg.vms[0].name, "web", HYPE_CFG_NAME_MAX);

    hype_vm_wizard_begin(&w);
    CHECK("an existing name is refused", hype_vm_wizard_feed(&w, "web", &cfg) == HYPE_VMW_NAME);
    CHECK("and says so", w.have_error);
    CHECK("a free name is accepted", hype_vm_wizard_feed(&w, "db", &cfg) == HYPE_VMW_OS_HINT);
}

static void test_cpu_set_must_match_vcpus(void) {
    hype_vm_wizard_t w;
    const char *upto[] = {"a", "linux", "2", "512", "", "", ""};
    hype_vm_wizard_begin(&w);
    CHECK("reaches cpu_set", drive(&w, 0, upto, 7) == HYPE_VMW_CPU_SET);

    /*
     * One cpu_set entry per vCPU -- the same rule admission enforces as CPU_SET_COUNT_MISMATCH.
     * Correct on every host, SMT or not, because a vCPU IS a physical core (§10 decision 47):
     * asking for two cores while naming one is a real mismatch.
     */
    CHECK("one core for two vcpus is refused", hype_vm_wizard_feed(&w, "3", 0) == HYPE_VMW_CPU_SET);
    CHECK("and says why", w.have_error);
    CHECK("a range of two is accepted", hype_vm_wizard_feed(&w, "2-3", 0) == HYPE_VMW_CONFIRM);
    CHECK("two cores recorded", w.vm.cpu_set_count == 2u);
    CHECK("and they are the ones asked for", w.vm.cpu_set[0] == 2u && w.vm.cpu_set[1] == 3u);
}

static void test_cpu_set_shapes(void) {
    hype_vm_wizard_t w;
    const char *upto[] = {"a", "linux", "3", "512", "", "", ""};
    hype_vm_wizard_begin(&w);
    (void)drive(&w, 0, upto, 7);
    CHECK("a comma list plus a range is accepted",
          hype_vm_wizard_feed(&w, "1,4-5", 0) == HYPE_VMW_CONFIRM);
    CHECK("three cores", w.vm.cpu_set_count == 3u);
    CHECK("in order given", w.vm.cpu_set[0] == 1u && w.vm.cpu_set[1] == 4u && w.vm.cpu_set[2] == 5u);

    hype_vm_wizard_begin(&w);
    (void)drive(&w, 0, upto, 7);
    CHECK("a reversed range is refused", hype_vm_wizard_feed(&w, "5-1", 0) == HYPE_VMW_CPU_SET);
    CHECK("a core past the maximum is refused",
          hype_vm_wizard_feed(&w, "999", 0) == HYPE_VMW_CPU_SET);
    CHECK("junk is refused", hype_vm_wizard_feed(&w, "2-", 0) == HYPE_VMW_CPU_SET);
}

/* boot=installer with no media boots to a firmware shell, which from inside the VM is
 * indistinguishable from a hype bug. */
static void test_installer_without_media_is_refused(void) {
    hype_vm_wizard_t w;
    const char *upto[] = {"a", "linux", "1", "512", "", "-"};
    hype_vm_wizard_begin(&w);
    CHECK("reaches boot", drive(&w, 0, upto, 6) == HYPE_VMW_BOOT);
    CHECK("installer with no media is refused",
          hype_vm_wizard_feed(&w, "installer", 0) == HYPE_VMW_BOOT);
    CHECK("and says what to do", w.have_error);
    CHECK("boot=disk is fine without media", hype_vm_wizard_feed(&w, "disk", 0) == HYPE_VMW_CPU_SET);
}

static void test_cancel_works_at_every_step(void) {
    hype_vm_wizard_t w;
    unsigned step;
    const char *walk[] = {"a", "linux", "1", "512", "", "", "installer", "-"};
    for (step = 0; step <= 8u; step++) {
        hype_vm_wizard_begin(&w);
        (void)drive(&w, 0, walk, step);
        CHECK("cancel abandons the wizard", hype_vm_wizard_feed(&w, "cancel", 0) == HYPE_VMW_CANCELLED);
        CHECK("and feeding a cancelled wizard changes nothing",
              hype_vm_wizard_feed(&w, "yes", 0) == HYPE_VMW_CANCELLED);
    }
}

static void test_confirm_no_creates_nothing(void) {
    hype_vm_wizard_t w;
    const char *answers[] = {"a", "linux", "1", "512", "", "", "", ""};
    hype_vm_wizard_begin(&w);
    CHECK("reaches confirm", drive(&w, 0, answers, 8) == HYPE_VMW_CONFIRM);
    CHECK("a junk answer holds the step", hype_vm_wizard_feed(&w, "maybe", 0) == HYPE_VMW_CONFIRM);
    CHECK("no cancels", hype_vm_wizard_feed(&w, "no", 0) == HYPE_VMW_CANCELLED);
}

static void test_prompts_and_summary_are_always_available(void) {
    hype_vm_wizard_t w;
    const char *answers[] = {"web", "windows", "2", "4096", "", "", "", "2-3"};
    char sum[192];
    hype_vm_wizard_begin(&w);
    CHECK("a prompt exists at the start", hype_vm_wizard_prompt(&w)[0] != '\0');
    (void)drive(&w, 0, answers, 8);
    hype_vm_wizard_summary(&w, sum, sizeof(sum));
    CHECK("the summary names the VM", strstr(sum, "web") != 0);
    CHECK("and its os", strstr(sum, "windows") != 0);
    CHECK("and its memory", strstr(sum, "4096") != 0);
    CHECK("a null wizard yields an empty prompt", hype_vm_wizard_prompt(0)[0] == '\0');
    hype_vm_wizard_summary(0, sum, sizeof(sum)); /* must not fault */
    hype_vm_wizard_begin(0);                     /* must not fault */
    CHECK("feeding a null wizard is cancelled", hype_vm_wizard_feed(0, "x", 0) == HYPE_VMW_CANCELLED);
}

static void test_explicit_paths_and_limits(void) {
    hype_vm_wizard_t w;
    const char *upto[] = {"a", "none", "1", "1"};
    char longname[HYPE_CFG_NAME_MAX + 8];
    unsigned i;

    hype_vm_wizard_begin(&w);
    for (i = 0; i < sizeof(longname) - 1u; i++) longname[i] = 'x';
    longname[sizeof(longname) - 1u] = '\0';
    CHECK("a name longer than the field is refused",
          hype_vm_wizard_feed(&w, longname, 0) == HYPE_VMW_NAME);

    hype_vm_wizard_begin(&w);
    (void)drive(&w, 0, upto, 4);
    CHECK("an explicit disk path is taken verbatim",
          hype_vm_wizard_feed(&w, "\\hype\\disks\\custom.img", 0) == HYPE_VMW_MEDIA);
    CHECK("disk path", strcmp(w.vm.target_disk.path_or_id, "\\hype\\disks\\custom.img") == 0);
    CHECK("an explicit media path is taken verbatim",
          hype_vm_wizard_feed(&w, "\\iso\\other.iso", 0) == HYPE_VMW_BOOT);
    CHECK("media path", strcmp(w.vm.install_media, "\\iso\\other.iso") == 0);
    CHECK("whitespace-only counts as empty", w.vm.install_media[0] != '\0');

    hype_vm_wizard_begin(&w);
    (void)hype_vm_wizard_feed(&w, "b", 0);
    (void)hype_vm_wizard_feed(&w, "windows", 0);
    CHECK("vcpus above 255 is refused", hype_vm_wizard_feed(&w, "256", 0) == HYPE_VMW_VCPUS);
    CHECK("a tab-only line takes the default", hype_vm_wizard_feed(&w, "\t", 0) == HYPE_VMW_MEM_MB);
}

static void test_cpu_set_pathological_input(void) {
    hype_vm_wizard_t w;
    const char *upto[] = {"a", "none", "1", "8", "", "-", "disk"};
    char huge[64];
    unsigned i;

    hype_vm_wizard_begin(&w);
    CHECK("reaches cpu_set", drive(&w, 0, upto, 7) == HYPE_VMW_CPU_SET);
    for (i = 0; i < sizeof(huge) - 1u; i++) huge[i] = '7';
    huge[sizeof(huge) - 1u] = '\0';
    CHECK("a token longer than the parse buffer is refused",
          hype_vm_wizard_feed(&w, huge, 0) == HYPE_VMW_CPU_SET);
    CHECK("an empty element is refused", hype_vm_wizard_feed(&w, "1,,2", 0) == HYPE_VMW_CPU_SET);
    CHECK("a trailing comma is refused", hype_vm_wizard_feed(&w, "1,", 0) == HYPE_VMW_CPU_SET);
    CHECK("a single core for a single vcpu is accepted",
          hype_vm_wizard_feed(&w, "5", 0) == HYPE_VMW_CONFIRM);
}

static void test_terminal_states_are_inert(void) {
    hype_vm_wizard_t w;
    char sum[192];
    const char *answers[] = {"z", "bsd", "1", "64", "", "-", "disk", "-", "y"};

    hype_vm_wizard_begin(&w);
    CHECK("y is accepted as yes", drive(&w, 0, answers, 9) == HYPE_VMW_DONE);
    CHECK("done has a prompt", hype_vm_wizard_prompt(&w)[0] != '\0');
    CHECK("feeding a done wizard changes nothing",
          hype_vm_wizard_feed(&w, "cancel", 0) == HYPE_VMW_DONE);
    hype_vm_wizard_summary(&w, sum, sizeof(sum));
    CHECK("the summary reports boot=disk", strstr(sum, "boot=disk") != 0);
    CHECK("and bsd", strstr(sum, "bsd") != 0);
    CHECK("and names the absent media", strstr(sum, "(none)") != 0);
    hype_vm_wizard_summary(&w, sum, 0u); /* zero-length out must not write */

    hype_vm_wizard_begin(&w);
    (void)hype_vm_wizard_feed(&w, "cancel", 0);
    CHECK("cancelled has a prompt", hype_vm_wizard_prompt(&w)[0] != '\0');
    hype_vm_wizard_summary(&w, sum, sizeof(sum));
    CHECK("an unnamed VM still summarises", strstr(sum, "(unnamed)") != 0);
}


static void test_remaining_edges(void) {
    hype_vm_wizard_t w;
    char sum[192];
    const char *upto[] = {"Win11-VM", "none", "1", "128", "", "-", "disk"};

    hype_vm_wizard_begin(&w);
    /* Uppercase is as valid as lowercase in a section name; the parser accepts it. */
    CHECK("an uppercase name is accepted", drive(&w, 0, upto, 7) == HYPE_VMW_CPU_SET);
    CHECK("a set larger than the CPU maximum is refused",
          hype_vm_wizard_feed(&w, "0-255", 0) == HYPE_VMW_CPU_SET);
    CHECK("hype-chooses is accepted", hype_vm_wizard_feed(&w, "", 0) == HYPE_VMW_CONFIRM);
    hype_vm_wizard_summary(&w, sum, sizeof(sum));
    CHECK("os_hint none summarises as none", strstr(sum, "none") != 0);
    CHECK("and the uppercase name survives", strstr(sum, "Win11-VM") != 0);
}


/* Every step must offer a prompt -- a wizard that goes silent at one of them looks hung, and the
 * operator's only recourse would be to guess. */
static void test_every_step_prompts(void) {
    hype_vm_wizard_t w;
    const char *walk[] = {"a", "linux", "1", "512", "", "", "installer", "-", "yes"};
    unsigned i;
    hype_vm_wizard_begin(&w);
    for (i = 0; i < 9u; i++) {
        CHECK("the current step has a prompt", hype_vm_wizard_prompt(&w)[0] != '\0');
        (void)hype_vm_wizard_feed(&w, walk[i], 0);
    }
    CHECK("and so does the terminal state", hype_vm_wizard_prompt(&w)[0] != '\0');
    CHECK("reached done", w.step == HYPE_VMW_DONE);

    /* A null line is not a crash and not an acceptance: it is an empty answer. */
    hype_vm_wizard_begin(&w);
    CHECK("a null name is refused", hype_vm_wizard_feed(&w, 0, 0) == HYPE_VMW_NAME);
    (void)hype_vm_wizard_feed(&w, "n", 0);
    CHECK("a null answer takes the default where there is one",
          hype_vm_wizard_feed(&w, 0, 0) == HYPE_VMW_VCPUS);
}


int main(void) {
    test_all_defaults_creates_a_linux_installer_vm();
    test_a_bad_answer_holds_the_step_and_explains();
    test_duplicate_name_is_refused_against_the_live_config();
    test_cpu_set_must_match_vcpus();
    test_cpu_set_shapes();
    test_installer_without_media_is_refused();
    test_cancel_works_at_every_step();
    test_confirm_no_creates_nothing();
    test_prompts_and_summary_are_always_available();
    test_explicit_paths_and_limits();
    test_cpu_set_pathological_input();
    test_terminal_states_are_inert();
    test_remaining_edges();
    test_every_step_prompts();
    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
