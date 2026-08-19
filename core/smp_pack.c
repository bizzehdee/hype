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
        out[vi].cores = 0u;
        out[vi].vcpus = 0u;
        out[vi].first_core = ci;
        out[vi].threads_per_core = 1u;
    }
    if (per_core == 0 || want == 0 || ncores == 0u) {
        return 0;
    }

    for (vi = 0; vi < nvms; vi++) {
        unsigned int n = want[vi] ? want[vi] : 1u;
        unsigned int cores = 0u, narrowest = 0u;

        out[vi].first_core = ci;
        /*
         * Take exactly the cores asked for. A core with 0 threads ends the run: select_cores
         * cannot emit one, but trusting that silently would turn a malformed table into an
         * infinite loop.
         */
        while (cores < n && ci < ncores && per_core[ci] > 0u) {
            if (narrowest == 0u || per_core[ci] < narrowest) {
                narrowest = per_core[ci];
            }
            cores++;
            ci++; /* spent whole: never shared with the next VM */
        }
        if (narrowest == 0u) {
            narrowest = 1u;
        }
        /*
         * Respect the per-VM logical-CPU ceiling by dropping CORES, not by half-using one. A
         * core whose threads this VM cannot use is hardware taken from another VM for nothing,
         * and the cores dropped here are handed back for the next VM to take.
         */
        if (max_vcpus_per_vm != 0u) {
            unsigned int max_cores = max_vcpus_per_vm / narrowest;
            if (max_cores == 0u) {
                max_cores = 1u; /* one core is the floor: a VM with no core cannot run */
            }
            if (cores > max_cores) {
                ci -= (cores - max_cores);
                cores = max_cores;
            }
            /*
             * A ceiling is not a shortage. The VM was given everything it is ALLOWED, so it fit;
             * counting it as a miss would cap the launchable prefix and stop later VMs starting
             * for no reason. Only running out of cores is a miss.
             */
            if (n > max_cores) {
                n = max_cores;
            }
        }
        out[vi].cores = cores;
        out[vi].threads_per_core = narrowest;
        out[vi].vcpus = cores * narrowest;
        if (cores < n && fit == nvms) {
            fit = vi; /* first VM that did not fit; cores are spent in order, so this is the cap */
        }
    }
    return fit;
}
