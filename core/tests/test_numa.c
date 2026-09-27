#include <stdio.h>
#include <string.h>
#include "../numa.h"

static int failures = 0;
#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), (long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void w64(uint8_t *p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }

static uint32_t srat_begin(uint8_t *t) {
    memset(t, 0, 48);
    memcpy(t, "SRAT", 4);
    w32(t + 36, 1u);
    return 48u;
}
static uint32_t add_lapic(uint8_t *t, uint32_t off, uint32_t domain, uint8_t apic, uint32_t flags) {
    uint8_t *e = t + off;
    memset(e, 0, 16);
    e[0] = 0; e[1] = 16; e[2] = (uint8_t)domain; e[3] = apic; w32(e + 4, flags);
    e[9] = (uint8_t)(domain >> 8); e[10] = (uint8_t)(domain >> 16); e[11] = (uint8_t)(domain >> 24);
    return off + 16u;
}
static uint32_t add_x2apic(uint8_t *t, uint32_t off, uint32_t domain, uint32_t apic, uint32_t flags) {
    uint8_t *e = t + off;
    memset(e, 0, 24);
    e[0] = 2; e[1] = 24; w32(e + 4, domain); w32(e + 8, apic); w32(e + 12, flags);
    return off + 24u;
}
static uint32_t add_mem(uint8_t *t, uint32_t off, uint32_t domain, uint64_t base, uint64_t len, uint32_t flags) {
    uint8_t *e = t + off;
    memset(e, 0, 40);
    e[0] = 1; e[1] = 40; w32(e + 2, domain); w64(e + 8, base); w64(e + 16, len); w32(e + 28, flags);
    return off + 40u;
}
static void srat_end(uint8_t *t, uint32_t len) { w32(t + 4, len); }

static void test_two_nodes_sparse_ids(void) {
    uint8_t t[512];
    hype_numa_t n;
    uint32_t off = srat_begin(t);

    off = add_lapic(t, off, 3, 0, 1);
    off = add_lapic(t, off, 3, 2, 1);
    off = add_x2apic(t, off, 0x107, 16, 1);        /* domain 0x107: the 31:8 bits matter */
    off = add_x2apic(t, off, 0x107, 18, 1);
    off = add_lapic(t, off, 3, 4, 0);              /* disabled: skipped */
    off = add_mem(t, off, 3, 0x0, 0x80000000ull, 1);
    off = add_mem(t, off, 0x107, 0x100000000ull, 0x80000000ull, 1);
    off = add_mem(t, off, 3, 0x200000000ull, 0x1000ull, 0); /* disabled */
    srat_end(t, off);
    hype_numa_init(&n);
    CHECK_INT("parse ok", 0, hype_numa_parse_srat(&n, t, off));
    CHECK_INT("two nodes", 2, n.nnodes);
    CHECK_INT("dense 0 is domain 3", 3, n.domain[0]);
    CHECK_INT("dense 1 is domain 0x107", 0x107, n.domain[1]);
    CHECK_INT("four enabled CPUs", 4, n.ncpu);
    CHECK_INT("apic 2 on node 0", 0, hype_numa_node_of_apic(&n, 2));
    CHECK_INT("x2apic 18 on node 1", 1, hype_numa_node_of_apic(&n, 18));
    CHECK_INT("disabled apic 4 unknown -> 0", 0, hype_numa_node_of_apic(&n, 4));
    CHECK_INT("two enabled ranges", 2, n.nmem);
    CHECK_INT("addr 1 GiB on node 0", 0, hype_numa_node_of_addr(&n, 0x40000000ull));
    CHECK_INT("addr 5 GiB on node 1", 1, hype_numa_node_of_addr(&n, 0x140000000ull));
    CHECK_INT("addr past every range -> 0", 0, hype_numa_node_of_addr(&n, 0x900000000ull));
    CHECK_INT("nothing dropped", 0, n.dropped);
}

