#include "host_nic.h"

static hype_host_nic_driver_t g_drivers[HYPE_HOST_NIC_MAX_DRIVERS];
static unsigned int g_driver_count;

int hype_host_nic_register(const hype_host_nic_driver_t *driver) {
    hype_host_nic_driver_t *slot;
    unsigned int i;

    if (driver == 0 || driver->attach == 0) {
        return 0;
    }
    if (driver->match_count == 0u || driver->match_count > HYPE_HOST_NIC_MAX_MATCH_IDS) {
        return 0;
    }
    if (g_driver_count >= HYPE_HOST_NIC_MAX_DRIVERS) {
        return 0;
    }

    /* Field-by-field: `driver` contains a match_ids[] array, and whole-struct
     * assignment of a struct containing an array emits a hidden memcpy call
     * this freestanding build cannot link (no libc). */
    slot = &g_drivers[g_driver_count];
    slot->name = driver->name;
    slot->match_count = driver->match_count;
    slot->probe = driver->probe;
    slot->attach = driver->attach;
    for (i = 0; i < driver->match_count; i++) {
        slot->match_ids[i].vendor_id = driver->match_ids[i].vendor_id;
        slot->match_ids[i].device_id = driver->match_ids[i].device_id;
    }
    g_driver_count++;
    return 1;
}

const hype_host_nic_driver_t *hype_host_nic_match(uint16_t vendor_id, uint16_t device_id) {
    unsigned int i, j;
    for (i = 0; i < g_driver_count; i++) {
        const hype_host_nic_driver_t *d = &g_drivers[i];
        for (j = 0; j < d->match_count; j++) {
            if (d->match_ids[j].vendor_id == vendor_id && d->match_ids[j].device_id == device_id) {
                return d;
            }
        }
    }
    return 0;
}

unsigned int hype_host_nic_probe_all(hype_host_pci_read32_fn read32, uint8_t max_bus,
                                     hype_host_nic_bound_t *out, unsigned int out_cap) {
    unsigned int filled = 0;
    uint32_t start_bdf = 0;
    hype_host_nic_t loc;
    uint32_t found_bdf;

    if (read32 == 0 || out == 0) {
        return 0;
    }

    while (filled < out_cap && hype_host_pci_find_nic_from(read32, max_bus, start_bdf, &loc, &found_bdf)) {
        const hype_host_nic_driver_t *driver = hype_host_nic_match(loc.vendor_id, loc.device_id);
        start_bdf = found_bdf + 1u;

        if (driver == 0) {
            continue; /* class 0x02 present, no driver owns this vendor/device -- not an error */
        }
        if (driver->probe != 0 && !driver->probe(&loc)) {
            continue; /* vendor/device matched but the driver's own gate declined it */
        }

        out[filled].loc = loc;
        out[filled].driver = driver;
        out[filled].mac_valid = 0;
        out[filled].link_up = 0;
        out[filled].attached = 0;
        out[filled].mac[0] = out[filled].mac[1] = out[filled].mac[2] = 0;
        out[filled].mac[3] = out[filled].mac[4] = out[filled].mac[5] = 0;

        if (driver->attach(&loc, &out[filled])) {
            out[filled].attached = 1;
        }
        filled++;
    }

    return filled;
}

void hype_host_nic_registry_reset(void) {
    g_driver_count = 0;
}
