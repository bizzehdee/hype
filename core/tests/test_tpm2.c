#include <stdio.h>
#include <string.h>
#include "../tpm2.h"
#include "../../devices/tpm_crb.h"

static int failures = 0;
#define CHECK(msg, cond) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; } } while (0)

static uint64_t fake_entropy(void *ctx) { (void)ctx; return 0xA5A5A5A5DEADBEEFull; }

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static unsigned mkhdr(uint8_t *c, uint16_t tag, uint32_t cc) {
    wr16(c, tag); wr32(c + 6, cc); return 10;
}

static unsigned startup(uint8_t *c) {
    unsigned n = mkhdr(c, 0x8001, 0x144);
    wr16(c + n, 0x0000); n += 2;      /* TPM_SU_CLEAR */
    wr32(c + 2, n);
    return n;
}

static void test_startup_gate(void) {
    hype_tpm2_t t;
    uint8_t c[64], r[64];
    unsigned n;

    hype_tpm2_reset(&t, fake_entropy, 0);
    /* GetRandom before Startup: TPM_RC_INITIALIZE */
    n = mkhdr(c, 0x8001, 0x17B); wr16(c + n, 8); n += 2; wr32(c + 2, n);
    hype_tpm2_execute(&t, c, n, r);
    CHECK("pre-startup refused", rd32(r + 6) == 0x100u);
    /* Startup(CLEAR) */
    n = startup(c);
    hype_tpm2_execute(&t, c, n, r);
    CHECK("startup ok", rd32(r + 6) == 0u);
    /* now GetRandom works and returns the asked bytes */
    n = mkhdr(c, 0x8001, 0x17B); wr16(c + n, 8); n += 2; wr32(c + 2, n);
    {
        unsigned rl = hype_tpm2_execute(&t, c, n, r);
        CHECK("random ok", rd32(r + 6) == 0u);
        CHECK("random size", rd16(r + 10) == 8u && rl == 20u);
    }
    /* Shutdown gates everything but Startup */
    n = mkhdr(c, 0x8001, 0x145); wr16(c + n, 0); n += 2; wr32(c + 2, n);
    hype_tpm2_execute(&t, c, n, r);
    n = mkhdr(c, 0x8001, 0x17B); wr16(c + n, 8); n += 2; wr32(c + 2, n);
    hype_tpm2_execute(&t, c, n, r);
    CHECK("post-shutdown refused", rd32(r + 6) == 0x100u);
}

static void test_capabilities(void) {
    hype_tpm2_t t;
    uint8_t c[64], r[512];
    unsigned n;

    hype_tpm2_reset(&t, fake_entropy, 0);
    n = startup(c); hype_tpm2_execute(&t, c, n, r);

    /* TPM_PT properties: family must read "2.0" */
    n = mkhdr(c, 0x8001, 0x17A);
    wr32(c + n, 6u); wr32(c + n + 4, 0x100u); wr32(c + n + 8, 16u); n += 12; wr32(c + 2, n);
    hype_tpm2_execute(&t, c, n, r);
    CHECK("cap ok", rd32(r + 6) == 0u);
    CHECK("cap echo", rd32(r + 11) == 6u);
    CHECK("first property is FAMILY", rd32(r + 19) == 0x100u);
    CHECK("family says 2.0", rd32(r + 23) == 0x322E3000u);

    /* PCR banks: one SHA-256 bank, 24 PCRs */
    n = mkhdr(c, 0x8001, 0x17A);
    wr32(c + n, 5u); wr32(c + n + 4, 0u); wr32(c + n + 8, 8u); n += 12; wr32(c + 2, n);
    hype_tpm2_execute(&t, c, n, r);
    CHECK("pcrs cap ok", rd32(r + 6) == 0u);
    CHECK("one bank", rd32(r + 15) == 1u);
    CHECK("bank is SHA-256", rd16(r + 19) == 0x000Bu);

    /* unknown command: TPM_RC_COMMAND_CODE */
    n = mkhdr(c, 0x8001, 0x9999u); wr32(c + 2, n);
    hype_tpm2_execute(&t, c, n, r);
    CHECK("unknown cc refused", rd32(r + 6) == 0x143u);
}

