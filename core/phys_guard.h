#ifndef HYPE_CORE_PHYS_GUARD_H
#define HYPE_CORE_PHYS_GUARD_H

#include <stdint.h>

/*
 * M10-3b (#195): the destructive-write safety guard for a `physical:` target
 * disk (§6d / §10 decision). A `physical:` entry in hype.cfg is NEVER by itself
 * sufficient to write to a real drive. Before the FIRST write to a physical
 * target, every one of these must hold, checked here as pure policy:
 *
 *   1. Identity: the configured serial-or-GUID matches the drive actually
 *      enumerated (#122: ATA/NVMe serial + GPT disk GUID) -- never a positional
 *      index. A mismatch refuses to arm that VM's writes.
 *   2. Non-empty-disk guard: a drive whose partition table is already non-empty
 *      is refused unless a distinct explicit "allow overwrite" flag is set for
 *      that specific disk.
 *   3. Interactive confirmation: the operator has explicitly accepted the write
 *      on the local dashboard (drive model/serial/size shown).
 *
 * This module is the pure decision; the caller (physical-backend arming) gathers
 * the inputs (config, enumeration, dashboard state) and refuses to create/arm a
 * writable physical backend unless the result is ALLOW.
 *
 * GUID string form: hype matches the GPT disk GUID as the raw 16 bytes in the
 * order hype itself prints them (`host-disk: gpt-guid aabbccdd-eeff-...`), i.e.
 * a plain in-order hex encoding grouped 8-4-4-4-12 -- NOT the mixed-endian
 * "canonical" rendering -- so what the operator reads off the dashboard is
 * exactly what they put in hype.cfg.
 */

typedef enum {
    HYPE_PHYS_GUARD_ALLOW = 0,
    HYPE_PHYS_GUARD_DENY_ID_MISMATCH,   /* configured serial/GUID != enumerated drive */
    HYPE_PHYS_GUARD_DENY_NONEMPTY,      /* non-empty partition table, no allow-overwrite */
    HYPE_PHYS_GUARD_DENY_NEEDS_CONFIRM  /* operator has not confirmed the first write */
} hype_phys_guard_result_t;

typedef struct {
    const char *configured_id;      /* hype.cfg `physical:<serial-or-guid>` value */
    const char *drive_serial;       /* enumerated ATA/NVMe serial (trimmed), or NULL */
    const uint8_t *disk_guid;       /* enumerated 16-byte GPT disk GUID, or NULL */
    int partition_table_nonempty;   /* 1 if the disk already has a non-empty part table */
    int allow_overwrite;            /* explicit per-disk override flag from config */
    int operator_confirmed;         /* dashboard confirmation given for this disk */
} hype_phys_guard_ctx_t;

/*
 * Parses a GUID string (32 hex digits, optionally grouped 8-4-4-4-12 with '-')
 * into `out16` as bytes in string order. Returns 0 on success, -1 on a bad
 * length or a non-hex character. Pure.
 */
int hype_phys_guid_parse(const char *s, uint8_t out16[16]);

/*
 * Applies the guard. Returns ALLOW only when the identity matches AND the disk
 * is safe to write (empty, or overwrite explicitly allowed) AND the operator
 * has confirmed. Otherwise returns the first failing reason in priority order
 * (identity, then non-empty, then confirmation). Pure.
 */
hype_phys_guard_result_t hype_phys_guard_check(const hype_phys_guard_ctx_t *c);

/*
 * M10-4 (#124): detects whether a disk ALREADY carries a partition table, from
 * its first two 512-byte sectors -- the input to the guard's non-empty refusal,
 * computed from the real drive at arm time (not a config flag). Returns 1 if:
 *   - GPT: sector 1 begins with the "EFI PART" signature, OR
 *   - MBR: sector 0 ends with the 0x55AA boot signature AND at least one of the
 *     four partition-table entries has a non-zero type byte.
 * Returns 0 for a blank/zeroed disk. Pure -- caller supplies the sector bytes
 * (read via the host AHCI/NVMe driver). Either pointer may be NULL (treated as
 * absent -> that check contributes nothing).
 */
int hype_phys_part_table_nonempty(const uint8_t *sector0, const uint8_t *sector1);

/*
 * M10-4 (#124): the arm-time "match-before-write" decision, composed from the
 * detection helper + the guard. Given the config target id, the ENUMERATED
 * drive identity (serial + GPT GUID, #122), the disk's first two sectors, and
 * the overwrite/confirm flags, returns the guard result. The caller creates a
 * WRITABLE physical blk_backend ONLY on HYPE_PHYS_GUARD_ALLOW, and must refuse
 * (never arm writes) otherwise -- a `physical:` config entry is never by itself
 * sufficient (§6d/§10). Pure: derives partition_table_nonempty from the sectors,
 * then applies hype_phys_guard_check.
 */
hype_phys_guard_result_t hype_phys_guard_arm(const char *configured_id, const char *drive_serial,
                                             const uint8_t *disk_guid, const uint8_t *sector0,
                                             const uint8_t *sector1, int allow_overwrite,
                                             int operator_confirmed);

#endif /* HYPE_CORE_PHYS_GUARD_H */
