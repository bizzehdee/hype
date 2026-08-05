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
    HYPE_ADM_ERR_MEDIA_DISK_ABSENT
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

#endif /* HYPE_ADMISSION_H */