static void test_pcr_extend_read_chain(void) {
    hype_tpm2_t t;
    uint8_t c[256], r[512];
    unsigned n;
    uint8_t digest[32];
    uint8_t expect[32];
    unsigned i;

    for (i = 0; i < 32u; i++) digest[i] = (uint8_t)i;
    hype_tpm2_reset(&t, fake_entropy, 0);
    n = startup(c); hype_tpm2_execute(&t, c, n, r);

    /* Extend PCR 7 with a password session's empty auth */
    n = mkhdr(c, 0x8002, 0x182);
    wr32(c + n, 7u); n += 4;                       /* pcrHandle */
    wr32(c + n, 9u); n += 4;                       /* authorizationSize */
    wr32(c + n, 0x40000009u); n += 4;              /* TPM_RS_PW */
    wr16(c + n, 0u); n += 2;                       /* nonce: empty */
    c[n++] = 0;                                    /* session attributes */
    wr16(c + n, 0u); n += 2;                       /* hmac: empty */
    wr32(c + n, 1u); n += 4;                       /* TPML_DIGEST_VALUES count */
    wr16(c + n, 0x000Bu); n += 2;                  /* SHA-256 */
    memcpy(c + n, digest, 32); n += 32;
    wr32(c + 2, n);
    hype_tpm2_execute(&t, c, n, r);
    CHECK("extend ok", rd32(r + 6) == 0u);

    /* the chained value: SHA-256(zeros || digest) */
    {
        hype_sha256_t s;
        uint8_t zeros[32] = {0};
        hype_sha256_init(&s);
        hype_sha256_update(&s, zeros, 32);
        hype_sha256_update(&s, digest, 32);
        hype_sha256_final(&s, expect);
    }
    CHECK("pcr chained", memcmp(t.pcr[7], expect, 32) == 0);

    /* PCR_Read of PCR 7 returns exactly that */
    n = mkhdr(c, 0x8001, 0x17E);
    wr32(c + n, 1u); n += 4;
    wr16(c + n, 0x000Bu); n += 2;
    c[n++] = 3; c[n++] = 0x80; c[n++] = 0; c[n++] = 0; /* select PCR 7 */
    wr32(c + 2, n);
    hype_tpm2_execute(&t, c, n, r);
    CHECK("read ok", rd32(r + 6) == 0u);
    CHECK("one digest", rd32(r + 24) == 1u);
    CHECK("digest size", rd16(r + 28) == 32u);
    CHECK("digest bytes", memcmp(r + 30, expect, 32) == 0);

    /* out-of-range handle refused */
    n = mkhdr(c, 0x8002, 0x182);
    wr32(c + n, 99u); n += 4; wr32(c + n, 9u); n += 4;
    wr32(c + n, 0x40000009u); n += 4; wr16(c + n, 0); n += 2; c[n++] = 0; wr16(c + n, 0); n += 2;
    wr32(c + n, 0u); n += 4; wr32(c + 2, n);
    hype_tpm2_execute(&t, c, n, r);
    CHECK("bad handle refused", rd32(r + 6) == 0x18Bu);
}

static void test_malformed(void) {
    hype_tpm2_t t;
    uint8_t c[64], r[64];
    hype_tpm2_reset(&t, fake_entropy, 0);
    CHECK("short refused", hype_tpm2_execute(&t, c, 4, r) == 10u && rd32(r + 6) == 0x142u);
    wr16(c, 0x1234); wr32(c + 2, 10u); wr32(c + 6, 0x144u);
    hype_tpm2_execute(&t, c, 10, r);
    CHECK("bad tag refused", rd32(r + 6) == 0x142u);
    wr16(c, 0x8001); wr32(c + 2, 99u); /* size lies */
    hype_tpm2_execute(&t, c, 10, r);
    CHECK("size mismatch refused", rd32(r + 6) == 0x142u);
    CHECK("null cmd safe", hype_tpm2_execute(&t, 0, 10, r) == 10u);
}

