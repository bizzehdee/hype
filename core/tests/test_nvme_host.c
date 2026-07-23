#include <stdio.h>
#include "../nvme_host.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static void test_read_sqe(void) {
    uint8_t sqe[64];
    hype_nvme_build_read_sqe(sqe, /*cid=*/0x1234, /*nsid=*/1, /*slba=*/0x00A0B0C0D0ull,
                             /*nlb_0based=*/7, /*prp1=*/0xCAFE000ull, /*prp2=*/0xBEEF000ull);
    CHECK_HEX("read opcode 0x02", 0x02u, sqe[0]);
    CHECK_HEX("read cid", 0x1234u, rd16(sqe + 2));
    CHECK_HEX("read nsid", 1u, rd32(sqe + 4));
    CHECK_HEX("read prp1", 0xCAFE000ull, rd64(sqe + 24));
    CHECK_HEX("read prp2", 0xBEEF000ull, rd64(sqe + 32));
    CHECK_HEX("read slba", 0x00A0B0C0D0ull, rd64(sqe + 40));
    CHECK_HEX("read nlb (0-based, 8 blocks)", 7u, rd16(sqe + 48));
}

static void test_write_sqe(void) {
    uint8_t sqe[64];
    hype_nvme_build_write_sqe(sqe, /*cid=*/0x2345, /*nsid=*/1, /*slba=*/0x00B0C0D0E0ull,
                              /*nlb_0based=*/15, /*prp1=*/0xDEAD000ull, /*prp2=*/0xF00D000ull);
    CHECK_HEX("write opcode 0x01", 0x01u, sqe[0]);
    CHECK_HEX("write cid", 0x2345u, rd16(sqe + 2));
    CHECK_HEX("write nsid", 1u, rd32(sqe + 4));
    CHECK_HEX("write prp1", 0xDEAD000ull, rd64(sqe + 24));
    CHECK_HEX("write prp2", 0xF00D000ull, rd64(sqe + 32));
    CHECK_HEX("write slba", 0x00B0C0D0E0ull, rd64(sqe + 40));
    CHECK_HEX("write nlb (0-based, 16 blocks)", 15u, rd16(sqe + 48));
}

static void test_identify_sqe(void) {
    uint8_t sqe[64];
    hype_nvme_build_identify_sqe(sqe, /*cid=*/0x9, /*cns=*/HYPE_NVME_CNS_NAMESPACE, /*nsid=*/1,
                                 /*prp1=*/0x40000ull);
    CHECK_HEX("identify opcode 0x06", 0x06u, sqe[0]);
    CHECK_HEX("identify cid", 0x9u, rd16(sqe + 2));
    CHECK_HEX("identify nsid", 1u, rd32(sqe + 4));
    CHECK_HEX("identify prp1", 0x40000ull, rd64(sqe + 24));
    CHECK_HEX("identify cns (CDW10)", HYPE_NVME_CNS_NAMESPACE, rd32(sqe + 40));
}

static void test_cqe_decode(void) {
    uint8_t cqe[16] = {0};
    /* success, phase 1, cid 0x1234 */
    cqe[12] = 0x34; cqe[13] = 0x12;
    cqe[14] = 0x01; cqe[15] = 0x00; /* status word = 0x0001: phase 1, status field 0 */
    CHECK_HEX("cqe cid", 0x1234u, hype_nvme_cqe_cid(cqe));
    CHECK_HEX("cqe phase 1", 1, hype_nvme_cqe_phase(cqe));
    CHECK_HEX("cqe success", 1, hype_nvme_cqe_success(cqe));
    /* error: status field nonzero (SC=0x02), phase 0 */
    cqe[14] = 0x04; cqe[15] = 0x00; /* 0x0004 => phase 0, status field = 0x02 */
    CHECK_HEX("cqe phase 0", 0, hype_nvme_cqe_phase(cqe));
    CHECK_HEX("cqe failure", 0, hype_nvme_cqe_success(cqe));
}

static void test_parse_identify_ns(void) {
    uint8_t id[4096] = {0};
    uint64_t blocks = 0;
    uint32_t bs = 0;
    /* NSZE = 0x100000 blocks; FLBAS selects format 1; LBAF1 LBADS = 9 (512 B). */
    id[0] = 0x00; id[1] = 0x00; id[2] = 0x10; /* 0x00100000 */
    id[26] = 0x01;                            /* FLBAS -> format index 1 */
    /* LBAF0 (index 0) LBADS=12 (4 KiB), LBAF1 (index 1) LBADS=9 (512 B). */
    id[128 + 0 * 4 + 2] = 12;                 /* LBAF0 byte2 = LBADS bits 23:16 */
    id[128 + 1 * 4 + 2] = 9;                  /* LBAF1 byte2 = LBADS = 9 */
    CHECK_HEX("parse ns ok", 0, hype_nvme_parse_identify_ns(id, &blocks, &bs));
    CHECK_HEX("ns total blocks", 0x100000ull, blocks);
    CHECK_HEX("ns block size 512", 512u, bs);

    /* NSZE 0 rejected. */
    id[2] = 0x00;
    CHECK_HEX("ns nsze 0 rejected", (unsigned long long)(-1),
              (unsigned long long)hype_nvme_parse_identify_ns(id, &blocks, &bs));
    /* implausible LBADS rejected (too large and too small). */
    id[2] = 0x10; id[128 + 1 * 4 + 2] = 20;
    CHECK_HEX("ns huge LBADS rejected", (unsigned long long)(-1),
              (unsigned long long)hype_nvme_parse_identify_ns(id, &blocks, &bs));
    id[128 + 1 * 4 + 2] = 5; /* < 9 (sub-512-byte) */
    CHECK_HEX("ns tiny LBADS rejected", (unsigned long long)(-1),
              (unsigned long long)hype_nvme_parse_identify_ns(id, &blocks, &bs));
}

static void test_cap_and_doorbell(void) {
    /* CAP with DSTRD (bits 35:32) = 2 => stride 4 << 2 = 16 bytes. */
    uint64_t cap = ((uint64_t)2u << 32);
    CHECK_HEX("dstrd", 2u, hype_nvme_cap_dstrd(cap));
    /* SQ0 tail doorbell: qid 0, is_cq 0 => base + 0. */
    CHECK_HEX("sq0 tail doorbell", 0x1000u, hype_nvme_doorbell_offset(0, 0, 2));
    /* CQ0 head doorbell: 2*0+1 = 1 stride. */
    CHECK_HEX("cq0 head doorbell", 0x1000u + 16u, hype_nvme_doorbell_offset(0, 1, 2));
    /* SQ1 tail doorbell: 2*1+0 = 2 strides. */
    CHECK_HEX("sq1 tail doorbell", 0x1000u + 32u, hype_nvme_doorbell_offset(1, 0, 2));
    /* stride 0 (DSTRD 0) => 4-byte doorbells. */
    CHECK_HEX("cq0 head, dstrd 0", 0x1000u + 4u, hype_nvme_doorbell_offset(0, 1, 0));
}

int main(void) {
    test_read_sqe();
    test_write_sqe();
    test_identify_sqe();
    test_cqe_decode();
    test_parse_identify_ns();
    test_cap_and_doorbell();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
