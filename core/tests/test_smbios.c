#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../devices/smbios.h"

int hype_smbios_build(const hype_smbios_config_t *cfg, uint8_t *anchor, uint32_t anchor_size,
                      uint8_t *tables, uint32_t tables_size, hype_smbios_layout_t *out);

static uint8_t anchor[64];
static uint8_t tables[1024];

static void test_anchor_is_valid(void) {
    hype_smbios_config_t cfg = {1u, 3ull * 1024 * 1024 * 1024};
    hype_smbios_layout_t lay;
    unsigned i, sum = 0;

    assert(hype_smbios_build(&cfg, anchor, sizeof anchor, tables, sizeof tables, &lay) == 0);
    assert(memcmp(anchor, "_SM3_", 5) == 0);
    /* The checksum is what firmware validates before trusting anything else. */
    for (i = 0; i < lay.anchor_length; i++) sum += anchor[i];
    assert((sum & 0xFFu) == 0);
    /* Firmware relocates the structures and patches the address in. */
    assert(anchor[16] == 0 && anchor[17] == 0);
}

static void test_structures_are_walkable(void) {
    hype_smbios_config_t cfg = {1u, 3ull * 1024 * 1024 * 1024};
    hype_smbios_layout_t lay;
    uint32_t off = 0;
    int saw_bios = 0, saw_system = 0, saw_cpu = 0, saw_mem = 0, saw_end = 0;

    assert(hype_smbios_build(&cfg, anchor, sizeof anchor, tables, sizeof tables, &lay) == 0);

    /* Walk exactly as a consumer does: formatted area, then the string set,
     * ending at the double NUL. A structure that miscounts either makes the
     * whole table unparseable from that point on. */
    while (off < lay.tables_length) {
        uint8_t type = tables[off];
        uint8_t len = tables[off + 1];
        assert(len >= 4);
        off += len;
        while (off + 1 < lay.tables_length && !(tables[off] == 0 && tables[off + 1] == 0)) off++;
        off += 2;
        if (type == HYPE_SMBIOS_TYPE_BIOS) saw_bios = 1;
        if (type == HYPE_SMBIOS_TYPE_SYSTEM) saw_system = 1;
        if (type == HYPE_SMBIOS_TYPE_PROCESSOR) saw_cpu = 1;
        if (type == HYPE_SMBIOS_TYPE_MEMORY_DEVICE) saw_mem = 1;
        if (type == HYPE_SMBIOS_TYPE_END) { saw_end = 1; break; }
    }
    assert(saw_bios && saw_system && saw_cpu && saw_mem && saw_end);
}

static void test_reports_the_machine_hype_actually_gives(void) {
    hype_smbios_config_t cfg = {2u, 2ull * 1024 * 1024 * 1024};
    hype_smbios_layout_t lay;
    uint32_t off = 0;
    unsigned cpus = 0;
    int checked_size = 0;

    assert(hype_smbios_build(&cfg, anchor, sizeof anchor, tables, sizeof tables, &lay) == 0);
    while (off < lay.tables_length) {
        uint8_t type = tables[off];
        uint8_t len = tables[off + 1];
        if (type == HYPE_SMBIOS_TYPE_PROCESSOR) cpus++;
        if (type == HYPE_SMBIOS_TYPE_MEMORY_DEVICE) {
            /* Size in MB must be the RAM the guest was really given, not a
             * round number picked here. */
            uint16_t mb = (uint16_t)(tables[off + 12] | (tables[off + 13] << 8));
            assert(mb == 2048);
            checked_size = 1;
        }
        off += len;
        while (off + 1 < lay.tables_length && !(tables[off] == 0 && tables[off + 1] == 0)) off++;
        off += 2;
        if (type == HYPE_SMBIOS_TYPE_END) break;
    }
    /* One processor structure per vCPU -- no more, no fewer. */
    assert(cpus == 2);
    assert(checked_size);
}

static void test_rejects_buffers_it_cannot_fill(void) {
    hype_smbios_config_t cfg = {1u, 1024ull * 1024 * 1024};
    hype_smbios_layout_t lay;
    uint8_t tiny[8];
    assert(hype_smbios_build(&cfg, anchor, sizeof anchor, tiny, sizeof tiny, &lay) == -1);
    assert(hype_smbios_build(&cfg, tiny, sizeof tiny, tables, sizeof tables, &lay) == -1);
    /* A machine with no processors is not a machine. */
    cfg.cpu_count = 0;
    assert(hype_smbios_build(&cfg, anchor, sizeof anchor, tables, sizeof tables, &lay) == -1);
}

int main(void) {
    test_anchor_is_valid();
    test_structures_are_walkable();
    test_reports_the_machine_hype_actually_gives();
    test_rejects_buffers_it_cannot_fill();
    printf("test_smbios: all tests passed\n");
    return 0;
}