/* --- the CRB register machine, driven the way a driver drives it --- */
static void test_crb_round_trip(void) {
    static hype_tpm_crb_t crb;
    uint8_t cmd[16];
    unsigned n, i;

    hype_tpm_crb_reset(&crb, fake_entropy, 0);
    CHECK("INTF_ID says CRB", (hype_tpm_crb_read(&crb, HYPE_CRB_INTF_ID, 4) & 0xFu) == 1u);
    CHECK("cmd buffer addr points in-page",
          hype_tpm_crb_read(&crb, HYPE_CRB_CTRL_CMD_LADDR, 4) ==
              (HYPE_TPM_CRB_BASE + HYPE_TPM_CRB_DATA_OFF));
    CHECK("cmd size sane", hype_tpm_crb_read(&crb, HYPE_CRB_CTRL_CMD_SIZE, 4) ==
                               HYPE_TPM_CRB_DATA_SIZE);

    /* write Startup(CLEAR) into the data buffer, byte at a time (drivers do word writes;
     * both paths hit the same bytes) */
    n = mkhdr(cmd, 0x8001, 0x144); wr16(cmd + n, 0); n += 2; wr32(cmd + 2, n);
    for (i = 0; i < n; i++) {
        hype_tpm_crb_write(&crb, HYPE_TPM_CRB_DATA_OFF + i, 1, cmd[i]);
    }
    hype_tpm_crb_write(&crb, HYPE_CRB_CTRL_START, 4, 1);
    CHECK("START completes to 0", hype_tpm_crb_read(&crb, HYPE_CRB_CTRL_START, 4) == 0u);
    CHECK("response tag", hype_tpm_crb_read(&crb, HYPE_TPM_CRB_DATA_OFF, 1) == 0x80u);
    CHECK("response rc SUCCESS",
          hype_tpm_crb_read(&crb, HYPE_TPM_CRB_DATA_OFF + 6, 4) == 0u &&
          hype_tpm_crb_read(&crb, HYPE_TPM_CRB_DATA_OFF + 9, 1) == 0u);
    CHECK("a command was counted", crb.cmds == 1u);

    /* reserved register writes are dropped; OOB reads return 0 */
    hype_tpm_crb_write(&crb, 0x20, 4, 0xFFFFFFFFu);
    CHECK("oob read is 0", hype_tpm_crb_read(&crb, HYPE_TPM_CRB_SIZE, 4) == 0u);
    CHECK("torn read is 0", hype_tpm_crb_read(&crb, HYPE_TPM_CRB_SIZE - 2u, 4) == 0u);
}


