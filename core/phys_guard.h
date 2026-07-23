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

#endif /* HYPE_CORE_PHYS_GUARD_H */
