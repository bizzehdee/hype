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
    HYPE_PHYS_GUARD_DENY_NEEDS_CONFIRM, /* operator has not confirmed the first write */
    /*
     * #243: the sector bytes cannot be trusted, so "is this disk empty?" has no
     * answer. Distinct from DENY_NONEMPTY on purpose -- an operator told "non-empty"
     * looks for data they recognise, whereas the truth here is "this drive is lying to
     * us", which is a different action (replace the disk, not pick another target).
     */
    HYPE_PHYS_GUARD_DENY_UNTRUSTWORTHY_SECTORS
} hype_phys_guard_result_t;

typedef struct {
    const char *configured_id;      /* hype.cfg `physical:<serial-or-guid>` value */
    const char *drive_serial;       /* enumerated ATA/NVMe serial (trimmed), or NULL */
    const uint8_t *disk_guid;       /* enumerated 16-byte GPT disk GUID, or NULL */
    int partition_table_nonempty;   /* 1 if the disk already has a non-empty part table */
    int allow_overwrite;            /* explicit per-disk override flag from config */
    int operator_confirmed;         /* dashboard confirmation given for this disk */
    /*
     * #243: 0 when the sector bytes are not believable -- either they were implausible
     * (see hype_phys_sectors_trustworthy) or two reads of the same LBAs disagreed.
     *
     * This exists because `partition_table_nonempty == 0` used to conflate two states
     * that call for opposite actions: a genuinely blank disk (safe) and a disk whose
     * sectors read back as zeros WITHOUT an I/O error (full of data, and the last
     * thing that should be written). A WD Blue SN5000 on the dev box does exactly the
     * latter -- it reads as empty while full, and reformatting produced a
     * plausible-looking table that was not real.
     */
    int sectors_trustworthy;
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
 * #332: the PARTITION-scoped counterpart -- does this partition already hold a filesystem?
 *
 * hype_phys_part_table_nonempty() above is the wrong question for a partition target, and dangerously
 * so in both directions: the disk ALWAYS has a partition table (that is how the partition exists), so
 * using it would refuse every partition target; and were it bypassed it would say nothing whatever
 * about whether the TARGET PARTITION holds data.
 *
 * Takes the partition's own first sector and its THIRD sector (partition-relative LBA 0 and 2).
 * Sector 2 is needed because an ext2/3/4 superblock lives at byte offset 1024 of the volume, i.e.
 * outside the boot sector. Either pointer may be NULL (that family's check contributes nothing).
 *
 * Returns 1 when a filesystem signature is recognised: FAT12/16 ("FAT" at 54), FAT32 ("FAT32" at 82),
 * exFAT / NTFS (OEM name at 3), ext2/3/4 (0xEF53 at superblock offset 56), or ISO9660 ("CD001").
 * Returns 0 for a partition with none of them.
 *
 * DO NOT pair this with hype_phys_sectors_trustworthy(). That helper reads two all-zero sectors as a
 * lying drive, which is right for a whole disk (any mainstream tool leaves at least a protective MBR)
 * and WRONG for a partition, where all-zeros is exactly what a freshly created empty partition looks
 * like -- every blank partition would be refused. Drive health stays a question about the DISK's
 * LBA0/1; this is a question about the partition's contents. Pure.
 */
int hype_phys_partition_nonempty(const uint8_t *sector0, const uint8_t *sector2);

/*
 * #243: are these sector bytes believable at all?
 *
 * Returns 0 (NOT trustworthy) when LBA0 and LBA1 are BOTH entirely zero. A truly blank
 * disk from any mainstream tool still carries something -- a protective MBR, or at
 * least the 0x55AA signature -- so two fully-zero sectors is the signature of a drive
 * that returned nothing useful without reporting an error, which is what a dying disk
 * does. Fails CLOSED: the caller denies rather than treating it as blank.
 *
 * Deliberately narrow. It is not a general health check and cannot detect a disk that
 * returns plausible garbage; hype_phys_sectors_agree() covers the case where the drive
 * is inconsistent instead of empty. Pure.
 */
int hype_phys_sectors_trustworthy(const uint8_t *sector0, const uint8_t *sector1);

/*
 * #243: do two independent reads of the same two sectors agree?
 *
 * A drive whose content changes between back-to-back reads is lying, whatever the
 * bytes say. Returns 1 when both pairs match, 0 otherwise. Cheap, and it catches the
 * exact failure mode observed: a disk presenting a plausible-but-fabricated table.
 * Pure -- the caller performs the two reads.
 */
int hype_phys_sectors_agree(const uint8_t *a0, const uint8_t *a1, const uint8_t *b0,
                            const uint8_t *b1);

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

/*
 * #332: the arm-time decision for a PARTITION-scoped target.
 *
 * Two different sector pairs, because the two questions are about different things and answering
 * either with the wrong bytes is a wipe-the-wrong-thing bug:
 *
 *   - "does the target already hold data?" is about the PARTITION -- part_sector0/part_sector2, via
 *     hype_phys_partition_nonempty(). Asking hype_phys_part_table_nonempty() about the disk would
 *     refuse every partition target, since the disk always has a table.
 *   - "is this drive telling the truth?" is about the DISK -- disk_sector0/disk_sector1, via
 *     hype_phys_sectors_trustworthy(). It must NOT be asked of the partition: two all-zero sectors
 *     mean a lying drive for a whole disk, but for a partition they are exactly what a freshly
 *     created empty one looks like, so every blank partition would be refused (#243's WD Blue case
 *     is about drive health, not partition contents).
 *
 * Pure; the caller performs both reads.
 */
hype_phys_guard_result_t hype_phys_guard_arm_partition(const char *configured_id,
                                                      const char *drive_serial,
                                                      const uint8_t *disk_guid,
                                                      const uint8_t *disk_sector0,
                                                      const uint8_t *disk_sector1,
                                                      const uint8_t *part_sector0,
                                                      const uint8_t *part_sector2,
                                                      int allow_overwrite, int operator_confirmed);

/*
 * #267: how a `physical:` target should be ATTACHED, which is a separate question
 * from whether it may be WRITTEN.
 *
 * The two were conflated: the backend attached only on a full guard pass, so a
 * `boot = disk` run over an already-installed drive -- non-empty, and with no
 * reason to set allow_overwrite -- got no physical disk at all and silently fell
 * back to a blank RAM scratch. The guest then had nothing to boot, which read as
 * a firmware bug rather than a declined attach.
 *
 * Booting a disk is not installing to it. Identity match alone is enough to hand
 * the drive to the guest READ-ONLY; only the full guard (identity + safe disk
 * state + operator confirmation) may install the backend's write path. So the
 * safe configuration can boot its own disk without ever asserting "yes, wipe it".
 */
typedef enum {
    HYPE_PHYS_ATTACH_NONE = 0,  /* not this target -- do not attach at all */
    HYPE_PHYS_ATTACH_READ_ONLY, /* attach so the guest can boot it; writes refused */
    HYPE_PHYS_ATTACH_WRITABLE   /* full guard passed -- guest writes reach the disk */
} hype_phys_attach_mode_t;

/*
 * Maps a guard result to an attach mode. Only an identity mismatch means "not
 * this drive"; every other refusal is a refusal to WRITE, not a refusal to
 * attach. Pure.
 */
hype_phys_attach_mode_t hype_phys_attach_mode(hype_phys_guard_result_t guard);

#endif /* HYPE_CORE_PHYS_GUARD_H */
