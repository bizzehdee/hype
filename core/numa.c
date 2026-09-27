#include "numa.h"

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

void hype_numa_init(hype_numa_t *n) {
    unsigned int i, j;
    n->ncpu = 0;
    n->nmem = 0;
    n->nnodes = 1;
    n->domain[0] = 0;
    n->has_slit = 0;
    n->dropped = 0;
    for (i = 0; i < HYPE_NUMA_MAX_NODES; i++) {
        for (j = 0; j < HYPE_NUMA_MAX_NODES; j++) n->dist[i][j] = (uint8_t)(i == j ? 10u : 20u);
    }
}

/* Dense index for a raw proximity domain, adding it if new; -1 when out of room. */
static int node_index(hype_numa_t *n, uint32_t domain, int *first) {
    unsigned int i;
    if (*first) {
        /* The one-node default carries no real domain yet: the first one seen replaces it. */
        n->domain[0] = domain;
        n->nnodes = 1;
        *first = 0;
        return 0;
    }
    for (i = 0; i < n->nnodes; i++) {
        if (n->domain[i] == domain) return (int)i;
    }
    if (n->nnodes >= HYPE_NUMA_MAX_NODES) return -1;
    n->domain[n->nnodes] = domain;
    return (int)n->nnodes++;
}

int hype_numa_parse_srat(hype_numa_t *n, const uint8_t *srat, uint32_t len) {
    uint32_t off = 48u, tlen;
    int first = 1;

    if (len < 48u || srat[0] != 'S' || srat[1] != 'R' || srat[2] != 'A' || srat[3] != 'T') return -1;
    tlen = rd32(srat + 4);
    if (tlen < len) len = tlen;
    while (off + 2u <= len) {
        uint8_t type = srat[off];
        uint8_t elen = srat[off + 1];
        const uint8_t *e = srat + off;
        uint32_t domain = 0, apic = 0, flags = 0;
        int is_cpu = 0, idx;

        if (elen < 2u || off + elen > len) {
            n->dropped++;
            break; /* a zero or overlong length cannot be stepped over safely */
        }
        if (type == 0u && elen >= 16u) {
            domain = (uint32_t)e[2] | ((uint32_t)e[9] << 8) | ((uint32_t)e[10] << 16) |
                     ((uint32_t)e[11] << 24);
            apic = e[3];
            flags = rd32(e + 4);
            is_cpu = 1;
        } else if (type == 2u && elen >= 24u) {
            domain = rd32(e + 4);
            apic = rd32(e + 8);
            flags = rd32(e + 12);
            is_cpu = 1;
        } else if (type == 1u && elen >= 40u) {
            domain = rd32(e + 2);
            flags = rd32(e + 28);
            if ((flags & 1u) != 0u && rd64(e + 16) != 0u) {
                idx = node_index(n, domain, &first);
                if (idx < 0 || n->nmem >= HYPE_NUMA_MAX_MEM) {
                    n->dropped++;
                } else {
                    n->mem_base[n->nmem] = rd64(e + 8);
                    n->mem_len[n->nmem] = rd64(e + 16);
                    n->mem_node[n->nmem] = (uint8_t)idx;
                    n->nmem++;
                }
            }
        }
        if (is_cpu && (flags & 1u) != 0u) {
            idx = node_index(n, domain, &first);
            if (idx < 0 || n->ncpu >= HYPE_NUMA_MAX_CPUS) {
                n->dropped++;
            } else {
                n->cpu_apic[n->ncpu] = apic;
                n->cpu_node[n->ncpu] = (uint8_t)idx;
                n->ncpu++;
            }
        }
        off += elen;
    }
    return 0;
}

int hype_numa_parse_slit(hype_numa_t *n, const uint8_t *slit, uint32_t len) {
    uint64_t loc;
    unsigned int i, j;

    if (len < 44u || slit[0] != 'S' || slit[1] != 'L' || slit[2] != 'I' || slit[3] != 'T') return -1;
    loc = rd64(slit + 36);
    if (loc == 0u || loc > 255u || 44u + loc * loc > len) return -1;
    for (i = 0; i < n->nnodes; i++) {
        for (j = 0; j < n->nnodes; j++) {
            if (n->domain[i] < loc && n->domain[j] < loc) {
                n->dist[i][j] = slit[44u + n->domain[i] * loc + n->domain[j]];
            }
        }
    }
    n->has_slit = 1;
    return 0;
}

unsigned int hype_numa_node_of_apic(const hype_numa_t *n, uint32_t apic_id) {
    unsigned int i;
    for (i = 0; i < n->ncpu; i++) {
        if (n->cpu_apic[i] == apic_id) return n->cpu_node[i];
    }
    return 0;
}

unsigned int hype_numa_node_of_addr(const hype_numa_t *n, uint64_t addr) {
    unsigned int i;
    for (i = 0; i < n->nmem; i++) {
        if (addr >= n->mem_base[i] && addr - n->mem_base[i] < n->mem_len[i]) return n->mem_node[i];
    }
    return 0;
}

int hype_numa_pick_node(const hype_numa_t *n, const unsigned int *free_cores,
                        const uint64_t *free_bytes, unsigned int want_cores, uint64_t want_bytes) {
    unsigned int i;
    for (i = 0; i < n->nnodes; i++) {
        if (free_cores[i] >= want_cores && free_bytes[i] >= want_bytes) return (int)i;
    }
    return -1;
}
