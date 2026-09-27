#ifndef HYPE_NUMA_H
#define HYPE_NUMA_H

#include <stdint.h>

/*
 * SMP-19 (#475): the host's NUMA topology, from ACPI SRAT (and SLIT when present), and the pure
 * placement decision built on it.
 *
 * Table layouts follow the vendored edk2/MdePkg/Include/IndustryStandard/Acpi63.h (packed):
 *   SRAT = 36-byte header + UINT32 Reserved1 + UINT64 Reserved2, then entries from offset 48:
 *     type 0 Processor Local APIC/SAPIC Affinity (16 B): [2] domain 7:0, [3] APIC ID,
 *            [4..7] flags (bit 0 enabled), [9..11] domain 31:8
 *     type 1 Memory Affinity (40 B): [2..5] domain, [8..15] base, [16..23] length,
 *            [28..31] flags (bit 0 enabled)
 *     type 2 Processor Local x2APIC Affinity (24 B): [4..7] domain, [8..11] x2APIC ID,
 *            [12..15] flags (bit 0 enabled)
 *   SLIT = 36-byte header + UINT64 locality count N, then N x N distance bytes.
 *
 * Node membership comes from the table, never from enumeration order (#360: APIC IDs are sparse).
 * A host with no SRAT, or one domain, is ONE node: every decision below is then an exact no-op.
 */

#define HYPE_NUMA_MAX_CPUS 256u
#define HYPE_NUMA_MAX_MEM 64u
#define HYPE_NUMA_MAX_NODES 8u

typedef struct {
    unsigned int ncpu;
    uint32_t cpu_apic[HYPE_NUMA_MAX_CPUS];
    uint8_t cpu_node[HYPE_NUMA_MAX_CPUS]; /* dense node index, not the raw domain number */
    unsigned int nmem;
    uint64_t mem_base[HYPE_NUMA_MAX_MEM];
    uint64_t mem_len[HYPE_NUMA_MAX_MEM];
    uint8_t mem_node[HYPE_NUMA_MAX_MEM];
    unsigned int nnodes;                     /* >= 1 after hype_numa_init() */
    uint32_t domain[HYPE_NUMA_MAX_NODES];    /* raw proximity domain of each dense index */
    int has_slit;
    uint8_t dist[HYPE_NUMA_MAX_NODES][HYPE_NUMA_MAX_NODES];
    unsigned int dropped;                    /* entries past a capacity, or malformed */
} hype_numa_t;

/* One node, no CPUs or ranges known: what a host without SRAT is. */
void hype_numa_init(hype_numa_t *n);

/* Parse an SRAT. Returns 0, or -1 when the table is too short or not an SRAT (n then stays one
 * node). Disabled entries are skipped; entries past a capacity are counted in `dropped`. */
int hype_numa_parse_srat(hype_numa_t *n, const uint8_t *srat, uint32_t len);

/* Parse a SLIT into dist[][] for the domains the SRAT named. Returns 0 or -1. */
int hype_numa_parse_slit(hype_numa_t *n, const uint8_t *slit, uint32_t len);

/* Dense node index of a CPU / a physical address; 0 when unknown (the one-node answer). */
unsigned int hype_numa_node_of_apic(const hype_numa_t *n, uint32_t apic_id);
unsigned int hype_numa_node_of_addr(const hype_numa_t *n, uint64_t addr);

/*
 * The node a VM should live on: the lowest-numbered node that has `want_cores` free cores AND
 * `want_bytes` free RAM. Returns that node, or -1 when no single node holds both -- the caller
 * then places across nodes and says so (decision 39: prefer node-local, fall back with a loud
 * diagnostic, never refuse over locality). With one node this is always 0 when the VM fits at
 * all, so a single-node host takes exactly today's decisions.
 */
int hype_numa_pick_node(const hype_numa_t *n, const unsigned int *free_cores,
                        const uint64_t *free_bytes, unsigned int want_cores, uint64_t want_bytes);

#endif
