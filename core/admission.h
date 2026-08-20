#ifndef HYPE_ADMISSION_H
#define HYPE_ADMISSION_H

#include "cfg.h"
#include "efi_types.h"

/*
 * Startup admission control (ADM-*, plan.md §6i): validates a parsed
 * hype.cfg against actual host resources and cross-VM invariants
 * before any VM launches. Every check here is pure -- given a
 * hype_cfg_t and whatever host facts it needs (usable RAM, physical
 * core count), decide pass/fail -- so all of it is unit tested with no
 * UEFI dependency, same as the parser it consumes.
 */

#define HYPE_ADM_NO_VM 0xFFFFFFFFu

typedef enum {
    HYPE_ADM_OK = 0,
    HYPE_ADM_ERR_MEMORY_OVERCOMMIT,
    HYPE_ADM_ERR_VCPU_OVERCOMMIT,
    HYPE_ADM_ERR_CPU_SET_CORE_OUT_OF_RANGE,
    HYPE_ADM_ERR_CPU_SET_COUNT_MISMATCH,
    HYPE_ADM_ERR_CPU_SET_OVERLAP,
    HYPE_ADM_ERR_TARGET_DISK_COLLISION,
    HYPE_ADM_ERR_NET_PEER_UNKNOWN_VM,
    HYPE_ADM_ERR_NET_PEER_NOT_NAT,
    HYPE_ADM_ERR_MEDIA_DISK_ABSENT,
    /* #329 */
    HYPE_ADM_ERR_DISK_REF_UNKNOWN,     /* disks=/cdroms= names no [disk.*] that exists */
    HYPE_ADM_ERR_DISK_REF_WRONG_TYPE,  /* a cdrom in disks=, or a disk in cdroms= */
    HYPE_ADM_ERR_DISK_SHARED_WRITABLE, /* two VMs attach the same writable device */
    HYPE_ADM_ERR_DISK_PHYS_OVERLAP,    /* two devices claim overlapping physical storage */
    HYPE_ADM_ERR_DISK_BUS_UNSUPPORTED, /* a bus hype cannot present yet */
    HYPE_ADM_ERR_DISK_COUNT_EXCEEDED,  /* a VM attaches more disks than hype can present */
    /* ADM-6 (#224) */
    HYPE_ADM_ERR_CPU_BUDGET_OUT_OF_RANGE, /* host_cpu_budget names a core this host does not have */
    HYPE_ADM_ERR_CPU_SET_OUTSIDE_BUDGET,  /* a cpu_set core is outside host_cpu_budget */
    HYPE_ADM_ERR_VCPU_EXCEEDS_BUDGET,     /* the VMs want more cores than the budget offers */
    HYPE_ADM_ERR_VCPUS_EXCEED_HOST,       /* ONE VM alone wants more cores than the host has */
    /* #583 (§5.5) */
    HYPE_ADM_ERR_NIC_REF_UNKNOWN,   /* nics= names no [nic.*] that exists */
    HYPE_ADM_ERR_SWITCH_REF_UNKNOWN, /* a [nic.*].switch names no [switch.*] that exists */
    HYPE_ADM_ERR_NIC_SHARED,        /* two VMs attach the same [nic.*] */
    HYPE_ADM_ERR_NIC_COUNT_EXCEEDED /* a VM attaches more NICs than hype can present */
} hype_adm_status_t;

typedef struct {
    hype_adm_status_t status;
    /* Offending VM index/indices, or HYPE_ADM_NO_VM if not applicable
     * to this particular status (e.g. a whole-config overcommit). */
    unsigned int vm_index_a;
    unsigned int vm_index_b;
} hype_adm_result_t;

/*
 * Sums every VM's mem_mb and rejects if it exceeds usable_ram_bytes
 * minus reserved_bytes (the hypervisor's own memory, device buffers,
 * guest firmware/varstore regions -- see HYPE_ADM_RESERVED_MB_DEFAULT
 * for the current placeholder figure; there's no device model yet to
 * measure a real one against).
 */
#define HYPE_ADM_RESERVED_MB_DEFAULT 256u

