#ifndef HYPE_CORE_HOST_NIC_H
#define HYPE_CORE_HOST_NIC_H

#include <stdint.h>
#include "host_pci.h"

/*
 * HNET-1 (#398): host NIC driver-model + PCI bind, layered on the existing
 * host PCI enumerator (core/host_pci.c, GLADDER-10 #147 / NET-1 #80). Does
 * NOT add a second PCI enumerator -- hype_host_pci_find_nic[_from]() already
 * walks class 0x02 (network controller) functions; this module adds the
 * driver-registration and match/bind layer on top, per plan.md §10 decision
 * 34 (one NIC vtable, multiple vendor drivers behind it) and decision 35 (the
 * registration seam that keeps a driver a clean module-extraction candidate).
 *
 * A driver never reaches hype's PCI/DMA/IRQ internals through a private
 * cross-reference: it registers a match table (vendor/device ID pairs) plus
 * probe/attach callbacks, and this module's matcher decides which driver (if
 * any) owns a discovered NIC function. The matcher is pure logic, unit-tested
 * against a synthetic driver table -- no live NIC needed, same as host_pci's
 * own injected-read32 tests.
 */

#define HYPE_HOST_NIC_MAX_DRIVERS 8
#define HYPE_HOST_NIC_MAX_MATCH_IDS 8

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
} hype_host_nic_match_id_t;

/*
 * `probe` is offered a discovered function's location/BAR/ids and returns 1
 * if this driver definitely owns it (beyond the vendor/device match already
 * checked -- e.g. a prog-if or revision gate), else 0. May be NULL, meaning
 * "the vendor/device match table is sufficient, no further check."
 *
 * `attach` is called exactly once, only after `probe` (or the plain table
 * match, if probe is NULL) accepts the function; it does the real MMIO bind
 * (map the BAR, read the MAC, enable bus-master) and fills *out. Returns 1 on
 * success, 0 on failure (e.g. the MMIO window did not come up).
 */
typedef int (*hype_host_nic_probe_fn)(const hype_host_nic_t *loc);
typedef struct hype_host_nic_bound hype_host_nic_bound_t;
typedef int (*hype_host_nic_attach_fn)(const hype_host_nic_t *loc, hype_host_nic_bound_t *out);

typedef struct {
    const char *name; /* for diagnostics, e.g. "r8169" */
    hype_host_nic_match_id_t match_ids[HYPE_HOST_NIC_MAX_MATCH_IDS];
    unsigned int match_count;
    hype_host_nic_probe_fn probe;   /* optional, may be NULL */
    hype_host_nic_attach_fn attach; /* required */
} hype_host_nic_driver_t;

/* Registers a driver. Returns 1 on success, 0 if the registry is already
 * full, `driver` is NULL, `driver->attach` is NULL, or match_count is 0 or
 * exceeds HYPE_HOST_NIC_MAX_MATCH_IDS -- a driver with no match table can
 * never bind anything, and a silently-truncated one can bind the WRONG
 * chip, so both are refused outright rather than half-registered. */
int hype_host_nic_register(const hype_host_nic_driver_t *driver);

/*
 * Finds the registered driver whose match table contains (vendor_id,
 * device_id), or NULL if none does. First-registered-wins on a duplicate
 * entry (should not happen with distinct real drivers; deterministic rather
 * than undefined if it does). Pure -- reads the registry, calls nothing.
 */
const hype_host_nic_driver_t *hype_host_nic_match(uint16_t vendor_id, uint16_t device_id);

/*
 * Per-NIC bound state: what every driver fills in on a successful attach,
 * regardless of vendor. `link_up` is refreshed by the driver's own poll, not
 * this module. `poll_irq` is the shared host_pci_irq registration this NIC's
 * driver uses for its RX/TX drain, if it registered one (0/NULL if it hasn't
 * attached to the poll facility, e.g. before HNET-3 wiring lands for it).
 */
struct hype_host_nic_bound {
    hype_host_nic_t loc; /* location + BAR, as found by host_pci */
    const hype_host_nic_driver_t *driver;
    uint8_t mac[6];
    int mac_valid;
    int link_up;
    int attached;
};

/*
 * Probes every NIC function host_pci finds (bus 0..max_bus) against the
 * driver registry, attaching the first match for each. Fills `out[]` (up to
 * `out_cap` entries) and returns the count filled. A NIC with no matching
 * driver is reported by hype_host_pci_find_nic_from's own logging path (the
 * caller's, not this module's) and simply does not appear in `out[]` --
 * matching how host_pci already treats "found the class, no owning driver"
 * as informative, not an error.
 */
unsigned int hype_host_nic_probe_all(hype_host_pci_read32_fn read32, uint8_t max_bus,
                                     hype_host_nic_bound_t *out, unsigned int out_cap);

/* Test/reinit hook, same rationale as hype_host_poll_reset(). */
void hype_host_nic_registry_reset(void);

#endif /* HYPE_CORE_HOST_NIC_H */
