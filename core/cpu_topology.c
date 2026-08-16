#include "cpu_topology.h"

void hype_cpu_topology_reset(hype_cpu_topology_t *t) {
    if (t == 0) return;
    t->count = 0;
    t->bsp_index = 0;
    t->have_bsp = 0;
    t->dropped = 0;
}

int hype_cpu_topology_add(hype_cpu_topology_t *t, uint32_t apic_id, int is_bsp, int is_enabled) {
    return hype_cpu_topology_add_at(t, apic_id, is_bsp, is_enabled, 0u, 0u, 0u);
}

int hype_cpu_topology_add_at(hype_cpu_topology_t *t, uint32_t apic_id, int is_bsp, int is_enabled,
                             uint32_t package, uint32_t core, uint32_t thread) {
    if (t == 0) return -1;
    if (!is_enabled) return -1; /* cannot be started; recording it would hand out a dead ID */
    if (t->count >= HYPE_CPU_TOPOLOGY_MAX) {
        t->dropped++;
        return -1;
    }
    t->apic_id[t->count] = apic_id;
    t->loc[t->count].package = package;
    t->loc[t->count].core = core;
    t->loc[t->count].thread = thread;
    if (is_bsp && !t->have_bsp) {
        t->bsp_index = t->count;
        t->have_bsp = 1;
    }
    t->count++;
    return 0;
}

int64_t hype_cpu_topology_ap(const hype_cpu_topology_t *t, unsigned int n) {
    unsigned int i;
    unsigned int seen = 0;
    if (t == 0) return -1;
    for (i = 0; i < t->count; i++) {
        if (t->have_bsp && i == t->bsp_index) continue;
        if (seen == n) return (int64_t)t->apic_id[i];
        seen++;
    }
    return -1;
}

unsigned int hype_cpu_topology_ap_count(const hype_cpu_topology_t *t) {
    if (t == 0 || t->count == 0) return 0;
    return t->have_bsp ? (t->count - 1u) : t->count;
}

int64_t hype_cpu_topology_bsp(const hype_cpu_topology_t *t) {
    if (t == 0 || !t->have_bsp) return -1;
    return (int64_t)t->apic_id[t->bsp_index];
}

int hype_cpu_topology_is_consecutive(const hype_cpu_topology_t *t) {
    unsigned int i;
    if (t == 0 || t->count == 0) return 0;
    for (i = 0; i < t->count; i++) {
        if (t->apic_id[i] != i) return 0;
    }
    return 1;
}

void hype_cpu_topology_core_summary(const hype_cpu_topology_t *t, unsigned int *cores,
                                    unsigned int *smt_cores) {
    unsigned int i, j;
    unsigned int ncores = 0, nsmt = 0;
    if (t == 0) {
        if (cores != 0) *cores = 0;
        if (smt_cores != 0) *smt_cores = 0;
        return;
    }
    for (i = 0; i < t->count; i++) {
        unsigned int threads = 0;
        int first = 1;
        for (j = 0; j < i; j++) {
            if (t->loc[j].package == t->loc[i].package && t->loc[j].core == t->loc[i].core) {
                first = 0;
                break;
            }
        }
        if (!first) continue; /* already counted this physical core */
        ncores++;
        for (j = 0; j < t->count; j++) {
            if (t->loc[j].package == t->loc[i].package && t->loc[j].core == t->loc[i].core) {
                threads++;
            }
        }
        if (threads > 1u) nsmt++;
    }
    if (cores != 0) *cores = ncores;
    if (smt_cores != 0) *smt_cores = nsmt;
}

