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

static UINT64 adm_round_up(UINT64 v, UINT64 granule) {
    if (granule == 0ULL) {
        return v;
    }
    return ((v + granule - 1ULL) / granule) * granule;
}

hype_adm_result_t hype_adm_check_pool(const hype_cfg_t *cfg, UINT64 pool_bytes,
                                      UINT64 per_vm_firmware_bytes, UINT64 per_vm_vdisk_bytes,
                                      UINT64 granule_bytes, unsigned int *fit_out,
                                      UINT64 *shortfall_bytes_out) {
    UINT64 used = 0ULL;
    unsigned int i;
    unsigned int fit = 0u;

    if (fit_out != 0) {
        *fit_out = 0u;
    }
    if (shortfall_bytes_out != 0) {
        *shortfall_bytes_out = 0ULL;
    }
    if (cfg == 0) {
        return adm_err(HYPE_ADM_ERR_MEMORY_OVERCOMMIT, HYPE_ADM_NO_VM, HYPE_ADM_NO_VM);
    }
    for (i = 0; i < cfg->vm_count; i++) {
        UINT64 want = adm_round_up((UINT64)cfg->vms[i].mem_mb * 1024ULL * 1024ULL, granule_bytes) +
                      adm_round_up(per_vm_firmware_bytes, granule_bytes) +
                      adm_round_up(per_vm_vdisk_bytes, granule_bytes);
        if (used + want > pool_bytes) {
            /* Stop at the first VM that does not fit: the caller names this one and every one
             * after it, in config order, so the operator sees WHICH machines will not exist. */
            if (shortfall_bytes_out != 0) {
                *shortfall_bytes_out = (used + want) - pool_bytes;
            }
            if (fit_out != 0) {
                *fit_out = fit;
            }
            return adm_err(HYPE_ADM_ERR_MEMORY_OVERCOMMIT, i, HYPE_ADM_NO_VM);
        }
        used += want;
        fit++;
    }
    if (fit_out != 0) {
        *fit_out = fit;
    }
    return adm_ok();
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

int hype_adm_select_media_dev(const hype_cfg_t *cfg, unsigned int vm_index,
                              const char *const *dev_serials, unsigned int dev_count) {
    const hype_cfg_vm_t *vm;
    unsigned int i;

    if (cfg == 0 || vm_index >= cfg->vm_count) {
        return HYPE_ADM_MEDIA_AUTO;
    }
    vm = &cfg->vms[vm_index];
    if (!vm->has_media_disk || vm->media_disk[0] == '\0') {
        return HYPE_ADM_MEDIA_AUTO;
    }
    for (i = 0; i < dev_count; i++) {
        const char *ser = (dev_serials != 0) ? dev_serials[i] : 0;

        if (ser != 0 && ser[0] != '\0' && hype_streq(ser, vm->media_disk)) {
            return (int)i;
        }
    }
    return HYPE_ADM_MEDIA_ABSENT;
}

hype_adm_result_t hype_adm_check_media_disk(const hype_cfg_t *cfg, const char *const *dev_serials,
                                            unsigned int dev_count) {
    unsigned int i;

    for (i = 0; i < cfg->vm_count; i++) {
        if (hype_adm_select_media_dev(cfg, i, dev_serials, dev_count) == HYPE_ADM_MEDIA_ABSENT) {
            return adm_err(HYPE_ADM_ERR_MEDIA_DISK_ABSENT, i, HYPE_ADM_NO_VM);
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

/* #329: index of the [disk.*] with this id, or -1. */
static int disk_by_id(const hype_cfg_t *cfg, const char *id) {
    unsigned int i;
    for (i = 0; i < cfg->disk_count; i++) {
        if (hype_streq(cfg->disks[i].id, id)) {
            return (int)i;
        }
    }
    return -1;
}

hype_adm_result_t hype_adm_check_disk_refs(const hype_cfg_t *cfg) {
    unsigned int vi, k;

    for (vi = 0; vi < cfg->vm_count; vi++) {
        const hype_cfg_vm_t *vm = &cfg->vms[vi];

        for (k = 0; k < vm->disks_count; k++) {
            int d = disk_by_id(cfg, vm->disks[k]);
            if (d < 0) {
                return adm_err(HYPE_ADM_ERR_DISK_REF_UNKNOWN, vi, HYPE_ADM_NO_VM);
            }
            if (cfg->disks[d].type != HYPE_CFG_DISK_TYPE_DISK) {
                return adm_err(HYPE_ADM_ERR_DISK_REF_WRONG_TYPE, vi, HYPE_ADM_NO_VM);
            }
        }
        for (k = 0; k < vm->cdroms_count; k++) {
            int d = disk_by_id(cfg, vm->cdroms[k]);
            if (d < 0) {
                return adm_err(HYPE_ADM_ERR_DISK_REF_UNKNOWN, vi, HYPE_ADM_NO_VM);
            }
            if (cfg->disks[d].type != HYPE_CFG_DISK_TYPE_CDROM) {
                return adm_err(HYPE_ADM_ERR_DISK_REF_WRONG_TYPE, vi, HYPE_ADM_NO_VM);
            }
        }
    }
    return adm_ok();
}

hype_adm_result_t hype_adm_check_disk_sharing(const hype_cfg_t *cfg) {
    unsigned int a, b, ka, kb;

    for (a = 0; a < cfg->vm_count; a++) {
        for (ka = 0; ka < cfg->vms[a].disks_count; ka++) {
            int d = disk_by_id(cfg, cfg->vms[a].disks[ka]);
            if (d < 0 || cfg->disks[d].read_only) {
                continue; /* unknown ids are check_disk_refs' business; read-only IS shareable */
            }
            for (b = a + 1u; b < cfg->vm_count; b++) {
                for (kb = 0; kb < cfg->vms[b].disks_count; kb++) {
                    if (hype_streq(cfg->vms[a].disks[ka], cfg->vms[b].disks[kb])) {
                        return adm_err(HYPE_ADM_ERR_DISK_SHARED_WRITABLE, a, b);
                    }
                }
            }
        }
    }
    /* cdroms are deliberately not checked: they are read-only by construction (the parser forces it),
     * and one installer ISO serving every VM is the normal case, not a mistake. */
    return adm_ok();
}

/* Do two physical claims on the SAME drive overlap? */
static int phys_ranges_overlap(unsigned int part_a, unsigned int part_b) {
    /* 0 means the whole disk, which contains every partition -- so it conflicts with anything. */
    if (part_a == 0u || part_b == 0u) {
        return 1;
    }
    /* Otherwise only the SAME partition conflicts. Two different partitions are disjoint by
     * definition, and refusing them would defeat #332's whole purpose. */
    return part_a == part_b;
}

hype_adm_result_t hype_adm_check_disk_phys_overlap(const hype_cfg_t *cfg) {
    unsigned int i, j;

    for (i = 0; i < cfg->disk_count; i++) {
        const hype_cfg_disk_t *da = &cfg->disks[i];
        if (da->backing != HYPE_CFG_BACKING_PHYSICAL || !da->has_id_match) {
            continue;
        }
        for (j = i + 1u; j < cfg->disk_count; j++) {
            const hype_cfg_disk_t *db = &cfg->disks[j];
            if (db->backing != HYPE_CFG_BACKING_PHYSICAL || !db->has_id_match) {
                continue;
            }
            if (!hype_streq(da->id_match, db->id_match)) {
                continue; /* different drives cannot overlap */
            }
            if (phys_ranges_overlap(da->partition, db->partition)) {
                return adm_err(HYPE_ADM_ERR_DISK_PHYS_OVERLAP, HYPE_ADM_NO_VM, HYPE_ADM_NO_VM);
            }
        }
    }
    return adm_ok();
}

hype_adm_result_t hype_adm_check_disk_bus(const hype_cfg_t *cfg) {
    unsigned int vi, k;

    for (vi = 0; vi < cfg->vm_count; vi++) {
        for (k = 0; k < cfg->vms[vi].disks_count; k++) {
            int d = disk_by_id(cfg, cfg->vms[vi].disks[k]);
            hype_cfg_bus_t bus;
            if (d < 0) {
                continue;
            }
            bus = hype_cfg_resolve_bus(&cfg->disks[d], cfg->vms[vi].os_hint);
            /*
             * The front-ends hype can actually present: virtio-blk, ahci-sata (#333) and, since #202
             * slice 6, nvme. Anything else must fail LOUDLY here rather than producing a VM with no
             * disk, which from inside the guest is indistinguishable from a hype bug.
             *
             * This list is the thing to update when a front-end lands -- it was `nvme` that was
             * refused here until the controller existed.
             */
            if (bus != HYPE_CFG_BUS_VIRTIO_BLK && bus != HYPE_CFG_BUS_AHCI_SATA &&
                bus != HYPE_CFG_BUS_NVME) {
                return adm_err(HYPE_ADM_ERR_DISK_BUS_UNSUPPORTED, vi, HYPE_ADM_NO_VM);
            }
        }
    }
    return adm_ok();
}

hype_adm_result_t hype_adm_check_disk_count(const hype_cfg_t *cfg, unsigned int max_disks_per_vm) {
    unsigned int vi;

    for (vi = 0; vi < cfg->vm_count; vi++) {
        if (cfg->vms[vi].disks_count > max_disks_per_vm) {
            return adm_err(HYPE_ADM_ERR_DISK_COUNT_EXCEEDED, vi, HYPE_ADM_NO_VM);
        }
    }
    return adm_ok();
}
