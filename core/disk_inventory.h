#ifndef HYPE_CORE_DISK_INVENTORY_H
#define HYPE_CORE_DISK_INVENTORY_H

#include <stdint.h>

/*
 * #258: every host disk hype found, not just the first one on the first
 * controller.
 *
 * The host-disk path used to be built on a single "find the first SATA port"
 * result, so hype could see exactly one disk per controller -- whichever sat on
 * the lowest implemented port. On the ordinary desktop layout (OS disk on port
 * 0, scratch on port 1) a `physical:` target naming the scratch could never be
 * selected. It failed SAFE -- the serial guard refused to write to a disk whose
 * serial did not match -- but the target was simply unreachable.
 *
 * This is the inventory that replaces that single value: enumerate everything,
 * then select by serial. It also fixes a diagnostic gap that mattered as much as
 * the bug -- a serial mismatch could not distinguish "that disk is not in this
 * machine" from "that disk is here, on a port hype never scanned", and those
 * call for completely different operator responses.
 *
 * Pure: no MMIO, no allocation, no globals. The hardware scan fills it; this
 * module only stores and searches. Fully unit tested.
 */

/* Bus/backend a disk was found on -- selection needs to know which driver to
 * drive it with, and the operator needs it to identify the disk physically. */
typedef enum {
    HYPE_DISK_BUS_AHCI = 0,
    HYPE_DISK_BUS_NVME = 1
} hype_disk_bus_t;

typedef struct {
    hype_disk_bus_t bus;
    uint64_t bar_phys;      /* ABAR (AHCI) or the NVMe controller's BAR0 */
    unsigned int port;      /* AHCI port; 0 for NVMe (namespace 1 is implied) */
    char serial[21];        /* ATA/NVMe serial, NUL-terminated, space-trimmed */
    char model[41];         /* model string, same conventions */
    uint64_t total_sectors; /* capacity in 512-byte sectors */
} hype_disk_entry_t;

/* 8 covers every machine hype targets (the AMD laptop has 1, the QEMU rigs 2-3)
 * with headroom, and keeps the whole inventory a few hundred bytes of .bss. */
#define HYPE_DISK_INVENTORY_MAX 8u

typedef struct {
    hype_disk_entry_t disks[HYPE_DISK_INVENTORY_MAX];
    unsigned int count;
    unsigned int dropped; /* disks seen but not recorded -- capacity exceeded */
} hype_disk_inventory_t;

/* Empty the inventory. Must be called before the first add. */
void hype_disk_inventory_reset(hype_disk_inventory_t *inv);

/*
 * Record one disk. Returns 0 if stored, -1 if the inventory is full (and
 * increments `dropped` so the caller can SAY so -- a silently truncated
 * inventory reads exactly like a disk that is not present, which is the
 * confusion this whole ticket is about).
 *
 * `serial` and `model` may be 0 or over-long; both are copied with truncation.
 */
int hype_disk_inventory_add(hype_disk_inventory_t *inv, hype_disk_bus_t bus, uint64_t bar_phys,
                            unsigned int port, const char *serial, const char *model,
                            uint64_t total_sectors);

/*
 * Find a disk by exact serial. Returns its index, or -1 if no disk carries that
 * serial. A NULL or empty `serial` never matches: a disk that reported no serial
 * must not be selectable as a write target by an empty config value.
 */
int hype_disk_inventory_find_serial(const hype_disk_inventory_t *inv, const char *serial);

/* Entry at `idx`, or 0 if out of range. */
const hype_disk_entry_t *hype_disk_inventory_get(const hype_disk_inventory_t *inv,
                                                 unsigned int idx);

/* How many disks share `serial`. Non-unique serials mean "matched by serial" is
 * ambiguous, and picking one arbitrarily is not acceptable for a WRITE target --
 * the caller must refuse rather than guess. */
unsigned int hype_disk_inventory_count_serial(const hype_disk_inventory_t *inv,
                                              const char *serial);

#endif /* HYPE_CORE_DISK_INVENTORY_H */
