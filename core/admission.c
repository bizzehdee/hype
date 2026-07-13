#include "admission.h"
#include "strutil.h"

static hype_adm_result_t adm_ok(void) {
    hype_adm_result_t r;
    r.status = HYPE_ADM_OK;
    r.vm_index_a = HYPE_ADM_NO_VM;
    r.vm_index_b = HYPE_ADM_NO_VM;
    return r;
}

static hype_adm_result_t adm_err(hype_adm_status_t status, unsigned int a, unsigned int b) {
    hype_adm_result_t r;
    r.status = status;
    r.vm_index_a = a;
    r.vm_index_b = b;
    return r;
}

hype_adm_result_t hype_adm_check_memory(const hype_cfg_t *cfg, UINT64 usable_ram_bytes,
                                         UINT64 reserved_bytes) {
    UINT64 total_mb = 0;
    UINT64 total_bytes;
    UINT64 budget;
    unsigned int i;

    for (i = 0; i < cfg->vm_count; i++) {
        total_mb += cfg->vms[i].mem_mb;
    }
    total_bytes = total_mb * 1024ULL * 1024ULL;
    budget = (usable_ram_bytes > reserved_bytes) ? (usable_ram_bytes - reserved_bytes) : 0;

    if (total_bytes > budget) {
        return adm_err(HYPE_ADM_ERR_MEMORY_OVERCOMMIT, HYPE_ADM_NO_VM, HYPE_ADM_NO_VM);
    }
    return adm_ok();
}

hype_adm_result_t hype_adm_check_vcpus(const hype_cfg_t *cfg, unsigned int physical_core_count) {
    unsigned int total = 0;
    unsigned int i;

    for (i = 0; i < cfg->vm_count; i++) {
        total += cfg->vms[i].vcpus;
    }
    if (total > physical_core_count) {
        return adm_err(HYPE_ADM_ERR_VCPU_OVERCOMMIT, HYPE_ADM_NO_VM, HYPE_ADM_NO_VM);
    }
    return adm_ok();
}

static int cpu_sets_overlap(const hype_cfg_vm_t *a, const hype_cfg_vm_t *b) {
    unsigned int i, j;

    for (i = 0; i < a->cpu_set_count; i++) {
        for (j = 0; j < b->cpu_set_count; j++) {
            if (a->cpu_set[i] == b->cpu_set[j]) {
                return 1;
            }
        }
    }
    return 0;
}

hype_adm_result_t hype_adm_check_cpu_set(const hype_cfg_t *cfg, unsigned int physical_core_count) {
    unsigned int i, j, k;

    for (i = 0; i < cfg->vm_count; i++) {
        const hype_cfg_vm_t *vm = &cfg->vms[i];

        if (!vm->has_cpu_set) {
            continue;
        }
        if (vm->cpu_set_count != vm->vcpus) {
            return adm_err(HYPE_ADM_ERR_CPU_SET_COUNT_MISMATCH, i, HYPE_ADM_NO_VM);
        }
        for (k = 0; k < vm->cpu_set_count; k++) {
            if (vm->cpu_set[k] >= physical_core_count) {
                return adm_err(HYPE_ADM_ERR_CPU_SET_CORE_OUT_OF_RANGE, i, HYPE_ADM_NO_VM);
            }
        }
    }

    for (i = 0; i < cfg->vm_count; i++) {
        if (!cfg->vms[i].has_cpu_set) {
            continue;
        }
        for (j = i + 1; j < cfg->vm_count; j++) {
            if (!cfg->vms[j].has_cpu_set) {
                continue;
            }
            if (cpu_sets_overlap(&cfg->vms[i], &cfg->vms[j])) {
                return adm_err(HYPE_ADM_ERR_CPU_SET_OVERLAP, i, j);
            }
        }
    }

    return adm_ok();
}

static int target_disk_equal(const hype_cfg_target_disk_t *a, const hype_cfg_target_disk_t *b) {
    return a->kind == b->kind && hype_streq(a->path_or_id, b->path_or_id);
}

hype_adm_result_t hype_adm_check_target_disk(const hype_cfg_t *cfg) {
    unsigned int i, j;

    for (i = 0; i < cfg->vm_count; i++) {
        for (j = i + 1; j < cfg->vm_count; j++) {
            if (target_disk_equal(&cfg->vms[i].target_disk, &cfg->vms[j].target_disk)) {
                return adm_err(HYPE_ADM_ERR_TARGET_DISK_COLLISION, i, j);
            }
        }
    }
    return adm_ok();
}

hype_adm_result_t hype_adm_check_net_peers(const hype_cfg_t *cfg) {
    unsigned int i, p;

    for (i = 0; i < cfg->vm_count; i++) {
        const hype_cfg_vm_t *vm = &cfg->vms[i];

        for (p = 0; p < vm->net_peers_count; p++) {
            unsigned int k;
            int found = -1;

            for (k = 0; k < cfg->vm_count; k++) {
                if (hype_streq(cfg->vms[k].name, vm->net_peers[p])) {
                    found = (int)k;
                    break;
                }
            }
            if (found < 0) {
                return adm_err(HYPE_ADM_ERR_NET_PEER_UNKNOWN_VM, i, HYPE_ADM_NO_VM);
            }
            if (vm->net_mode != HYPE_CFG_NET_NAT || cfg->vms[(unsigned int)found].net_mode != HYPE_CFG_NET_NAT) {
                return adm_err(HYPE_ADM_ERR_NET_PEER_NOT_NAT, i, (unsigned int)found);
            }
        }
    }
    return adm_ok();
}
