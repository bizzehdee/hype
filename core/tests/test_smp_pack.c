#include <stdio.h>
#include <string.h>
#include "../smp_pack.h"

static int failures;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/*
 * The case that motivated #560: the operator's 4-core / 8-thread laptop, BSP core reserved, so
 * three 2-thread cores are offered to guests. Two 2-vCPU guests plus a 1-vCPU test VM must all
 * fit -- under the old one-core-per-vCPU pricing this host could run 3 vCPUs in total and could
 * not start #527's pair of 2-vCPU guests at all.
 */
static void test_amd_laptop_three_vms_fit(void) {
    unsigned int per_core[3] = {2u, 2u, 2u};
    unsigned int want[3] = {2u, 2u, 1u};
    hype_smp_pack_vm_t out[3];
    unsigned int fit = hype_smp_pack(per_core, 3u, want, 3u, out, 8u);

    CHECK(fit == 3u, "expected all 3 VMs to fit, got %u", fit);
    CHECK(out[0].vcpus == 2u && out[0].cores == 1u, "vm0: %u vcpu on %u core", out[0].vcpus,
          out[0].cores);
    CHECK(out[1].vcpus == 2u && out[1].cores == 1u, "vm1: %u vcpu on %u core", out[1].vcpus,
          out[1].cores);
    CHECK(out[2].vcpus == 1u && out[2].cores == 1u, "vm2: %u vcpu on %u core", out[2].vcpus,
          out[2].cores);
    /* Cores are handed out in order and never shared. */
    CHECK(out[0].first_core == 0u && out[1].first_core == 1u && out[2].first_core == 2u,
          "cores overlap: %u %u %u", out[0].first_core, out[1].first_core, out[2].first_core);
    CHECK(out[0].threads_per_core == 2u, "vm0 should be told 2 threads/core, got %u",
          out[0].threads_per_core);
    /* The 1-vCPU VM owns a 2-thread core but must NOT advertise a sibling it has no vCPU on. */
    CHECK(out[2].threads_per_core == 1u, "vm2 should be told 1 thread/core, got %u",
          out[2].threads_per_core);
}

/* A VM wider than one core spans cores; threads_per_core still describes the core, not the VM. */
static void test_vm_spans_cores(void) {
    unsigned int per_core[4] = {2u, 2u, 2u, 2u};
    unsigned int want[2] = {4u, 2u};
    hype_smp_pack_vm_t out[2];
    unsigned int fit = hype_smp_pack(per_core, 4u, want, 2u, out, 8u);

    CHECK(fit == 2u, "expected 2 VMs to fit, got %u", fit);
    CHECK(out[0].vcpus == 4u && out[0].cores == 2u, "vm0: %u vcpu on %u core", out[0].vcpus,
          out[0].cores);
    CHECK(out[0].threads_per_core == 2u, "vm0 tpc %u", out[0].threads_per_core);
    CHECK(out[1].first_core == 2u, "vm1 must start after vm0's cores, got %u", out[1].first_core);
}

/*
 * #378's degenerate table: siblings could not be proven, so select_cores reports one thread per
 * core. Pricing must fall back to one vCPU per core -- wasting a thread is safe, pairing two
 * vCPUs onto an unproven sibling is not.
 */
static void test_siblings_unproven_one_thread_per_core(void) {
    unsigned int per_core[4] = {1u, 1u, 1u, 1u};
    unsigned int want[2] = {2u, 2u};
    hype_smp_pack_vm_t out[2];
    unsigned int fit = hype_smp_pack(per_core, 4u, want, 2u, out, 8u);

    CHECK(fit == 2u, "expected 2 VMs to fit on 4 single-thread cores, got %u", fit);
    CHECK(out[0].cores == 2u && out[0].vcpus == 2u, "vm0: %u vcpu on %u core", out[0].vcpus,
          out[0].cores);
    CHECK(out[0].threads_per_core == 1u, "vm0 must be told 1 thread/core, got %u",
          out[0].threads_per_core);
    CHECK(out[1].first_core == 2u, "vm1 first core %u", out[1].first_core);
}

/* Over-subscribed: the cap is a prefix, and the VM that did not fit reports what it would get. */
static void test_overcommit_reports_prefix(void) {
    unsigned int per_core[2] = {2u, 2u};
    unsigned int want[3] = {2u, 2u, 2u};
    hype_smp_pack_vm_t out[3];
    unsigned int fit = hype_smp_pack(per_core, 2u, want, 3u, out, 8u);

    CHECK(fit == 2u, "expected only 2 VMs to fit, got %u", fit);
    CHECK(out[2].vcpus == 0u && out[2].cores == 0u, "vm2 should get nothing, got %u/%u",
          out[2].vcpus, out[2].cores);
    CHECK(out[2].threads_per_core == 1u, "vm2 tpc %u", out[2].threads_per_core);

    /*
     * Two VMs failing must not move the cap past the FIRST one. Cores are spent in order, so the
     * launchable set is a prefix; a later failure overwriting the cap would admit a VM that has
     * no core -- which is the #559 failure mode, one VM started on another's APIC ID.
     */
    {
        unsigned int one_core[1] = {2u};
        fit = hype_smp_pack(one_core, 1u, want, 3u, out, 8u);
        CHECK(fit == 1u, "expected the cap to stay at the first VM that missed, got %u", fit);
        CHECK(out[1].vcpus == 0u && out[2].vcpus == 0u, "vm1/vm2 should get nothing: %u/%u",
              out[1].vcpus, out[2].vcpus);
    }
}

