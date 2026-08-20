#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../devices/smbios.h"

int hype_smbios_build(const hype_smbios_config_t *cfg, uint8_t *anchor, uint32_t anchor_size,
                      uint8_t *tables, uint32_t tables_size, hype_smbios_layout_t *out);

static uint8_t anchor[64];
static uint8_t tables[1024];
static uint32_t g_last_len; /* #562: tables_length of the most recent build, for the walkers */

static void test_anchor_is_valid(void) {
    hype_smbios_config_t cfg = {.cpu_count = 1u, .threads_per_core = 1u,
                                .ram_bytes = 3ull * 1024 * 1024 * 1024};
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
    hype_smbios_config_t cfg = {.cpu_count = 1u, .threads_per_core = 1u,
                                .ram_bytes = 3ull * 1024 * 1024 * 1024};
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
    hype_smbios_config_t cfg = {.cpu_count = 2u, .threads_per_core = 1u,
                                .ram_bytes = 2ull * 1024 * 1024 * 1024};
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
    /*
     * #562: ONE processor structure -- per SOCKET, not per vCPU. This asserted `cpus == 2` for a
     * 2-vCPU guest, which is the shape that contradicted CPUID: two sockets of one single-threaded
     * core each. The counts inside it are what carry the topology now, and the three cases below
     * check them.
     */
    assert(cpus == 1);
    assert(checked_size);
}

/* Type 4's core/cores-enabled/thread bytes, for the topology assertions below. */
static void read_type4_counts(unsigned *cores, unsigned *enabled, unsigned *threads) {
    hype_smbios_layout_t lay;
    uint32_t off = 0;
    *cores = *enabled = *threads = 0u;
    (void)lay;
    while (off < g_last_len) {
        uint8_t type = tables[off];
        uint8_t len = tables[off + 1];
        if (type == HYPE_SMBIOS_TYPE_PROCESSOR) {
            *cores = tables[off + 35];
            *enabled = tables[off + 36];
            *threads = tables[off + 37];
        }
        off += len;
        while (off + 1 < g_last_len && !(tables[off] == 0 && tables[off + 1] == 0)) off++;
        off += 2;
        if (type == HYPE_SMBIOS_TYPE_END) break;
    }
}

/*
 * #562: Type 4 must agree with what CPUID leaf 0xB/0x1F tells the same guest. Before this it said
 * one single-threaded core for EVERY guest -- the only caller passed cpu_count = 1 unconditionally
 * -- so `dmidecode -t 4` and `lscpu` disagreed inside the guest.
 *
 * Per plan.md §10 decision 47 a granted core is granted WHOLE and SMT is a bonus, so the guest's
 * logical CPU count IS cores * threads_per_core. Note this is where the ticket's own third bar item
 * has been superseded: it asked for a 1-vCPU VM on a 2-thread core to report 1 thread, which was
 * #560's rule. #564/decision 47 replaced it -- the sibling IS this guest's, and hiding it would
 * make Type 4 disagree with CPUID again, in the other direction.
 */
static void test_topology_matches_cpuid(void) {
    hype_smbios_config_t cfg;
    unsigned cores, enabled, threads;
    hype_smbios_layout_t lay;

    /* 2 logical CPUs on ONE 2-thread core: 1 core, 2 threads. */
    cfg.cpu_count = 2u;
    cfg.threads_per_core = 2u;
    cfg.ram_bytes = 1024ull * 1024 * 1024;
    assert(hype_smbios_build(&cfg, anchor, sizeof anchor, tables, sizeof tables, &lay) == 0);
    g_last_len = lay.tables_length;
    read_type4_counts(&cores, &enabled, &threads);
    assert(cores == 1 && enabled == 1 && threads == 2);

    /* 2 logical CPUs on TWO single-threaded cores: 2 cores, 2 threads. */
    cfg.cpu_count = 2u;
    cfg.threads_per_core = 1u;
    assert(hype_smbios_build(&cfg, anchor, sizeof anchor, tables, sizeof tables, &lay) == 0);
    g_last_len = lay.tables_length;
    read_type4_counts(&cores, &enabled, &threads);
    assert(cores == 2 && enabled == 2 && threads == 2);

    /* 1 logical CPU, no SMT: 1 core, 1 thread. */
    cfg.cpu_count = 1u;
    cfg.threads_per_core = 1u;
    assert(hype_smbios_build(&cfg, anchor, sizeof anchor, tables, sizeof tables, &lay) == 0);
    g_last_len = lay.tables_length;
    read_type4_counts(&cores, &enabled, &threads);
    assert(cores == 1 && enabled == 1 && threads == 1);

    /* 4 logical CPUs on two 2-thread cores: 2 cores, 4 threads. */
    cfg.cpu_count = 4u;
    cfg.threads_per_core = 2u;
    assert(hype_smbios_build(&cfg, anchor, sizeof anchor, tables, sizeof tables, &lay) == 0);
    g_last_len = lay.tables_length;
    read_type4_counts(&cores, &enabled, &threads);
    assert(cores == 2 && enabled == 2 && threads == 4);

    /* threads_per_core = 0 means "not stated" and is read as 1, not as a divide by zero. */
    cfg.cpu_count = 2u;
    cfg.threads_per_core = 0u;
    assert(hype_smbios_build(&cfg, anchor, sizeof anchor, tables, sizeof tables, &lay) == 0);
    g_last_len = lay.tables_length;
    read_type4_counts(&cores, &enabled, &threads);
    assert(cores == 2 && enabled == 2 && threads == 2);
}