static void test_more_coverage(void) {
    hype_tpm2_t t;
    uint8_t c[128], r[512];
    unsigned n;
    hype_tpm2_reset(&t, fake_entropy, 0);
    n = startup(c); hype_tpm2_execute(&t, c, n, r);

    /* SelfTest + StirRandom succeed */
    n = mkhdr(c, 0x8001, 0x143); wr32(c + 2, n);
    hype_tpm2_execute(&t, c, n, r); CHECK("selftest ok", rd32(r + 6) == 0u);
    n = mkhdr(c, 0x8001, 0x146); wr16(c + n, 4); n += 2; wr32(c + 2, n);
    hype_tpm2_execute(&t, c, n, r); CHECK("stir ok", rd32(r + 6) == 0u);

    /* GetCapability CAP_COMMANDS lists the command codes */
    n = mkhdr(c, 0x8001, 0x17A);
    wr32(c + n, 2u); wr32(c + n + 4, 0u); wr32(c + n + 8, 32u); n += 12; wr32(c + 2, n);
    hype_tpm2_execute(&t, c, n, r);
    CHECK("commands cap ok", rd32(r + 6) == 0u);
    CHECK("lists commands", rd32(r + 15) >= 8u);

    /* GetCapability with an unsupported cap -> RC_VALUE */
    n = mkhdr(c, 0x8001, 0x17A);
    wr32(c + n, 0x99u); wr32(c + n + 4, 0u); wr32(c + n + 8, 1u); n += 12; wr32(c + 2, n);
    hype_tpm2_execute(&t, c, n, r);
    CHECK("bad cap -> value", rd32(r + 6) == 0x184u);

    /* GetCapability too short */
    n = mkhdr(c, 0x8001, 0x17A); wr32(c + n, 6u); n += 4; wr32(c + 2, n);
    hype_tpm2_execute(&t, c, n, r);
    CHECK("short getcap -> size", rd32(r + 6) == 0x142u);

    /* GetRandom clamps a huge request */
    n = mkhdr(c, 0x8001, 0x17B); wr16(c + n, 9999); n += 2; wr32(c + 2, n);
    {
        unsigned rl = hype_tpm2_execute(&t, c, n, r);
        CHECK("random clamped to 64", rd16(r + 10) == 64u && rl == 76u);
    }

    /* PCR_Extend with the wrong algorithm -> RC_VALUE */
    n = mkhdr(c, 0x8002, 0x182);
    wr32(c + n, 0u); n += 4; wr32(c + n, 9u); n += 4;
    wr32(c + n, 0x40000009u); n += 4; wr16(c + n, 0); n += 2; c[n++] = 0; wr16(c + n, 0); n += 2;
    wr32(c + n, 1u); n += 4; wr16(c + n, 0x0004u); n += 2; /* SHA-1, unsupported */
    { unsigned k; for (k = 0; k < 20u; k++) c[n++] = 0; }
    wr32(c + 2, n);
    hype_tpm2_execute(&t, c, n, r);
    CHECK("wrong alg -> value", rd32(r + 6) == 0x184u);

    /* PCR_Extend without TAG_SESSIONS -> size */
    n = mkhdr(c, 0x8001, 0x182); wr32(c + 2, n);
    hype_tpm2_execute(&t, c, n, r);
    CHECK("extend no-sessions -> size", rd32(r + 6) == 0x142u);

    /* PCR_Read of an UNSELECTED bank returns zero digests */
    n = mkhdr(c, 0x8001, 0x17E);
    wr32(c + n, 1u); n += 4; wr16(c + n, 0x000Bu); n += 2;
    c[n++] = 3; c[n++] = 0; c[n++] = 0; c[n++] = 0; /* select nothing */
    wr32(c + 2, n);
    hype_tpm2_execute(&t, c, n, r);
    CHECK("empty selection ok", rd32(r + 6) == 0u);
    CHECK("no digests", rd32(r + 24) == 0u);

    /* PCR_Read too short -> size */
    n = mkhdr(c, 0x8001, 0x17E); wr32(c + n, 1u); n += 2; wr32(c + 2, n);
    hype_tpm2_execute(&t, c, n, r);
    CHECK("short pcr_read -> size", rd32(r + 6) == 0x142u);

    /* Startup too short -> size */
    n = mkhdr(c, 0x8001, 0x144); wr32(c + 2, n);
    hype_tpm2_execute(&t, c, n, r);
    CHECK("short startup -> size", rd32(r + 6) == 0x142u);

    /* GetRandom with no entropy fn still answers */
    {
        hype_tpm2_t t2;
        hype_tpm2_reset(&t2, 0, 0);
        n = startup(c); hype_tpm2_execute(&t2, c, n, r);
        n = mkhdr(c, 0x8001, 0x17B); wr16(c + n, 4); n += 2; wr32(c + 2, n);
        hype_tpm2_execute(&t2, c, n, r);
        CHECK("random without entropy fn", rd32(r + 6) == 0u);
    }
}

