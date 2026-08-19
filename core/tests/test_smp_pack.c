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
 * The operator's 4-core / 8-thread laptop, BSP core reserved, so three 2-thread cores are offered
 * to guests. Three 1-vCPU VMs cost exactly three cores, and SMT hands each of them a second
 * logical CPU as a bonus.
 */
static void test_amd_laptop_three_vms_fit(void) {
    unsigned int per_core[3] = {2u, 2u, 2u};
    unsigned int want[3] = {1u, 1u, 1u};
    hype_smp_pack_vm_t out[3];
    unsigned int fit = hype_smp_pack(per_core, 3u, want, 3u, out, 8u);

    CHECK(fit == 3u, "expected all 3 VMs to fit, got %u", fit);
    CHECK(out[0].cores == 1u && out[0].vcpus == 2u, "vm0: %u core -> %u logical", out[0].cores,
          out[0].vcpus);
    CHECK(out[1].cores == 1u && out[1].vcpus == 2u, "vm1: %u core -> %u logical", out[1].cores,
          out[1].vcpus);
    CHECK(out[2].cores == 1u && out[2].vcpus == 2u, "vm2: %u core -> %u logical", out[2].cores,
          out[2].vcpus);
    /* Cores are handed out in order and never shared. */
    CHECK(out[0].first_core == 0u && out[1].first_core == 1u && out[2].first_core == 2u,
          "cores overlap: %u %u %u", out[0].first_core, out[1].first_core, out[2].first_core);
    CHECK(out[0].threads_per_core == 2u, "vm0 should be told 2 threads/core, got %u",
          out[0].threads_per_core);
}

/*
 * THE SAME CONFIG ON A NON-SMT HOST. The cost is identical -- one core each -- and the guests
 * simply get one logical CPU instead of two. That invariance is the point of pricing in cores:
 * a config that fits one host fits every host, and the guest was never promised the sibling.
 */
static void test_same_config_non_smt_host(void) {
    unsigned int per_core[3] = {1u, 1u, 1u};
    unsigned int want[3] = {1u, 1u, 1u};
    hype_smp_pack_vm_t out[3];
    unsigned int fit = hype_smp_pack(per_core, 3u, want, 3u, out, 8u);

    CHECK(fit == 3u, "the same config must still fit without SMT, got %u", fit);
    CHECK(out[0].cores == 1u && out[0].vcpus == 1u, "vm0: %u core -> %u logical", out[0].cores,
          out[0].vcpus);
    CHECK(out[0].threads_per_core == 1u, "vm0 tpc %u", out[0].threads_per_core);
}

/* A multi-core VM gets every thread of every core it was granted. */
static void test_multi_core_vm(void) {
    unsigned int per_core[4] = {2u, 2u, 2u, 2u};
    unsigned int want[2] = {2u, 1u};
    hype_smp_pack_vm_t out[2];
    unsigned int fit = hype_smp_pack(per_core, 4u, want, 2u, out, 8u);

    CHECK(fit == 2u, "expected 2 VMs to fit, got %u", fit);
    CHECK(out[0].cores == 2u && out[0].vcpus == 4u, "vm0: %u cores -> %u logical", out[0].cores,
          out[0].vcpus);
    CHECK(out[0].threads_per_core == 2u, "vm0 tpc %u", out[0].threads_per_core);
    CHECK(out[1].first_core == 2u, "vm1 must start after vm0's cores, got %u", out[1].first_core);
}

/*
 * A hybrid host: the VM is granted a 2-thread and a 1-thread core. CPUID reports ONE
 * threads-per-core for the whole VM, so the narrower core decides -- 1 -- and the wider core's
 * sibling stays idle rather than being described by a number that is wrong for one of them.
 */
static void test_mixed_width_cores_take_the_minimum(void) {
    unsigned int per_core[2] = {2u, 1u};
    unsigned int want[1] = {2u};
    hype_smp_pack_vm_t out[1];
    unsigned int fit = hype_smp_pack(per_core, 2u, want, 1u, out, 8u);

    CHECK(fit == 1u, "fit %u", fit);
    CHECK(out[0].cores == 2u, "vm0 cores %u", out[0].cores);
    CHECK(out[0].threads_per_core == 1u, "must take the NARROWEST core, got %u",
          out[0].threads_per_core);
    CHECK(out[0].vcpus == 2u, "vm0 logical %u", out[0].vcpus);
}

/*
 * #378's degenerate table: siblings could not be proven, so select_cores reports one thread per
 * core. The guest simply loses the bonus -- wasting a thread is safe, pairing two vCPUs onto an
 * unproven sibling is not.
 */
static void test_siblings_unproven_one_thread_per_core(void) {
    unsigned int per_core[4] = {1u, 1u, 1u, 1u};
    unsigned int want[2] = {2u, 2u};
    hype_smp_pack_vm_t out[2];
    unsigned int fit = hype_smp_pack(per_core, 4u, want, 2u, out, 8u);

    CHECK(fit == 2u, "expected 2 VMs to fit on 4 single-thread cores, got %u", fit);
    CHECK(out[0].cores == 2u && out[0].vcpus == 2u, "vm0: %u cores -> %u logical", out[0].cores,
          out[0].vcpus);
    CHECK(out[0].threads_per_core == 1u, "vm0 must be told 1 thread/core, got %u",
          out[0].threads_per_core);
    CHECK(out[1].first_core == 2u, "vm1 first core %u", out[1].first_core);
}