static void test_slit(void) {
    uint8_t t[512], s[44 + 0x108 * 0 + 64];
    hype_numa_t n;
    uint32_t off = srat_begin(t);
    off = add_lapic(t, off, 1, 0, 1);
    off = add_lapic(t, off, 2, 1, 1);
    srat_end(t, off);
    hype_numa_init(&n);
    hype_numa_parse_srat(&n, t, off);
    memset(s, 0, sizeof(s));
    memcpy(s, "SLIT", 4);
    w64(s + 36, 3);
    /* 3x3: domain 1<->2 is 32 */
    s[44 + 0] = 10; s[44 + 1] = 21; s[44 + 2] = 21;
    s[44 + 3] = 21; s[44 + 4] = 10; s[44 + 5] = 32;
    s[44 + 6] = 21; s[44 + 7] = 32; s[44 + 8] = 10;
    CHECK_INT("slit ok", 0, hype_numa_parse_slit(&n, s, 53));
    CHECK_INT("has slit", 1, n.has_slit);
    CHECK_INT("dist node0->node1 = domain1->domain2", 32, n.dist[0][1]);
    CHECK_INT("dist self", 10, n.dist[1][1]);
    CHECK_INT("not a SLIT", -1, hype_numa_parse_slit(&n, t, off));
    CHECK_INT("short SLIT", -1, hype_numa_parse_slit(&n, s, 40));
    w64(s + 36, 0);
    CHECK_INT("zero localities", -1, hype_numa_parse_slit(&n, s, 53));
    w64(s + 36, 9);
    CHECK_INT("matrix past the table", -1, hype_numa_parse_slit(&n, s, 53));
}

static void test_no_srat_is_one_node(void) {
    hype_numa_t n;
    uint8_t junk[64];
    unsigned int free_cores[1] = { 3 };
    uint64_t free_bytes[1] = { 1ull << 30 };
    memset(junk, 0, sizeof(junk));
    hype_numa_init(&n);
    CHECK_INT("not an SRAT", -1, hype_numa_parse_srat(&n, junk, sizeof(junk)));
    CHECK_INT("too short", -1, hype_numa_parse_srat(&n, (const uint8_t *)"SRAT", 4));
    CHECK_INT("still one node", 1, n.nnodes);
    CHECK_INT("single node: every VM that fits is node 0", 0,
              hype_numa_pick_node(&n, free_cores, free_bytes, 3, 1ull << 30));
    CHECK_INT("single node: a VM that does not fit anywhere", -1,
              hype_numa_pick_node(&n, free_cores, free_bytes, 4, 1));
}

static void test_pick_node(void) {
    hype_numa_t n;
    unsigned int free_cores[2] = { 2, 6 };
    uint64_t free_bytes[2] = { 8ull << 30, 2ull << 30 };
    hype_numa_init(&n);
    n.nnodes = 2;
    CHECK_INT("fits node 0", 0, hype_numa_pick_node(&n, free_cores, free_bytes, 2, 4ull << 30));
    CHECK_INT("cores only on node 1, RAM fits there", 1,
              hype_numa_pick_node(&n, free_cores, free_bytes, 4, 1ull << 30));
    CHECK_INT("cores on node 1 but RAM only on node 0: no single node", -1,
              hype_numa_pick_node(&n, free_cores, free_bytes, 4, 4ull << 30));
}