/* A VM that fits only partly caps the prefix there but keeps the vCPUs it did get. */
static void test_partial_fit_caps_there(void) {
    unsigned int per_core[3] = {2u, 2u, 1u};
    unsigned int want[2] = {2u, 4u};
    hype_smp_pack_vm_t out[2];
    unsigned int fit = hype_smp_pack(per_core, 3u, want, 2u, out, 8u);

    CHECK(fit == 1u, "expected 1 VM to fit in full, got %u", fit);
    CHECK(out[1].vcpus == 3u && out[1].cores == 2u, "vm1 partial: %u vcpu on %u core",
          out[1].vcpus, out[1].cores);
    /* Widest core it got was 2 threads, and it got 3 vCPUs, so 2 is right. */
    CHECK(out[1].threads_per_core == 2u, "vm1 tpc %u", out[1].threads_per_core);
}

/* vcpus = 0 in the config means one vCPU, not a free VM. */
static void test_zero_want_means_one(void) {
    unsigned int per_core[2] = {2u, 2u};
    unsigned int want[2] = {0u, 0u};
    hype_smp_pack_vm_t out[2];
    unsigned int fit = hype_smp_pack(per_core, 2u, want, 2u, out, 8u);

    CHECK(fit == 2u, "fit %u", fit);
    CHECK(out[0].vcpus == 1u && out[0].cores == 1u, "vm0: %u/%u", out[0].vcpus, out[0].cores);
    CHECK(out[1].vcpus == 1u && out[1].cores == 1u, "vm1: %u/%u", out[1].vcpus, out[1].cores);
}

/* The per-VM vCPU ceiling clamps a request before it is priced. */
static void test_max_vcpus_clamp(void) {
    unsigned int per_core[4] = {2u, 2u, 2u, 2u};
    unsigned int want[1] = {8u};
    hype_smp_pack_vm_t out[1];
    unsigned int fit = hype_smp_pack(per_core, 4u, want, 1u, out, 4u);

    CHECK(fit == 1u, "fit %u", fit);
    CHECK(out[0].vcpus == 4u && out[0].cores == 2u, "clamped to %u vcpu on %u core", out[0].vcpus,
          out[0].cores);

    /* 0 means "no ceiling". */
    fit = hype_smp_pack(per_core, 4u, want, 1u, out, 0u);
    CHECK(fit == 1u && out[0].vcpus == 8u && out[0].cores == 4u, "unclamped: fit %u %u/%u", fit,
          out[0].vcpus, out[0].cores);
}

/* A malformed table with a zero-thread core must terminate, not spin. */
static void test_zero_thread_core_terminates(void) {
    unsigned int per_core[3] = {2u, 0u, 2u};
    unsigned int want[1] = {4u};
    hype_smp_pack_vm_t out[1];
    unsigned int fit = hype_smp_pack(per_core, 3u, want, 1u, out, 8u);

    CHECK(fit == 0u, "a VM that cannot be filled must not count as fitting, got %u", fit);
    CHECK(out[0].vcpus == 2u && out[0].cores == 1u, "vm0: %u/%u", out[0].vcpus, out[0].cores);
}

/* Degenerate inputs are refused without touching memory the caller did not provide. */
static void test_degenerate_inputs(void) {
    unsigned int per_core[1] = {2u};
    unsigned int want[1] = {1u};
    hype_smp_pack_vm_t out[1];

    CHECK(hype_smp_pack(per_core, 1u, want, 0u, out, 8u) == 0u, "nvms 0");
    CHECK(hype_smp_pack(per_core, 1u, want, 1u, 0, 8u) == 0u, "null out");
    memset(out, 0xAA, sizeof(out));
    CHECK(hype_smp_pack(0, 1u, want, 1u, out, 8u) == 0u, "null per_core");
    CHECK(out[0].vcpus == 0u, "null per_core must still initialise out[]");
    CHECK(hype_smp_pack(per_core, 1u, 0, 1u, out, 8u) == 0u, "null want");
    CHECK(hype_smp_pack(per_core, 0u, want, 1u, out, 8u) == 0u, "zero cores");
}

int main(void) {
    test_amd_laptop_three_vms_fit();
    test_vm_spans_cores();
    test_siblings_unproven_one_thread_per_core();
    test_overcommit_reports_prefix();
    test_partial_fit_caps_there();
    test_zero_want_means_one();
    test_max_vcpus_clamp();
    test_zero_thread_core_terminates();
    test_degenerate_inputs();

    if (failures) {
        printf("%d test(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
