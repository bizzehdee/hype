#include "disk_inventory.h"
#include "strutil.h"

void hype_disk_inventory_reset(hype_disk_inventory_t *inv) {
    if (inv == 0) return;
    inv->count = 0;
    inv->dropped = 0;
}

int hype_disk_inventory_add(hype_disk_inventory_t *inv, hype_disk_bus_t bus, uint64_t bar_phys,
                            unsigned int port, const char *serial, const char *model,
                            uint64_t total_sectors) {
    hype_disk_entry_t *e;
    if (inv == 0) return -1;
    if (inv->count >= HYPE_DISK_INVENTORY_MAX) {
        inv->dropped++;
        return -1;
    }
    e = &inv->disks[inv->count];
    e->bus = bus;
    e->bar_phys = bar_phys;
    e->port = port;
    e->total_sectors = total_sectors;
    (void)hype_strlcpy(e->serial, (serial != 0) ? serial : "", sizeof(e->serial));
    (void)hype_strlcpy(e->model, (model != 0) ? model : "", sizeof(e->model));
    inv->count++;
    return 0;
}

int hype_disk_inventory_find_serial(const hype_disk_inventory_t *inv, const char *serial) {
    unsigned int i;
    if (inv == 0 || serial == 0 || serial[0] == '\0') return -1;
    for (i = 0; i < inv->count; i++) {
        if (hype_streq(inv->disks[i].serial, serial)) return (int)i;
    }
    return -1;
}

const hype_disk_entry_t *hype_disk_inventory_get(const hype_disk_inventory_t *inv,
                                                 unsigned int idx) {
    if (inv == 0 || idx >= inv->count) return 0;
    return &inv->disks[idx];
}

unsigned int hype_disk_inventory_count_serial(const hype_disk_inventory_t *inv,
                                              const char *serial) {
    unsigned int i;
    unsigned int n = 0;
    if (inv == 0 || serial == 0 || serial[0] == '\0') return 0;
    for (i = 0; i < inv->count; i++) {
        if (hype_streq(inv->disks[i].serial, serial)) n++;
    }
    return n;
}