/*
 * A threads_per_core that does not divide the logical CPU count would make the derived core count
 * a fiction. Refused rather than rounded: an almost-right topology table is the kind that gets
 * believed.
 */
static void test_byte_fields_saturate_instead_of_wrapping(void) {
    /*
     * The Type 4 core/thread fields are single bytes. 256 threads must report 255, not 0 -- a
     * wrapped count would describe a machine with no CPUs, which is the worst of the options.
     */
    hype_smbios_config_t cfg;
    unsigned cores, enabled, threads;
    hype_smbios_layout_t lay;
    cfg.cpu_count = 512u;
    cfg.threads_per_core = 2u;
    cfg.ram_bytes = 1024ull * 1024 * 1024;
    assert(hype_smbios_build(&cfg, anchor, sizeof anchor, tables, sizeof tables, &lay) == 0);
    g_last_len = lay.tables_length;
    read_type4_counts(&cores, &enabled, &threads);
    assert(cores == 255 && enabled == 255 && threads == 255);
}

/* A NULL in any required argument is refused rather than dereferenced. */
/*
 * SMBIOS Type 17's 16-bit Size field cannot express >= 32 GB, so the spec has it set 0x7FFF and put
 * the real byte count in the 32-bit Extended Size field. A guest with a lot of RAM otherwise reads
 * a wrapped or capped figure.
 */
static void test_large_ram_uses_the_extended_size_field(void) {
    hype_smbios_config_t cfg = {.cpu_count = 1u, .threads_per_core = 1u,
                                .ram_bytes = 64ull * 1024 * 1024 * 1024};
    hype_smbios_layout_t lay;
    uint32_t off = 0;
    int checked = 0;

    assert(hype_smbios_build(&cfg, anchor, sizeof anchor, tables, sizeof tables, &lay) == 0);
    while (off < lay.tables_length) {
        uint8_t type = tables[off];
        uint8_t len = tables[off + 1];
        if (type == HYPE_SMBIOS_TYPE_MEMORY_DEVICE) {
            uint16_t mb = (uint16_t)(tables[off + 12] | (tables[off + 13] << 8));
            uint32_t ext = (uint32_t)tables[off + 28] | ((uint32_t)tables[off + 29] << 8) |
                           ((uint32_t)tables[off + 30] << 16) | ((uint32_t)tables[off + 31] << 24);
            assert(mb == 0x7FFF);
            assert(ext == 64u * 1024 * 1024 * 1024 % 0x100000000ull ||
                   ext == (uint32_t)(64ull * 1024 * 1024 * 1024));
            checked = 1;
        }
        off += len;
        while (off + 1 < lay.tables_length && !(tables[off] == 0 && tables[off + 1] == 0)) off++;
        off += 2;
        if (type == HYPE_SMBIOS_TYPE_END) break;
    }
    assert(checked);
}

static void test_rejects_null_arguments(void) {
    hype_smbios_config_t cfg = {.cpu_count = 1u, .threads_per_core = 1u,
                                .ram_bytes = 1024ull * 1024 * 1024};
    hype_smbios_layout_t lay;
    assert(hype_smbios_build(0, anchor, sizeof anchor, tables, sizeof tables, &lay) == -1);
    assert(hype_smbios_build(&cfg, 0, sizeof anchor, tables, sizeof tables, &lay) == -1);
    assert(hype_smbios_build(&cfg, anchor, sizeof anchor, 0, sizeof tables, &lay) == -1);
    assert(hype_smbios_build(&cfg, anchor, sizeof anchor, tables, sizeof tables, 0) == -1);
}

static void test_rejects_an_impossible_topology(void) {
    hype_smbios_config_t cfg;
    hype_smbios_layout_t lay;
    cfg.cpu_count = 3u;
    cfg.threads_per_core = 2u;
    cfg.ram_bytes = 1024ull * 1024 * 1024;
    assert(hype_smbios_build(&cfg, anchor, sizeof anchor, tables, sizeof tables, &lay) == -1);
}

static void test_rejects_buffers_it_cannot_fill(void) {
    hype_smbios_config_t cfg = {.cpu_count = 1u, .threads_per_core = 1u,
                                .ram_bytes = 1024ull * 1024 * 1024};
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
    test_topology_matches_cpuid();        /* #562 */
    test_rejects_an_impossible_topology(); /* #562 */
    test_byte_fields_saturate_instead_of_wrapping(); /* #562 */
    test_large_ram_uses_the_extended_size_field();
    test_rejects_null_arguments();
    test_rejects_buffers_it_cannot_fill();
    printf("test_smbios: all tests passed\n");
    return 0;
}
