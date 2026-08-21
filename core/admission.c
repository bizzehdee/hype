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

hype_adm_result_t hype_adm_check_pool(const hype_cfg_t *cfg, unsigned int vm_count,
                                      UINT64 default_mem_bytes, UINT64 pool_bytes,
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
    for (i = 0; i < vm_count; i++) {
        UINT64 mem = default_mem_bytes;
        if (i < cfg->vm_count && cfg->vms[i].mem_mb != 0u) {
            mem = (UINT64)cfg->vms[i].mem_mb * 1024ULL * 1024ULL;
        }
        UINT64 want = adm_round_up(mem, granule_bytes) +
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
        /*
         * One cpu_set entry per vCPU, because a vCPU IS a physical core (§10 decision 47) and
         * cpu_set names physical cores -- the operator is naming exactly the cores they asked
         * for. SMT does not enter it: the threads of those cores are what the guest ends up
         * seeing, not what it is charged for.
         */
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
        /*
         * #537: only VMs that actually DECLARE an inline target can collide on one.
         *
         * An absent target_disk leaves the struct zeroed -- kind FILE (enum 0) and an empty path
         * -- so two VMs with no inline target compared equal and the later one was refused. Two
         * empty strings are not the same disk; they are the absence of a disk. This was reachable
         * long before #535's storage-less boot mode: the `disks = <disk-id>` reference form of
         * §5.2 leaves target_disk empty too, so any config with two reference-form VMs already
         * lost the second one silently.
         */
        if (!hype_cfg_vm_has_target_disk(&cfg->vms[i])) {
            continue;
        }
        for (j = i + 1; j < cfg->vm_count; j++) {
            if (!hype_cfg_vm_has_target_disk(&cfg->vms[j])) {
                continue;
            }
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

/*
 * NET-4b (#85): may these two VMs exchange traffic directly?
 *
 * Lives next to hype_adm_check_net_peers() ON PURPOSE. That function decides whether a `net_peers`
 * configuration is ACCEPTABLE; this one decides whether a PACKET is allowed. They have to agree
 * about what "peer" means, and the way to guarantee that is for the rule to exist once.
 *
 * The rule (plan.md 6e): listing a peer on EITHER side establishes it bidirectionally -- "no need
 * to list it on both". So this is symmetric by construction rather than by the caller remembering
 * to ask twice.
 *
 * DEFAULT-DENY, which is #84: an empty or absent `net_peers` means no guest-to-guest connectivity
 * at all, regardless of `net_mode`. A VM is never reachable from another VM by accident -- it takes
 * an operator naming it.
 *
 * A VM is not its own peer. Returning 1 for a == b would make hype forward a guest's packet back
 * into its own receive ring, which is not connectivity, it is a loop.
 */
int hype_adm_vms_are_peers(const hype_cfg_t *cfg, unsigned int a, unsigned int b) {
    unsigned int p;

    if (cfg == 0 || a == b || a >= cfg->vm_count || b >= cfg->vm_count) {
        return 0;
    }
    /* Both ends must actually have networking. A `net_peers` entry naming a VM with
     * `net_mode = none` is refused at startup (hype_adm_check_net_peers), but a forwarding decision
     * must not depend on that check having run -- it is the last line before a packet moves. */
    if (cfg->vms[a].net_mode != HYPE_CFG_NET_NAT || cfg->vms[b].net_mode != HYPE_CFG_NET_NAT) {
        return 0;
    }
    for (p = 0; p < cfg->vms[a].net_peers_count; p++) {
        if (hype_streq(cfg->vms[a].net_peers[p], cfg->vms[b].name)) {
            return 1;
        }
    }
    for (p = 0; p < cfg->vms[b].net_peers_count; p++) {
        if (hype_streq(cfg->vms[b].net_peers[p], cfg->vms[a].name)) {
            return 1;
        }
    }
    return 0;
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

/*
 * ADM-6 (#224): one flat list of every physical claim in the config, from BOTH places a config can
 * make one.
 *
 * The check used to compare `[disk.*]` devices against each other only. A VM's inline
 * `target_disk = physical:<id>` was compared only against other inline targets, and only by string
 * equality (hype_adm_check_target_disk), which knows nothing about partition scope. So a
 * `[disk.*]` scoped to partition 2 of a drive and an inline target scoped to the WHOLE of that same
 * drive were two claims on the same storage that nothing rejected -- and §6d's "exclusively owned"
 * is a promise, not a description, so an unenforced version of it is worse than none.
 *
 * Attribution matters as much as detection here. The old version returned NO_VM for both indices,
 * because it was comparing devices rather than machines -- which left the caller able to report a
 * breach but not to act on it. Every claim now carries the VM that made it, so the boot path can
 * refuse the offending machine the way #531 refuses a cpu_set overlap.
 */
#define ADM_PHYS_CLAIM_MAX (HYPE_CFG_MAX_DISKS + HYPE_CFG_MAX_VMS)

typedef struct {
    const char *id;         /* the drive identity: id_match, or an inline target's path_or_id */
    unsigned int partition; /* 1-based; 0 = the whole drive */
    unsigned int vm_index;  /* who claimed it, or HYPE_ADM_NO_VM for an unattached [disk.*] */
} adm_phys_claim_t;

static unsigned int adm_collect_phys_claims(const hype_cfg_t *cfg, adm_phys_claim_t *out,
                                            unsigned int cap) {
    unsigned int n = 0, i, vi, k;

    /* Every declared physical [disk.*], attributed to the first VM that attaches it. An
     * UNATTACHED one still counts: a config declaring two overlapping physical devices is a config
     * to fix, and waiting until someone attaches it means the breach lands at the worst moment. */
    for (i = 0; i < cfg->disk_count && n < cap; i++) {
        const hype_cfg_disk_t *d = &cfg->disks[i];
        unsigned int owner = HYPE_ADM_NO_VM;
        if (d->backing != HYPE_CFG_BACKING_PHYSICAL || !d->has_id_match) {
            continue;
        }
        for (vi = 0; vi < cfg->vm_count && owner == HYPE_ADM_NO_VM; vi++) {
            for (k = 0; k < cfg->vms[vi].disks_count; k++) {
                if (hype_streq(cfg->vms[vi].disks[k], d->id)) {
                    owner = vi;
                    break;
                }
            }
            for (k = 0; k < cfg->vms[vi].cdroms_count && owner == HYPE_ADM_NO_VM; k++) {
                if (hype_streq(cfg->vms[vi].cdroms[k], d->id)) {
                    owner = vi;
                }
            }
        }
        out[n].id = d->id_match;
        out[n].partition = d->partition;
        out[n].vm_index = owner;
        n++;
    }
    /* And every VM's inline physical target. */
    for (vi = 0; vi < cfg->vm_count && n < cap; vi++) {
        const hype_cfg_vm_t *vm = &cfg->vms[vi];
        if (!hype_cfg_vm_has_target_disk(vm) || vm->target_disk.kind != HYPE_CFG_DISK_PHYSICAL) {
            continue;
        }
        out[n].id = vm->target_disk.path_or_id;
        out[n].partition = vm->target_disk.partition;
        out[n].vm_index = vi;
        n++;
    }
    return n;
}

hype_adm_result_t hype_adm_check_disk_phys_overlap(const hype_cfg_t *cfg) {
    adm_phys_claim_t claims[ADM_PHYS_CLAIM_MAX];
    unsigned int n, i, j;

    n = adm_collect_phys_claims(cfg, claims, ADM_PHYS_CLAIM_MAX);
    for (i = 0; i < n; i++) {
        for (j = i + 1u; j < n; j++) {
            if (!hype_streq(claims[i].id, claims[j].id)) {
                continue; /* different drives cannot overlap */
            }
            /*
             * Two claims by the SAME VM are not a breach: a machine given both the whole drive and
             * a partition of it is redundant, not unsafe, and refusing it would stop a VM booting
             * over a config that harms nobody.
             */
            if (claims[i].vm_index != HYPE_ADM_NO_VM &&
                claims[i].vm_index == claims[j].vm_index) {
                continue;
            }
            if (phys_ranges_overlap(claims[i].partition, claims[j].partition)) {
                return adm_err(HYPE_ADM_ERR_DISK_PHYS_OVERLAP, claims[i].vm_index,
                               claims[j].vm_index);
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
             * The front-ends hype can actually present: virtio-blk, ahci-sata (#333), nvme (#202
             * slice 6) and, since #593, usb-msc (removable USB mass storage over the guest xHCI).
             * Anything else must fail LOUDLY here rather than producing a VM with no disk, which
             * from inside the guest is indistinguishable from a hype bug.
             *
             * This list is the thing to update when a front-end lands -- it was `nvme` that was
             * refused here until the controller existed.
             */
            if (bus != HYPE_CFG_BUS_VIRTIO_BLK && bus != HYPE_CFG_BUS_AHCI_SATA &&
                bus != HYPE_CFG_BUS_NVME && bus != HYPE_CFG_BUS_USB_MSC) {
                return adm_err(HYPE_ADM_ERR_DISK_BUS_UNSUPPORTED, vi, HYPE_ADM_NO_VM);
            }
        }
    }
    return adm_ok();
}

hype_adm_result_t hype_adm_check_disk_count(const hype_cfg_t *cfg, unsigned int max_disks_per_vm) {
    unsigned int vi;

    for (vi = 0; vi < cfg->vm_count; vi++) {
        /*
         * ADM-6 (#224): cdroms count too. The bound is the FW-1 machine model's per-VM STORAGE
         * DEVICE budget -- an interrupt line and a PCI slot each -- and a cdrom spends both exactly
         * as a disk does. Counting only `disks` let a config with 2 disks and 3 cdroms pass a
         * 4-device budget and then have its tail silently never attached, which is the failure this
         * check exists to prevent, one list over.
         */
        if (cfg->vms[vi].disks_count + cfg->vms[vi].cdroms_count > max_disks_per_vm) {
            return adm_err(HYPE_ADM_ERR_DISK_COUNT_EXCEEDED, vi, HYPE_ADM_NO_VM);
        }
    }
    return adm_ok();
}

/* ADM-6 (#224): is `core` one of the cores host_cpu_budget offers? */
static int adm_in_budget(const hype_cfg_t *cfg, unsigned int core) {
    unsigned int i;
    for (i = 0; i < cfg->hype.host_cpu_budget_count; i++) {
        if (cfg->hype.host_cpu_budget[i] == core) {
            return 1;
        }
    }
    return 0;
}

hype_adm_result_t hype_adm_check_cpu_budget(const hype_cfg_t *cfg,
                                            unsigned int physical_core_count) {
    unsigned int i, k, claimed = 0, want_auto = 0, total = 0;

    /* No budget declared means the whole host is the budget, which is what check_vcpus and
     * check_cpu_set already assume. Nothing to add. */
    if (!cfg->hype.has_host_cpu_budget || cfg->hype.host_cpu_budget_count == 0u) {
        return adm_ok();
    }
    for (i = 0; i < cfg->hype.host_cpu_budget_count; i++) {
        if (cfg->hype.host_cpu_budget[i] >= physical_core_count) {
            return adm_err(HYPE_ADM_ERR_CPU_BUDGET_OUT_OF_RANGE, HYPE_ADM_NO_VM, HYPE_ADM_NO_VM);
        }
    }
    for (i = 0; i < cfg->vm_count; i++) {
        const hype_cfg_vm_t *vm = &cfg->vms[i];
        /*
         * The EFFECTIVE cost, not the literal field. vcpus == 0 means the key was absent and §5.2's
         * default of 1 applies (hype_cfg_resolve_vcpus). Summing the raw zeroes would price a
         * three-VM config at nothing and pass it against a one-core budget it cannot fit.
         */
        unsigned int want = (vm->vcpus != 0u) ? vm->vcpus : 1u;
        total += want;
        if (vm->has_cpu_set) {
            for (k = 0; k < vm->cpu_set_count; k++) {
                if (!adm_in_budget(cfg, vm->cpu_set[k])) {
                    return adm_err(HYPE_ADM_ERR_CPU_SET_OUTSIDE_BUDGET, i, HYPE_ADM_NO_VM);
                }
            }
            /* cpu_set_count, not vcpus: check_cpu_set is what insists the two agree, and this must
             * price what was actually named even when they do not. */
            claimed += vm->cpu_set_count;
        } else {
            want_auto += want;
        }
    }
    if (total > cfg->hype.host_cpu_budget_count) {
        return adm_err(HYPE_ADM_ERR_VCPU_EXCEEDS_BUDGET, HYPE_ADM_NO_VM, HYPE_ADM_NO_VM);
    }
    /*
     * The auto-assigned VMs draw from what the cpu_sets have NOT claimed. Checking them against the
     * whole budget instead would accept a config where the pinned VMs have already taken every core
     * -- and the VMs that would then silently have nowhere to go are exactly the ones the operator
     * did not think hard about.
     */
    if (claimed > cfg->hype.host_cpu_budget_count ||
        want_auto > cfg->hype.host_cpu_budget_count - claimed) {
        return adm_err(HYPE_ADM_ERR_VCPU_EXCEEDS_BUDGET, HYPE_ADM_NO_VM, HYPE_ADM_NO_VM);
    }
    return adm_ok();
}

/* #583: index of a [nic.*] by id, or -1. */
static int nic_by_id(const hype_cfg_t *cfg, const char *id) {
    unsigned int i;
    for (i = 0; i < cfg->nic_count; i++) {
        if (hype_streq(cfg->nics[i].id, id)) return (int)i;
    }
    return -1;
}

static int switch_exists(const hype_cfg_t *cfg, const char *id) {
    unsigned int i;
    for (i = 0; i < cfg->switch_count; i++) {
        if (hype_streq(cfg->switches[i].id, id)) return 1;
    }
    return 0;
}

hype_adm_result_t hype_adm_check_nic_refs(const hype_cfg_t *cfg) {
    unsigned int vi, k, i;

    for (vi = 0; vi < cfg->vm_count; vi++) {
        for (k = 0; k < cfg->vms[vi].nics_count; k++) {
            if (nic_by_id(cfg, cfg->vms[vi].nics[k]) < 0) {
                return adm_err(HYPE_ADM_ERR_NIC_REF_UNKNOWN, vi, HYPE_ADM_NO_VM);
            }
        }
    }
    /*
     * Every declared NIC's switch, whether or not a VM attaches it yet. A dangling switch on an
     * unattached NIC is still a config error the operator wants told about now, rather than the
     * first time they add it to a VM.
     */
    for (i = 0; i < cfg->nic_count; i++) {
        if (!cfg->nics[i].has_switch) {
            continue; /* §5.5: no switch IS the default -- its own private isolated segment */
        }
        if (!switch_exists(cfg, cfg->nics[i].switch_id)) {
            return adm_err(HYPE_ADM_ERR_SWITCH_REF_UNKNOWN, HYPE_ADM_NO_VM, HYPE_ADM_NO_VM);
        }
    }
    return adm_ok();
}

hype_adm_result_t hype_adm_check_nic_sharing(const hype_cfg_t *cfg) {
    unsigned int a, b, ka, kb;

    for (a = 0; a < cfg->vm_count; a++) {
        for (ka = 0; ka < cfg->vms[a].nics_count; ka++) {
            for (b = a + 1u; b < cfg->vm_count; b++) {
                for (kb = 0; kb < cfg->vms[b].nics_count; kb++) {
                    if (hype_streq(cfg->vms[a].nics[ka], cfg->vms[b].nics[kb])) {
                        return adm_err(HYPE_ADM_ERR_NIC_SHARED, a, b);
                    }
                }
            }
        }
    }
    /*
     * Two VMs on the same SWITCH is deliberately not checked here and must never be: it is the
     * whole point of §5.5. Unlike cpu_set, where overlap is a breach, switch membership IS the
     * shared-network case (NET-6 #223) -- put three VMs' NICs on one [switch.lan0] and those three
     * share a network. A check here would refuse the feature.
     */
    return adm_ok();
}

hype_adm_result_t hype_adm_check_nic_count(const hype_cfg_t *cfg, unsigned int max_nics_per_vm) {
    unsigned int vi;

    for (vi = 0; vi < cfg->vm_count; vi++) {
        if (cfg->vms[vi].nics_count > max_nics_per_vm) {
            return adm_err(HYPE_ADM_ERR_NIC_COUNT_EXCEEDED, vi, HYPE_ADM_NO_VM);
        }
    }
    return adm_ok();
}

hype_adm_result_t hype_adm_check_vm_ranges(const hype_cfg_t *cfg,
                                           unsigned int physical_core_count) {
    unsigned int i;

    for (i = 0; i < cfg->vm_count; i++) {
        /* vcpus == 0 means the key was ABSENT and §5.2's default of 1 applies (see the header):
         * refusing it would refuse most configs. A core count of 0 means enumeration failed, and
         * comparing against an unknown host is worse than not comparing. */
        if (physical_core_count != 0u && cfg->vms[i].vcpus > physical_core_count) {
            return adm_err(HYPE_ADM_ERR_VCPUS_EXCEED_HOST, i, HYPE_ADM_NO_VM);
        }
    }
    return adm_ok();
}
