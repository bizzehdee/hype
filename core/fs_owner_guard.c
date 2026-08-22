#include "fs_owner_guard.h"

void hype_fs_owner_guard_init(hype_fs_owner_guard_t *g) {
    if (g == 0) {
        return;
    }
    g->bound = 0;
    g->owner_apic_id = 0;
}

void hype_fs_owner_guard_bind(hype_fs_owner_guard_t *g, uint32_t owner_apic_id) {
    if (g == 0) {
        return;
    }
    g->bound = 1;
    g->owner_apic_id = owner_apic_id;
}

int hype_fs_owner_guard_check(const hype_fs_owner_guard_t *g, uint32_t executing_apic_id) {
    if (g == 0) {
        return 0;
    }
    if (!g->bound) {
        return 1;
    }
    return g->owner_apic_id == executing_apic_id;
}