/*
 * ADM-7 (#453, plan.md section 10 decision 37): does every VM's guest memory fit the POOL hype has
 * already reserved?
 *
 * hype_adm_check_memory() below compares against a memory-map estimate minus a fixed reserve,
 * which was the best answer available while the allocation was still ahead of the check. It is no
 * longer: #449 reserves one pool up front, so the size is known rather than predicted -- and the
 * prediction was wrong in the way that mattered, because AllocatePages fails on CONTIGUITY, not on
 * total free RAM (#290: "Not out of memory: out of contiguity"). One pool removes that variable,
 * so this check is exact.
 *
 * Counts what a VM actually carves: its guest RAM, its firmware image and its vdisk backing, each
 * rounded up to the pool's 2 MB granularity, because a carve consumes whole granules.
 *
 * `vm_count` is how many VMs hype will actually run, which is NOT always cfg->vm_count: with no
 * hype.cfg at all, hype runs its built-in default set and the config describes none of them.
 * `default_mem_bytes` covers those. Checking only the configured ones reported "0 VMs fit" on a
 * default boot and capped the host to a single VM.
 *
 * `fit_out` receives how many VMs fit in config order, so the caller can name the ones that will
 * not run rather than reporting a bare total. `shortfall_bytes_out` receives what was missing.
 */
hype_adm_result_t hype_adm_check_pool(const hype_cfg_t *cfg, unsigned int vm_count,
                                      UINT64 default_mem_bytes, UINT64 pool_bytes,
                                      UINT64 per_vm_firmware_bytes, UINT64 per_vm_vdisk_bytes,
                                      UINT64 granule_bytes, unsigned int *fit_out,
                                      UINT64 *shortfall_bytes_out);

hype_adm_result_t hype_adm_check_memory(const hype_cfg_t *cfg, UINT64 usable_ram_bytes,
                                         UINT64 reserved_bytes);

/* Sums every VM's vcpus and rejects if it exceeds physical_core_count
 * (the 1:1 pinning model, §3, needs one exclusive physical core per
 * vCPU). */
hype_adm_result_t hype_adm_check_vcpus(const hype_cfg_t *cfg, unsigned int physical_core_count);

/*
 * For every VM with an explicit cpu_set: every listed core must exist
 * on this host (< physical_core_count), and the count must match
 * vcpus. Across VMs: no two explicit cpu_set ranges may share a core --
 * checked hard-reject, not warn-only, since exclusive pinning is what
 * the fault-isolation guarantee (§6g) relies on. VMs without an
 * explicit cpu_set aren't checked here (they're auto-assigned later,
 * from whatever no cpu_set entry claimed).
 */
hype_adm_result_t hype_adm_check_cpu_set(const hype_cfg_t *cfg, unsigned int physical_core_count);

/* Rejects if any two VMs' target_disk resolve to the same file: path
 * or the same physical: serial/GUID -- security-critical per §10
 * decision #20, not just hygiene (see plan.md §6i). Varstore
 * uniqueness isn't separately checked: varstore filenames are derived
 * from each VM's name, and the parser (M1-1) already rejects duplicate
 * VM names, so collision is impossible by construction until a config
 * field for varstore paths exists to check independently of that. */
hype_adm_result_t hype_adm_check_target_disk(const hype_cfg_t *cfg);

/* Every net_peers entry must name another VM actually defined in this
 * config, and both VMs in the pairing must have net_mode = nat. */
hype_adm_result_t hype_adm_check_net_peers(const hype_cfg_t *cfg);

/*
 * NET-4a/4b (#84/#85): may VMs `a` and `b` exchange traffic directly? Symmetric -- listing a peer on
 * either side establishes the pair bidirectionally (plan.md 6e). Returns 0 for anything not
 * explicitly configured, which is #84's default-deny, and 0 for a == b.
 *
 * Deliberately alongside hype_adm_check_net_peers(): that decides whether a CONFIG is acceptable,
 * this decides whether a PACKET is allowed, and the two must not be able to disagree about what a
 * peer is.
 */
int hype_adm_vms_are_peers(const hype_cfg_t *cfg, unsigned int a, unsigned int b);

/*
 * #323: resolves a VM's media_disk (which host drive its install_media
 * lives on) against the drives host discovery actually enumerated.
 * dev_serials[i] is drive i's serial/GUID, or 0/"" for a drive that
 * reported none -- an unidentified drive can never match, since matching
 * it would mean guessing.
 *
 * Returns the drive index, or:
 *   HYPE_ADM_MEDIA_AUTO   -- no media_disk set; the caller picks (today's behaviour)
 *   HYPE_ADM_MEDIA_ABSENT -- a drive WAS named and is not present
 *
 * ABSENT is deliberately distinct from AUTO. A config naming a missing
 * drive is an operator error, and falling back to another drive is the
 * worst available response: it would hand the guest media from a drive
 * nobody asked for. Callers refuse the VM.
 */