int hype_cpu_topology_select_isolated(const hype_cpu_topology_t *t, unsigned int want,
                                      uint32_t *out_apic, unsigned int out_max) {
    unsigned int taken_pkg[HYPE_CPU_TOPOLOGY_MAX];
    unsigned int taken_core[HYPE_CPU_TOPOLOGY_MAX];
    unsigned int taken = 0, chosen = 0;
    unsigned int i;

    if (t == 0 || out_apic == 0 || want == 0u || out_max == 0u) return 0;
    if (want > out_max) want = out_max;

    /* The BSP's own physical core is claimed before anything else, so no vCPU can land on its
     * SMT sibling. That is the case this exists to prevent. */
    if (t->have_bsp && t->bsp_index < t->count) {
        taken_pkg[taken] = t->loc[t->bsp_index].package;
        taken_core[taken] = t->loc[t->bsp_index].core;
        taken++;
    }

    for (i = 0; i < t->count && chosen < want; i++) {
        unsigned int j;
        int clash = 0;
        if (t->have_bsp && i == t->bsp_index) continue;
        for (j = 0; j < taken; j++) {
            if (taken_pkg[j] == t->loc[i].package && taken_core[j] == t->loc[i].core) {
                clash = 1;
                break;
            }
        }
        if (clash) continue;
        taken_pkg[taken] = t->loc[i].package;
        taken_core[taken] = t->loc[i].core;
        taken++;
        out_apic[chosen++] = t->apic_id[i];
    }
    return (int)chosen;
}

int hype_cpu_topology_locations_degenerate(const hype_cpu_topology_t *t) {
    unsigned int i;
    if (t == 0 || t->count < 2u) return 0;
    for (i = 1u; i < t->count; i++) {
        if (t->loc[i].package != t->loc[0].package ||
            t->loc[i].core != t->loc[0].core ||
            t->loc[i].thread != t->loc[0].thread) {
            return 0;
        }
    }
    return 1;
}

int hype_cpu_topology_layout_from_shifts(unsigned int thread_shift,
                                         unsigned int package_shift,
                                         unsigned int *thread_bits,
                                         unsigned int *core_bits) {
    if (thread_bits == 0 || core_bits == 0 || thread_shift > package_shift ||
        package_shift > 31u) {
        return -1;
    }
    *thread_bits = thread_shift;
    *core_bits = package_shift - thread_shift;
    return 0;
}

static unsigned int bits_for_count(unsigned int count) {
    unsigned int bits = 0u;
    unsigned int value = count - 1u; /* callers reject zero */
    while (value != 0u) {
        bits++;
        value >>= 1;
    }
    return bits;
}

int hype_cpu_topology_layout_from_amd(unsigned int apic_core_id_bits,
                                      unsigned int threads_per_core,
                                      unsigned int *thread_bits,
                                      unsigned int *core_bits) {
    unsigned int logical_per_package;
    unsigned int cores_per_package;
    unsigned int tbits;
    unsigned int cbits;

    if (thread_bits == 0 || core_bits == 0 || apic_core_id_bits == 0u ||
        apic_core_id_bits > 31u || threads_per_core == 0u) {
        return -1;
    }
    logical_per_package = 1u << apic_core_id_bits;
    if (threads_per_core > logical_per_package ||
        logical_per_package % threads_per_core != 0u) {
        return -1;
    }
    cores_per_package = logical_per_package / threads_per_core;
    tbits = bits_for_count(threads_per_core);
    cbits = bits_for_count(cores_per_package);
    *thread_bits = tbits;
    *core_bits = cbits;
    return 0;
}

int hype_cpu_topology_apply_apic_layout(hype_cpu_topology_t *t,
                                        unsigned int thread_bits,
                                        unsigned int core_bits) {
    uint32_t thread_mask;
    uint32_t core_mask;
    unsigned int total_bits;
    unsigned int i;

    if (t == 0 || thread_bits > 31u || core_bits > 31u) return -1;
    total_bits = thread_bits + core_bits;
    if (total_bits > 31u) return -1;
    thread_mask = thread_bits == 0u ? 0u : ((1u << thread_bits) - 1u);
    core_mask = core_bits == 0u ? 0u : ((1u << core_bits) - 1u);
    for (i = 0u; i < t->count; i++) {
        uint32_t id = t->apic_id[i];
        t->loc[i].thread = id & thread_mask;
        t->loc[i].core = (id >> thread_bits) & core_mask;
        t->loc[i].package = id >> total_bits;
    }
    return 0;
}

/*
 * SMP-23 (#479): whole-core enumeration and selection. See cpu_topology.h and plan.md §10
 * decision 40 -- a thread is the unit of execution, a core the unit of allocation.
 */

