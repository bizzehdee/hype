#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../qcow2_create.h"
#include "../blk_qcow2.h"
#include "../blk_backend.h"

static int failures = 0;

#define CHECK(msg, cond) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; } \
} while (0)

#define CS HYPE_QCOW2_CREATE_CLUSTER_SIZE

static void test_layout_shapes(void) {
    hype_qcow2_layout_t lo;

    CHECK("zero size refused", hype_qcow2_layout(0, &lo) != 0);
    CHECK("null out refused", hype_qcow2_layout(CS, 0) != 0);

    CHECK("1-cluster image", hype_qcow2_layout(1, &lo) == 0);
    CHECK("size rounds up to a cluster", lo.virtual_bytes == CS);
    CHECK("one data cluster", lo.data_clusters == 1u);
    CHECK("one L2", lo.l2_tables == 1u);
    CHECK("regions tile the file",
          lo.total_clusters == 1u + lo.rt_clusters + lo.rb_clusters + lo.l1_clusters +
                                   lo.l2_tables + lo.data_clusters);

    /* 1 GiB: the ticket's own number. */
    CHECK("1 GiB layout", hype_qcow2_layout(1024ull << 20, &lo) == 0);
    CHECK("16384 data clusters", lo.data_clusters == 16384u);
    CHECK("2 L2 tables", lo.l2_tables == 2u);
    CHECK("refcounts cover the whole file",
          lo.rb_clusters * (CS / 2u) >= lo.total_clusters);
    CHECK("table covers the blocks", lo.rt_clusters * (CS / 8u) >= lo.rb_clusters);
}

/* Build a whole small image in memory and make hype's OWN qcow2 reader/writer consume it:
 * correct virtual size, zeros everywhere, and a write that round-trips -- the strongest
 * self-check available without qemu (tools/487 adds qemu-img check on top). */
static uint8_t g_img[6u * 1024u * 1024u];
static uint64_t g_img_len;

static void test_created_image_via_own_reader(void) {
    static uint8_t cl[CS];
    static hype_blk_file_t raw;
    static hype_blk_backend_t raw_be, be;
    static hype_qcow2_t q;
    hype_qcow2_layout_t lo;
    uint64_t i;
    uint8_t sec[512], pat[512];

    CHECK("4 MiB layout", hype_qcow2_layout(4ull << 20, &lo) == 0);
    g_img_len = lo.total_clusters * CS;
    CHECK("fits the test arena", g_img_len <= sizeof(g_img));
    for (i = 0; i < lo.total_clusters; i++) {
        CHECK("render", hype_qcow2_create_cluster(&lo, i, cl) == 0);
        memcpy(g_img + i * CS, cl, CS);
    }
    CHECK("index out of range refused",
          hype_qcow2_create_cluster(&lo, lo.total_clusters, cl) != 0);

    hype_blk_file_init(&raw, &raw_be, g_img, g_img_len);
    CHECK("hype's qcow2 layer opens it", hype_qcow2_init(&q, &be, &raw_be, 0) == 0);
    CHECK("virtual size correct", be.total_sectors == (4ull << 20) / 512u);

    /* zeros everywhere -- head, middle, tail */
    CHECK("read head", hype_blk_backend_read(&be, 0u, 1u, sec) == 0);
    CHECK("head zero", sec[0] == 0 && sec[511] == 0);
    CHECK("read middle", hype_blk_backend_read(&be, be.total_sectors / 2u, 1u, sec) == 0);
    CHECK("middle zero", sec[0] == 0);
    CHECK("read tail", hype_blk_backend_read(&be, be.total_sectors - 1u, 1u, sec) == 0);
    CHECK("tail zero", sec[0] == 0);

    /* a write round-trips -- and needs NO allocation (preallocated is the point) */
    for (i = 0; i < 512u; i++) pat[i] = (uint8_t)(0xA7u ^ i);
    CHECK("write", hype_blk_backend_write(&be, be.total_sectors - 1u, 1u, pat) == 0);
    CHECK("readback", hype_blk_backend_read(&be, be.total_sectors - 1u, 1u, sec) == 0);
    CHECK("bytes", memcmp(sec, pat, 512) == 0);
}

static void test_guards(void) {
    static uint8_t b[CS];
    hype_qcow2_layout_t lo;
    CHECK("layout", hype_qcow2_layout(CS, &lo) == 0);
    CHECK("null layout refused", hype_qcow2_create_cluster(0, 0, b) != 0);
    CHECK("null buf refused", hype_qcow2_create_cluster(&lo, 0, 0) != 0);
    CHECK("unaligned size rounds, never truncates",
          hype_qcow2_layout(CS + 1u, &lo) == 0 && lo.virtual_bytes == 2u * (uint64_t)CS);
}

static void test_determinism(void) {
    static uint8_t a[CS], b[CS];
    hype_qcow2_layout_t lo;
    CHECK("layout", hype_qcow2_layout(64ull << 20, &lo) == 0);
    CHECK("render 0a", hype_qcow2_create_cluster(&lo, 0, a) == 0);
    CHECK("render 0b", hype_qcow2_create_cluster(&lo, 0, b) == 0);
    CHECK("deterministic", memcmp(a, b, CS) == 0);
}

int main(void) {
    test_layout_shapes();
    test_created_image_via_own_reader();
    test_guards();
    test_determinism();
    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