static void test_malformed_and_capacity(void) {
    uint8_t t[8192];
    hype_numa_t n;
    uint32_t off = srat_begin(t), i;
    for (i = 0; i < HYPE_NUMA_MAX_NODES + 1u; i++) off = add_lapic(t, off, i, (uint8_t)i, 1);
    srat_end(t, off);
    hype_numa_init(&n);
    hype_numa_parse_srat(&n, t, off);
    CHECK_INT("nodes capped", HYPE_NUMA_MAX_NODES, n.nnodes);
    CHECK_INT("the ninth domain is dropped, not merged", 1, n.dropped);

    off = srat_begin(t);
    for (i = 0; i < HYPE_NUMA_MAX_MEM + 2u; i++) off = add_mem(t, off, 0, (uint64_t)i << 32, 1ull << 20, 1);
    srat_end(t, off);
    hype_numa_init(&n);
    hype_numa_parse_srat(&n, t, off);
    CHECK_INT("ranges capped", HYPE_NUMA_MAX_MEM, n.nmem);
    CHECK_INT("two ranges dropped", 2, n.dropped);

    off = srat_begin(t);
    off = add_lapic(t, off, 0, 1, 1);
    t[off] = 0; t[off + 1] = 0;                    /* a zero-length entry ends the walk */
    srat_end(t, off + 16u);
    hype_numa_init(&n);
    hype_numa_parse_srat(&n, t, off + 16u);
    CHECK_INT("zero-length entry stops, counted", 1, n.dropped);
    CHECK_INT("the entry before it kept", 1, n.ncpu);

    off = srat_begin(t);
    t[off] = 9; t[off + 1] = 8;                    /* an unknown type is stepped over */
    off += 8u;
    off = add_lapic(t, off, 0, 5, 1);
    srat_end(t, off);
    w32(t + 4, off - 16u);                         /* header shorter than the buffer wins */
    hype_numa_init(&n);
    hype_numa_parse_srat(&n, t, off);
    CHECK_INT("header length bounds the walk", 0, n.ncpu);

    off = srat_begin(t);
    for (i = 0; i < HYPE_NUMA_MAX_CPUS + 1u; i++) off = add_x2apic(t, off, 0, i, 1);
    srat_end(t, off);
    hype_numa_init(&n);
    hype_numa_parse_srat(&n, t, off);
    CHECK_INT("cpus capped", HYPE_NUMA_MAX_CPUS, n.ncpu);
}

static void test_short_entries_and_edges(void) {
    uint8_t t[512], sl[64];
    hype_numa_t n;
    uint32_t off = srat_begin(t);
    uint8_t *e;

    e = t + off; memset(e, 0, 12); e[0] = 0; e[1] = 12; e[3] = 7; w32(e + 4, 1); off += 12; /* short lapic */
    e = t + off; memset(e, 0, 20); e[0] = 2; e[1] = 20; w32(e + 8, 9); w32(e + 12, 1); off += 20; /* short x2apic */
    e = t + off; memset(e, 0, 32); e[0] = 1; e[1] = 32; w32(e + 28, 1); off += 32;          /* short mem */
    off = add_mem(t, off, 0, 0x1000, 0, 1);                          /* zero length: ignored */
    off = add_x2apic(t, off, 5, 3, 0);                               /* disabled x2apic */
    off = add_lapic(t, off, 0, 1, 1);
    for (int i = 0; i < 8; i++) off = add_mem(t, off, (uint32_t)(10 + i), (uint64_t)i << 30, 1ull << 20, 1);
    srat_end(t, off);
    hype_numa_init(&n);
    hype_numa_parse_srat(&n, t, off);
    CHECK_INT("short entries ignored, one cpu", 1, n.ncpu);
    CHECK_INT("zero-length range ignored; ranges up to the node cap kept", 7, n.nmem);
    CHECK_INT("the range of a ninth domain dropped", 1, n.dropped);

    /* A SLIT smaller than the SRAT's domain numbers leaves the defaults for those pairs. */
    memset(sl, 0, sizeof(sl));
    memcpy(sl, "SLIT", 4);
    w64(sl + 36, 2);
    sl[44] = 10; sl[45] = 17; sl[46] = 17; sl[47] = 10;
    CHECK_INT("small slit ok", 0, hype_numa_parse_slit(&n, sl, 48));
    CHECK_INT("pair inside the slit", 10, n.dist[0][0]);
    CHECK_INT("pair outside the slit keeps its default", 20, n.dist[1][2]);
}

int main(void) {
    test_short_entries_and_edges();
    test_two_nodes_sparse_ids();
    test_slit();
    test_no_srat_is_one_node();
    test_pick_node();
    test_malformed_and_capacity();
    if (failures) {
        printf("test_numa: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_numa: all passed\n");
    return 0;
}