#define HYPE_ADM_MEDIA_AUTO (-1)
#define HYPE_ADM_MEDIA_ABSENT (-2)

int hype_adm_select_media_dev(const hype_cfg_t *cfg, unsigned int vm_index,
                              const char *const *dev_serials, unsigned int dev_count);

/* Startup form of the above: refuses the whole config when any VM names
 * a media_disk no enumerated drive matches, naming the VM in
 * vm_index_a. Runs after host discovery, so unlike the other checks here
 * it needs the enumerated drive list rather than only host totals. */
hype_adm_result_t hype_adm_check_media_disk(const hype_cfg_t *cfg, const char *const *dev_serials,
                                            unsigned int dev_count);


/*
 * #329: the cross-entity checks for [disk.*] references. All pure, all cheap, and each of them catches
 * a misconfiguration that would otherwise surface as a guest with a missing or silently shared disk.
 *
 * Deliberately NOT in the parser: whether `disks = a, b` names devices that exist is a question about
 * the whole config, which is the same split target_disk already follows (§10).
 */

/* Every id in a VM's disks=/cdroms= must name a defined [disk.*], of the matching `type`. A cdrom in
 * disks= is a configuration error rather than something to coerce: the two lists mean different
 * front-ends and different read-only semantics. */
hype_adm_result_t hype_adm_check_disk_refs(const hype_cfg_t *cfg);

/*
 * No two VMs may attach the same WRITABLE device. Two guests writing one backing store corrupts it,
 * and neither guest can detect the other. Read-only devices (and every cdrom) ARE shareable -- that is
 * the normal case for one installer ISO serving several VMs, so refusing it would be wrong.
 */
hype_adm_result_t hype_adm_check_disk_sharing(const hype_cfg_t *cfg);

/*
 * Two backing=physical devices must not claim overlapping storage on the same drive.
 *
 * The arithmetic worth being careful about: identical partition numbers conflict, and `whole` conflicts
 * with ANY partition on that drive -- but two DIFFERENT partitions do not. Refusing adjacent partitions
 * would make the whole point of #332 (hand hype the spare partition you already have) unusable.
 */
hype_adm_result_t hype_adm_check_disk_phys_overlap(const hype_cfg_t *cfg);

/*
 * Refuse a bus hype cannot actually present, rather than attaching nothing and leaving the guest to
 * wonder. `nvme` parses (spec §5.3) but the guest-facing controller is #202; until it lands, a config
 * asking for it must fail loudly at startup instead of producing a diskless VM.
 */
hype_adm_result_t hype_adm_check_disk_bus(const hype_cfg_t *cfg);

/*
 * #329: refuse a VM whose disks= list is longer than the front-end can present. max_disks_per_vm
 * is a parameter, not a constant read from the FW layer: the bound is owned by the caller (it is
 * an interrupt-budget fact of the FW-1 machine model, HYPE_FW_1_MAX_DISKS), and admission stays a
 * pure library. The parser's own cap (HYPE_CFG_MAX_VM_DISKS) is larger, so without this a 4-disk
 * config would parse cleanly and then have its tail silently never attached.
 */
hype_adm_result_t hype_adm_check_disk_count(const hype_cfg_t *cfg, unsigned int max_disks_per_vm);

/*
 * ADM-6 (#224): `[hype] host_cpu_budget` -- the cores the operator has given hype at all.
 *
 * ADM-2/ADM-3 assumed every core on the host was hype's to spend. host_cpu_budget (spec §5.1) says
 * otherwise: it is an explicit list, and a core outside it belongs to whatever else the machine is
 * doing. Three things then have to hold, and none of them was checked:
 *
 *   - every budget core must EXIST on this host. A budget naming core 12 on an 8-core box is a
 *     typo that would otherwise place a VM on a core that is not there.
 *   - every `cpu_set` core must be INSIDE the budget. A cpu_set is the operator naming exact
 *     cores; naming one they also declared off-limits is a contradiction in their own config, and
 *     honouring either half silently picks a winner.
 *   - the VMs must FIT. Every cpu_set core is claimed exclusively (decision 47, one whole core per
 *     vCPU), so the VMs WITHOUT a cpu_set are drawn from what is left -- and that remainder, not
 *     the whole budget, is what has to cover them.
 *
 * With no budget declared this is a no-op: the whole host is the budget, which is what
 * hype_adm_check_vcpus/check_cpu_set already assume, so there is nothing here to duplicate.
 */
