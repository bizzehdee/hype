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