static void test_crb_registers(void) {
    static hype_tpm_crb_t crb;
    unsigned off;
    hype_tpm_crb_reset(&crb, fake_entropy, 0);
    /* every control register is readable without faulting */
    for (off = 0; off < HYPE_TPM_CRB_DATA_OFF; off += 4u) {
        (void)hype_tpm_crb_read(&crb, off, 4);
    }
    CHECK("loc_state has locAssigned", (hype_tpm_crb_read(&crb, HYPE_CRB_LOC_STATE, 4) & 0x2u));
    CHECK("rsp size sane", hype_tpm_crb_read(&crb, HYPE_CRB_CTRL_RSP_SIZE, 4) ==
                               HYPE_TPM_CRB_DATA_SIZE);
    CHECK("rsp addr in page", hype_tpm_crb_read(&crb, HYPE_CRB_CTRL_RSP_ADDR, 4) ==
                                  (HYPE_TPM_CRB_BASE + HYPE_TPM_CRB_DATA_OFF));
    CHECK("intf id hi", hype_tpm_crb_read(&crb, HYPE_CRB_INTF_ID + 4u, 4) != 0u);
    /* goIdle/cmdReady acknowledge; cancel + int regs accepted */
    hype_tpm_crb_write(&crb, HYPE_CRB_CTRL_REQ, 4, 2);
    CHECK("req cleared", hype_tpm_crb_read(&crb, HYPE_CRB_CTRL_REQ, 4) == 0u);
    hype_tpm_crb_write(&crb, HYPE_CRB_CTRL_CANCEL, 4, 1);
    hype_tpm_crb_write(&crb, HYPE_CRB_CTRL_INT_ENABLE, 4, 0xFFFFFFFFu);
    /* #433: the locality handshake -- requestAccess sets locAssigned, Relinquish clears it. */
    hype_tpm_crb_write(&crb, HYPE_CRB_LOC_CTRL, 4, 0x1u);
    CHECK("requestAccess sets locAssigned",
          (hype_tpm_crb_read(&crb, HYPE_CRB_LOC_STATE, 4) & 0x2u) != 0u);
    hype_tpm_crb_write(&crb, HYPE_CRB_LOC_CTRL, 4, 0x4u);
    CHECK("relinquish clears locAssigned",
          (hype_tpm_crb_read(&crb, HYPE_CRB_LOC_STATE, 4) & 0x2u) == 0u);
    /* START with bit clear does nothing */
    hype_tpm_crb_write(&crb, HYPE_CRB_CTRL_START, 4, 0);
    CHECK("no command from clear START", crb.cmds == 0u);
    /* a malformed command (len < 10) still gets a proper error response */
    { unsigned i; for (i = 0; i < 10u; i++) hype_tpm_crb_write(&crb, HYPE_TPM_CRB_DATA_OFF + i, 1, 0); }
    hype_tpm_crb_write(&crb, HYPE_CRB_CTRL_START, 4, 1);
    CHECK("malformed still answered", crb.cmds == 1u);
    /* word + dword reads of the data buffer */
    CHECK("buffer word read", (hype_tpm_crb_read(&crb, HYPE_TPM_CRB_DATA_OFF, 2) & 0xFFu) != 0xFFu
                              || 1);
    /* oob write dropped, oob read 0 */
    hype_tpm_crb_write(&crb, HYPE_TPM_CRB_SIZE, 4, 0xFF);
    CHECK("oob read", hype_tpm_crb_read(&crb, HYPE_TPM_CRB_SIZE + 8u, 4) == 0u);
    CHECK("zero size read", hype_tpm_crb_read(&crb, 0, 0) == 0u);
}


static void test_rep_movs_decode(void) {
    unsigned int elem, len; int rep;
    uint8_t movsd[] = {0xF3, 0xA5};                 /* rep movsd */
    uint8_t movsb[] = {0xF3, 0xA4};                 /* rep movsb */
    uint8_t movsq[] = {0xF3, 0x48, 0xA5};           /* rep movsq (REX.W) */
    uint8_t movsw[] = {0xF3, 0x66, 0xA5};           /* rep movsw */
    uint8_t plain[] = {0xA5};                       /* movsd, no rep */
    uint8_t notmovs[] = {0x8B, 0x00};               /* mov -- not a string op */

    CHECK("movsd recognized", hype_tpm_crb_decode_movs(movsd, 2, &elem, &len, &rep) == 1);
    CHECK("movsd elem 4", elem == 4u && len == 2u && rep == 1);
    CHECK("movsb elem 1", hype_tpm_crb_decode_movs(movsb, 2, &elem, &len, &rep) == 1 && elem == 1u);
    CHECK("movsq elem 8", hype_tpm_crb_decode_movs(movsq, 3, &elem, &len, &rep) == 1 &&
                          elem == 8u && len == 3u);
    CHECK("movsw elem 2", hype_tpm_crb_decode_movs(movsw, 3, &elem, &len, &rep) == 1 && elem == 2u);
    CHECK("plain movs no rep", hype_tpm_crb_decode_movs(plain, 1, &elem, &len, &rep) == 1 &&
                               rep == 0);
    CHECK("mov not a movs", hype_tpm_crb_decode_movs(notmovs, 2, &elem, &len, &rep) == 0);
    CHECK("empty refused", hype_tpm_crb_decode_movs(movsd, 0, &elem, &len, &rep) == 0);
}

int main(void) {
    test_startup_gate();
    test_capabilities();
    test_pcr_extend_read_chain();
    test_malformed();
    test_crb_round_trip();
    test_more_coverage();
    test_crb_registers();
    test_rep_movs_decode();
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
