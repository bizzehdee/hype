#include <stdio.h>
#include <string.h>
#include "../dump_fmt.h"

static int failures = 0;
#define CHECK(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)

static int text_has(const hype_dash_text_t *t, const char *needle) {
    unsigned i;
    for (i = 0; i < t->count; i++) {
        if (strstr(t->line[i], needle) != NULL) return 1;
    }
    return 0;
}

/* #611: a VM whose vCPU was never dispatched (e.g. still OFF) must say so plainly rather than
 * printing a page of zeros that reads as a genuine snapshot of a real, idle guest. */
static void test_undispatched_vcpu_says_so(void) {
    hype_dump_snapshot_t snap;
    hype_dash_text_t out;

    memset(&snap, 0, sizeof(snap));
    snap.vm_name = "vm0";
    snap.lifecycle = "off";
    snap.n_vcpus = 1;
    snap.vcpu[0].present = 0;

    hype_dump_format(&snap, &out);

    CHECK("names the vm", text_has(&out, "vm0"));
    CHECK("names the lifecycle", text_has(&out, "off"));
    CHECK("says not dispatched", text_has(&out, "not dispatched"));
    CHECK("no fabricated rip", !text_has(&out, "rip="));
}

/* A running vCPU's GPRs/RIP/CR3 and pending/injected event state all reach the printed text. */
static void test_running_vcpu_reports_regs_and_event_state(void) {
    hype_dump_snapshot_t snap;
    hype_dash_text_t out;
    unsigned i;

    memset(&snap, 0, sizeof(snap));
    snap.vm_name = "vm1";
    snap.lifecycle = "running";
    snap.n_vcpus = 1;
    snap.vcpu[0].present = 1;
    snap.vcpu[0].rip = 0xffffffff81000000ULL;
    snap.vcpu[0].cr3 = 0x103000ULL;
    for (i = 0; i < 16u; i++) snap.vcpu[0].gprs[i] = 0x10ULL + i;
    snap.vcpu[0].can_accept = 1;
    snap.vcpu[0].eventinj = 0x80000020ULL;
    snap.vcpu[0].vintr_armed = 1;
    snap.vcpu[0].pending_valid = 1;
    snap.vcpu[0].pending_count = 3;
    snap.vcpu[0].pending_vector = 0x21;

    hype_dump_format(&snap, &out);

    CHECK("names the vm", text_has(&out, "vm1"));
    CHECK("names the lifecycle", text_has(&out, "running"));
    CHECK("reports rip", text_has(&out, "rip=0xffffffff81000000"));
    CHECK("reports cr3", text_has(&out, "cr3=0x0000000000103000"));
    CHECK("reports a GPR", text_has(&out, "rax=0x0000000000000010"));
    CHECK("reports another GPR", text_has(&out, "r15=0x000000000000001f"));
    CHECK("reports can_accept", text_has(&out, "can_accept=yes"));
    CHECK("reports eventinj", text_has(&out, "eventinj=0x0000000080000020"));
    CHECK("reports the interrupt window", text_has(&out, "vintr_window=armed"));
    CHECK("reports pending count", text_has(&out, "pending=3"));
    CHECK("reports the highest pending vector", text_has(&out, "highest=0x21"));
}

/* The exit-reason buckets are the same set EXHIST already classifies (boot/main.c's
 * run_fw_1_test loop) -- naming them consistently is the whole point of reusing them. */
static void test_exit_history_buckets_all_present(void) {
    hype_dump_snapshot_t snap;
    hype_dash_text_t out;

    memset(&snap, 0, sizeof(snap));
    snap.vm_name = "vm0";
    snap.lifecycle = "running";
    snap.n_vcpus = 0;
    snap.ex_total = 1000;
    snap.ex_hlt = 100;
    snap.ex_npf = 200;
    snap.ex_ioio = 300;
    snap.ex_msr = 40;
    snap.ex_cpuid = 50;
    snap.ex_vintr = 60;
    snap.ex_pause = 70;
    snap.ex_intr = 80;
    snap.ex_other = 90;

    hype_dump_format(&snap, &out);

    CHECK("total exits", text_has(&out, "total=1000"));
    CHECK("hlt bucket", text_has(&out, "hlt=100"));
    CHECK("npf bucket", text_has(&out, "npf=200"));
    CHECK("ioio bucket", text_has(&out, "ioio=300"));
    CHECK("msr bucket", text_has(&out, "msr=40"));
    CHECK("cpuid bucket", text_has(&out, "cpuid=50"));
    CHECK("vintr bucket", text_has(&out, "vintr=60"));
    CHECK("pause bucket", text_has(&out, "pause=70"));
    CHECK("intr bucket", text_has(&out, "intr=80"));
    CHECK("other bucket", text_has(&out, "other=90"));
}

/* Several vCPUs of the same VM each get their own bounded section. */
static void test_multiple_vcpus_each_get_a_section(void) {
    hype_dump_snapshot_t snap;
    hype_dash_text_t out;

    memset(&snap, 0, sizeof(snap));
    snap.vm_name = "vm2";
    snap.lifecycle = "running";
    snap.n_vcpus = 2;
    snap.vcpu[0].present = 1;
    snap.vcpu[0].rip = 0x1111ULL;
    snap.vcpu[1].present = 0; /* the second core has not been dispatched yet */

    hype_dump_format(&snap, &out);

    CHECK("vcpu0 section present", text_has(&out, "vcpu0:"));
    CHECK("vcpu0 rip shown", text_has(&out, "rip=0x0000000000001111"));
    CHECK("vcpu1 section present", text_has(&out, "vcpu1:"));
    CHECK("vcpu1 marked undispatched", text_has(&out, "not dispatched"));
}

/* A NULL snapshot must not crash the formatter -- callers pass one whenever a vm lookup fails. */
static void test_null_snapshot_is_safe(void) {
    hype_dash_text_t out;
    hype_dump_format(NULL, &out);
    CHECK("empty result on NULL snapshot", out.count == 0);
    hype_dump_format(NULL, NULL); /* must not crash */
}

int main(void) {
    test_undispatched_vcpu_says_so();
    test_running_vcpu_reports_regs_and_event_state();
    test_exit_history_buckets_all_present();
    test_multiple_vcpus_each_get_a_section();
    test_null_snapshot_is_safe();
    if (failures == 0) {
        printf("OK: all dump_fmt tests passed\n");
        return 0;
    }
    printf("FAILED: %d dump_fmt check(s)\n", failures);
    return 1;
}