/* Index of the first processor belonging to the `core_index`-th distinct core, or -1. */
static int topo_first_of_core(const hype_cpu_topology_t *t, unsigned int core_index) {
    unsigned int i, j, seen = 0;
    for (i = 0; i < t->count; i++) {
        int first = 1;
        for (j = 0; j < i; j++) {
            if (t->loc[j].package == t->loc[i].package && t->loc[j].core == t->loc[i].core) {
                first = 0;
                break;
            }
        }
        if (!first) continue;
        if (seen == core_index) return (int)i;
        seen++;
    }
    return -1;
}

unsigned int hype_cpu_topology_core_count(const hype_cpu_topology_t *t) {
    unsigned int cores = 0;
    if (t == 0) return 0;
    hype_cpu_topology_core_summary(t, &cores, 0);
    return cores;
}

unsigned int hype_cpu_topology_core_threads(const hype_cpu_topology_t *t, unsigned int core_index,
                                            uint32_t *out_apic, unsigned int out_max) {
    int first;
    unsigned int i, n = 0;

    if (t == 0 || out_apic == 0 || out_max == 0u) return 0;
    first = topo_first_of_core(t, core_index);
    if (first < 0) return 0;
    for (i = 0; i < t->count && n < out_max; i++) {
        if (t->loc[i].package == t->loc[(unsigned int)first].package &&
            t->loc[i].core == t->loc[(unsigned int)first].core) {
            out_apic[n++] = t->apic_id[i];
        }
    }
    return n;
}

int hype_cpu_topology_siblings_known(const hype_cpu_topology_t *t) {
    unsigned int i;
    if (t == 0 || t->count == 0u) return 0;
    /*
     * The signature of an untrustworthy map is every processor reporting the SAME location --
     * which is what the all-zero firmware table #378 found looks like, and what a failed CPUID
     * repair leaves behind. One processor is trivially consistent, so it counts as known.
     */
    if (t->count == 1u) return 1;
    for (i = 1; i < t->count; i++) {
        if (t->loc[i].package != t->loc[0].package || t->loc[i].core != t->loc[0].core ||
            t->loc[i].thread != t->loc[0].thread) {
            return 1;
        }
    }
    return 0;
}

unsigned int hype_cpu_topology_select_cores(const hype_cpu_topology_t *t, unsigned int want_cores,
                                            uint32_t *out_apic, unsigned int out_max,
                                            unsigned int *out_cores) {
    unsigned int ncores, ci, written = 0, cores_taken = 0;
    uint32_t bsp_pkg = 0, bsp_core = 0;
    int have_bsp_loc = 0;

    if (out_cores != 0) *out_cores = 0;
    if (t == 0 || out_apic == 0 || want_cores == 0u || out_max == 0u) return 0;

    if (t->have_bsp && t->bsp_index < t->count) {
        bsp_pkg = t->loc[t->bsp_index].package;
        bsp_core = t->loc[t->bsp_index].core;
        have_bsp_loc = 1;
    }

    ncores = hype_cpu_topology_core_count(t);
    for (ci = 0; ci < ncores && cores_taken < want_cores; ci++) {
        uint32_t threads[HYPE_CPU_TOPOLOGY_MAX];
        unsigned int nthreads, k;
        int first = topo_first_of_core(t, ci);

        if (first < 0) continue;
        /* The BSP's whole core is excluded, both threads -- no guest vCPU may land on the
         * sibling of the core running hype's console and log duty. */
        if (have_bsp_loc && t->loc[(unsigned int)first].package == bsp_pkg &&
            t->loc[(unsigned int)first].core == bsp_core) {
            continue;
        }
        nthreads = hype_cpu_topology_core_threads(t, ci, threads, HYPE_CPU_TOPOLOGY_MAX);
        if (nthreads == 0u) continue;
        /* All-or-nothing: never hand back half a core, or the caller believes it owns a core
         * whose other thread is still available to someone else. */
        if (written + nthreads > out_max) continue;
        for (k = 0; k < nthreads; k++) {
            out_apic[written++] = threads[k];
        }
        cores_taken++;
    }
    if (out_cores != 0) *out_cores = cores_taken;
    return written;
}
