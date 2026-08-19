#include "smp_pack.h"

unsigned int hype_smp_pack(const unsigned int *per_core, unsigned int ncores,
                           const unsigned int *want, unsigned int nvms,
                           hype_smp_pack_vm_t *out, unsigned int max_vcpus_per_vm) {
    unsigned int ci = 0, vi;
    unsigned int fit = nvms;

    if (out == 0 || nvms == 0u) {
        return 0;
    }
    for (vi = 0; vi < nvms; vi++) {
        out[vi].vcpus = 0u;
        out[vi].cores = 0u;
        out[vi].first_core = ci;
        out[vi].threads_per_core = 1u;
    }
    if (per_core == 0 || want == 0 || ncores == 0u) {
        return 0;
    }

    for (vi = 0; vi < nvms; vi++) {
        unsigned int n = want[vi] ? want[vi] : 1u;
        unsigned int got = 0u, cores = 0u, widest = 0u;

        if (max_vcpus_per_vm != 0u && n > max_vcpus_per_vm) {
            n = max_vcpus_per_vm;
        }
        out[vi].first_core = ci;
        /*
         * A core with 0 threads ends the run: select_cores cannot emit one, but trusting that
         * silently would turn a malformed table into an infinite loop.
         */
        while (got < n && ci < ncores && per_core[ci] > 0u) {
            if (per_core[ci] > widest) {
                widest = per_core[ci];
            }
            got += per_core[ci];
            cores++;
            ci++; /* spent whole: never shared with the next VM */
        }
        /*
         * A VM never gets MORE vCPUs than it asked for. The last core may have more threads
         * than the VM still needed; those threads stay idle and belong to this VM, which is the
         * point -- they are not offered to anyone else.
         */
        if (got > n) {
            got = n;
        }
        if (widest > got) {
            widest = got;
        }
        out[vi].vcpus = got;
        out[vi].cores = cores;
        out[vi].threads_per_core = widest ? widest : 1u;
        if (got < n && fit == nvms) {
            fit = vi; /* first VM that did not fit; cores are spent in order, so this is the cap */
        }
    }
    return fit;
}