hype_adm_result_t hype_adm_check_cpu_budget(const hype_cfg_t *cfg,
                                            unsigned int physical_core_count);

/*
 * ADM-6 (#224): each VM's OWN vCPU request against the host, not just the sum.
 *
 * hype_adm_check_vcpus() sums every VM and compares once, which cannot distinguish "four VMs each
 * asking for two cores on a four-core host" from "one VM asking for eight". Both overcommit, but
 * only the second is a request no host of this size could ever satisfy, and telling the operator
 * WHICH VM is impossible is more useful than telling them the total is too big.
 *
 * A REPORT, NOT A REFUSAL, and that is the spec's choice rather than a softening: §5.2 gives
 * `vcpus` the range "1 .. host physical cores - 1" with the note that admission CAPS at the
 * available cores, and hype_cfg_resolve_vcpus() implements exactly that clamp. Refusing here would
 * contradict a documented clamp and stop a VM that hype is supposed to run narrower.
 *
 * `vcpus == 0` IS DELIBERATELY NOT AN ERROR, which is the opposite of the obvious reading. Zero in
 * the struct means the key was ABSENT, and §5.2's default is 1 -- hype_cfg_resolve_vcpus() turns it
 * into one vCPU and says DEFAULTED. A check that refused zero would refuse every config that simply
 * does not mention vcpus, which is most of them.
 *
 * Per-VM MEMORY is deliberately not repeated here: hype_adm_check_pool() already walks the VMs one
 * at a time against the pool that has actually been reserved, and it names the first VM that does
 * not fit. A second, less exact per-VM memory check could only disagree with it.
 */
hype_adm_result_t hype_adm_check_vm_ranges(const hype_cfg_t *cfg,
                                           unsigned int physical_core_count);

/*
 * #583 (§5.5): reference integrity for the network model, the last piece of ADM-6.
 *
 * Every id in a VM's `nics` must name a defined [nic.*], and every [nic.*].switch must name a
 * defined [switch.*]. A dangling reference is refused rather than defaulted, for the reason §5.5's
 * own default makes sharp: a NIC with NO switch is a legitimate configuration meaning "its own
 * private isolated segment". So silently treating `switch = lan0` with no [switch.lan0] as "no
 * switch" would move a guest from a shared network to an isolated one -- the operator asked for
 * connectivity and would get isolation, with nothing said. That is the #285/#331/#339 class: a
 * setting that appears accepted and does something else.
 *
 * A NIC referenced by NO VM is fine and is not reported: [nic.*] blocks may be defined ahead of the
 * VMs that will use them, exactly as [disk.*] may.
 */
hype_adm_result_t hype_adm_check_nic_refs(const hype_cfg_t *cfg);

/*
 * #583: no two VMs may attach the same [nic.*].
 *
 * A NIC is one device with one MAC. Two guests behind one MAC is not a shared network -- that is
 * what a [switch.*] is for -- it is two guests the forwarding plane cannot tell apart, because it
 * identifies a guest by its source address (#81). Sharing a SWITCH is the intended case and is
 * deliberately NOT checked; sharing a NIC is not.
 */
hype_adm_result_t hype_adm_check_nic_sharing(const hype_cfg_t *cfg);

/*
 * #583: a VM's NIC count against what hype can actually present.
 *
 * `max_nics_per_vm` is the caller's, for the same reason hype_adm_check_disk_count() takes its
 * bound: it is a fact of the FW-1 machine model (one guest NIC on PCI device 4 today, #81/#82, and
 * the IO-APIC's 24 pins are fully allocated) and admission stays a pure library. The parser's own
 * cap is larger, so without this a config asking for three NICs would parse cleanly and then have
 * two of them silently never attached.
 */
hype_adm_result_t hype_adm_check_nic_count(const hype_cfg_t *cfg, unsigned int max_nics_per_vm);

#endif /* HYPE_ADMISSION_H */