/* Over-subscribed: the cap is a prefix, and the VM that did not fit reports what it would get. */
static void test_overcommit_reports_prefix(void) {
    unsigned int per_core[2] = {2u, 2u};
    unsigned int want[3] = {1u, 1u, 1u};
    hype_smp_pack_vm_t out[3];
    unsigned int fit = hype_smp_pack(per_core, 2u, want, 3u, out, 8u);

    CHECK(fit == 2u, "expected only 2 VMs to fit on 2 cores, got %u", fit);
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

/* A VM that fits only partly caps the prefix there but keeps the cores it did get. */
static void test_partial_fit_caps_there(void) {
    unsigned int per_core[3] = {2u, 2u, 2u};
    unsigned int want[2] = {1u, 4u};
    hype_smp_pack_vm_t out[2];
    unsigned int fit = hype_smp_pack(per_core, 3u, want, 2u, out, 8u);

    CHECK(fit == 1u, "expected 1 VM to fit in full, got %u", fit);
    CHECK(out[1].cores == 2u && out[1].vcpus == 4u, "vm1 partial: %u cores -> %u logical",
          out[1].cores, out[1].vcpus);
}

/* vcpus = 0 in the config means one core, not a free VM. */
static void test_zero_want_means_one(void) {
    unsigned int per_core[2] = {2u, 2u};
    unsigned int want[2] = {0u, 0u};
    hype_smp_pack_vm_t out[2];
    unsigned int fit = hype_smp_pack(per_core, 2u, want, 2u, out, 8u);

    CHECK(fit == 2u, "fit %u", fit);
    CHECK(out[0].cores == 1u && out[0].vcpus == 2u, "vm0: %u/%u", out[0].cores, out[0].vcpus);
    CHECK(out[1].cores == 1u && out[1].vcpus == 2u, "vm1: %u/%u", out[1].cores, out[1].vcpus);
}

/*
 * The per-VM LOGICAL-CPU ceiling drops whole cores, and the dropped cores go back to the pool.
 * Half-using a core would take hardware from another VM for nothing.
 */
static void test_max_vcpus_clamp_drops_cores(void) {
    unsigned int per_core[4] = {2u, 2u, 2u, 2u};
    unsigned int want[2] = {4u, 1u};
    hype_smp_pack_vm_t out[2];
    unsigned int fit = hype_smp_pack(per_core, 4u, want, 2u, out, 4u);

    /* 4 cores x 2 threads = 8 logical, over the ceiling of 4, so only 2 cores are granted. */
    CHECK(out[0].cores == 2u && out[0].vcpus == 4u, "clamped to %u cores / %u logical",
          out[0].cores, out[0].vcpus);
    CHECK(out[1].first_core == 2u, "the dropped cores must go back: vm1 first_core %u",
          out[1].first_core);
    CHECK(fit == 2u, "both VMs should fit once the clamp returns the cores, got %u", fit);

    /* 0 means "no ceiling". */
    fit = hype_smp_pack(per_core, 4u, want, 1u, out, 0u);
    CHECK(fit == 1u && out[0].cores == 4u && out[0].vcpus == 8u, "unclamped: fit %u %u/%u", fit,
          out[0].cores, out[0].vcpus);

    /* A ceiling narrower than one core still grants one core -- a VM with no core cannot run. */
    fit = hype_smp_pack(per_core, 4u, want, 1u, out, 1u);
    CHECK(out[0].cores == 1u, "floor of one core, got %u", out[0].cores);
}

/* A malformed table with a zero-thread core must terminate, not spin. */
static void test_zero_thread_core_terminates(void) {
    unsigned int per_core[3] = {2u, 0u, 2u};
    unsigned int want[1] = {3u};
    hype_smp_pack_vm_t out[1];
    unsigned int fit = hype_smp_pack(per_core, 3u, want, 1u, out, 8u);

    CHECK(fit == 0u, "a VM that cannot be filled must not count as fitting, got %u", fit);
    CHECK(out[0].cores == 1u && out[0].vcpus == 2u, "vm0: %u/%u", out[0].cores, out[0].vcpus);
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
    CHECK(out[0].cores == 0u && out[0].vcpus == 0u, "null per_core must still initialise out[]");
    CHECK(hype_smp_pack(per_core, 1u, 0, 1u, out, 8u) == 0u, "null want");
    CHECK(hype_smp_pack(per_core, 0u, want, 1u, out, 8u) == 0u, "zero cores");
}

int main(void) {
    test_amd_laptop_three_vms_fit();
    test_same_config_non_smt_host();
    test_multi_core_vm();
    test_mixed_width_cores_take_the_minimum();
    test_siblings_unproven_one_thread_per_core();
    test_overcommit_reports_prefix();
    test_partial_fit_caps_there();
    test_zero_want_means_one();
    test_max_vcpus_clamp_drops_cores();
    test_zero_thread_core_terminates();
    test_degenerate_inputs();

    if (failures) {
        printf("%d test(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
