#ifndef HYPE_CORE_NVLOG_H
#define HYPE_CORE_NVLOG_H

#include <stdint.h>
#include "efi_types.h"

/*
 * RT-3: post-ExitBootServices diagnostic capture that survives a COLD power
 * cycle.
 *
 * The RT-1b next-boot RAM scan only recovers a log if the previous run's
 * bytes survived the reboot -- which a warm reset can preserve but a cold
 * power-off cannot. On a power-off-only machine (no serial, no warm reboot)
 * that leaves no way to get a post-EBS log off the box. This module uses the
 * one channel that both works after ExitBootServices AND persists across a
 * cold boot: a NON_VOLATILE EFI variable (RUNTIME_ACCESS), stored in the
 * firmware's SPI flash. SetVariable is a Runtime Service, callable post-EBS
 * under hype's identity map; GetVariable on the next boot reads it back.
 *
 * The variable holds only the TAIL of the console log (HYPE_NVLOG_CAPACITY
 * bytes) -- flash space is small and write endurance finite, so this is not
 * the full log, but the tail always contains the most recent periodic
 * diagnostic block (EXHIST/COSTHIST/PREEMPT/...), which is the RT-2b signal.
 * Writes are throttled by time + content-change (see hype_nvlog_should_write)
 * to protect the flash.
 *
 * The pure helpers (tail offset, checksum, throttle predicate) are unit
 * tested directly; the SetVariable/GetVariable wrappers are tested against a
 * mock EFI_RUNTIME_SERVICES (same approach as core/gop.c's Blt mock).
 */

#define HYPE_NVLOG_CAPACITY 4096u

/* Variable name + vendor GUID under which the tail is stored. The GUID is
 * hype's own, fixed, and distinctive so it never collides with a firmware or
 * OS variable. */
#define HYPE_NVLOG_VAR_NAME L"HypeDiagTail"
#define HYPE_NVLOG_GUID \
    { 0x68797065, 0x4e56, 0x4c47, { 0x52, 0x54, 0x33, 0x64, 0x69, 0x61, 0x67, 0x00 } }

/* Attributes for the tail variable: persist across cold boot, writable at
 * runtime (post-EBS), readable pre-EBS on the next boot. */
#define HYPE_NVLOG_ATTRS \
    (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

/* --- pure helpers (no EFI calls) --- */

/* Byte offset into a `total_len`-byte log at which the last min(total_len,
 * cap) bytes begin -- i.e. max(0, total_len - cap). */
unsigned int hype_nvlog_tail_offset(unsigned int total_len, unsigned int cap);

/* Rolling-sum checksum over data[0..len), for cheap change detection between
 * throttled writes (identical to logbuf's scheme). */
uint32_t hype_nvlog_checksum(const char *data, unsigned int len);

/*
 * Throttle + change-detection predicate for the periodic writer. Returns 1
 * (write now) if nothing has been written yet, OR if at least `interval_tsc`
 * has elapsed since the last write AND the content changed since then.
 * Returns 0 otherwise. Pure -- the caller owns the last_* state.
 */
int hype_nvlog_should_write(uint64_t now_tsc, uint64_t last_write_tsc, uint64_t interval_tsc,
                            int have_written, uint32_t cur_checksum, uint32_t last_checksum);

/* --- EFI glue (mockable via a fake EFI_RUNTIME_SERVICES) --- */

/*
 * Write the last min(len, HYPE_NVLOG_CAPACITY) bytes of `data` to the tail
 * variable via rt->SetVariable. Returns the SetVariable status (EFI_SUCCESS
 * on success). Best-effort: the caller latches failures to stop retrying.
 */
EFI_STATUS hype_nvlog_write(EFI_RUNTIME_SERVICES *rt, const char *data, unsigned int len);

/*
 * Read the tail variable back into `out` (capacity `out_cap`) via
 * rt->GetVariable. On EFI_SUCCESS, *out_len is the byte count read. Returns
 * EFI_NOT_FOUND when no prior variable exists (the common cold-boot-with-no-
 * prior-run case).
 */
EFI_STATUS hype_nvlog_read(EFI_RUNTIME_SERVICES *rt, char *out, unsigned int out_cap,
                           unsigned int *out_len);

/* Delete the tail variable (SetVariable with size 0) so a stale prior-run
 * tail can't be mistaken for the current run's after it's been recovered. */
EFI_STATUS hype_nvlog_clear(EFI_RUNTIME_SERVICES *rt);

#endif /* HYPE_CORE_NVLOG_H */
