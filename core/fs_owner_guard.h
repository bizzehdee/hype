#ifndef HYPE_CORE_FS_OWNER_GUARD_H
#define HYPE_CORE_FS_OWNER_GUARD_H

#include <stdint.h>

/*
 * #416 / plan.md §10 decision 64: a reusable single-owner-core guard,
 * generalizing the #239/decision-57 pattern (usb_log_this_core_owns_usb() in
 * boot/main.c) that #596 found missing at one call site.
 *
 * #596: hype's host filesystem writers hold shared, unlocked state (FAT32's
 * FAT cache, allocation cursor, per-file size) where OWNERSHIP IS THE LOCK --
 * exactly one core may ever touch it. A call site that forgets to check this
 * lets multiple cores run the same writer concurrently; #596 traced a
 * corrupted FAT32 cluster chain to precisely that (up to four cores ran
 * hype_fat32_append() on one file with no guard). Decision 57 rejected adding
 * a real lock (it would convert guest-dispatch latency into I/O waits on
 * every mutation) in favor of keeping single-owner and enforcing it AT THE
 * SHARED-STATE ENTRY POINT, not scattered at call sites.
 *
 * This module is that entry-point check, made reusable instead of re-derived
 * per writer: bind an owner APIC ID once (e.g. at mount time, or at MP-init
 * for a fixed BSP-owns-it design), then have every mutating entry point of
 * the shared state call hype_fs_owner_guard_check() and refuse rather than
 * proceed when it fails. Pure logic -- no locking primitive, no blocking --
 * a bind is a plain field write and a check is a plain comparison, so both
 * are safe to call from any core.
 */

typedef struct {
    int bound;              /* 0 until hype_fs_owner_guard_bind() is called */
    uint32_t owner_apic_id; /* meaningful only when bound != 0 */
} hype_fs_owner_guard_t;

/* Unbound: hype_fs_owner_guard_check() always passes until bound. Matches
 * the pre-MP case in usb_log_this_core_owns_usb() -- before any second core
 * exists, whichever one is running IS the owner by definition. */
void hype_fs_owner_guard_init(hype_fs_owner_guard_t *g);

/* Binds `owner_apic_id` as the sole core allowed past the guard from now on.
 * Call once, from the owning core itself (e.g. at mount time under whatever
 * discipline already made that core the natural owner -- this module does
 * not decide WHO owns it, only enforces the decision once made). */
void hype_fs_owner_guard_bind(hype_fs_owner_guard_t *g, uint32_t owner_apic_id);

/* 1 if `executing_apic_id` may proceed (unbound, or it IS the bound owner);
 * 0 otherwise. A NULL guard always refuses (0) -- unlike "unbound", which is
 * deliberately permissive, a caller passing no guard at all gets no free
 * pass, since that is far more likely a wiring bug than an intentional
 * single-core-only path. */
int hype_fs_owner_guard_check(const hype_fs_owner_guard_t *g, uint32_t executing_apic_id);

#endif /* HYPE_CORE_FS_OWNER_GUARD_H */
