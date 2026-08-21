#include "tpm2.h"

/* TPM 2.0 Part 2 constants -- the subset this processor speaks. */
#define TAG_NO_SESSIONS 0x8001u
#define TAG_SESSIONS 0x8002u

#define CC_Startup 0x00000144u
#define CC_Shutdown 0x00000145u
#define CC_SelfTest 0x00000143u
#define CC_GetCapability 0x0000017Au
#define CC_GetRandom 0x0000017Bu
#define CC_StirRandom 0x00000146u
#define CC_PCR_Read 0x0000017Eu
#define CC_PCR_Extend 0x00000182u

#define RC_SUCCESS 0x000u
#define RC_INITIALIZE 0x100u
#define RC_FAILURE 0x101u
#define RC_COMMAND_CODE 0x143u
#define RC_COMMAND_SIZE 0x142u
#define RC_VALUE 0x184u
#define RC_HANDLE 0x18Bu

#define ALG_SHA256 0x000Bu

#define CAP_TPM_PROPERTIES 0x00000006u
#define CAP_PCRS 0x00000005u
#define CAP_COMMANDS 0x00000002u

#define PT_FAMILY_INDICATOR 0x100u
#define PT_LEVEL 0x101u
#define PT_REVISION 0x102u
#define PT_MANUFACTURER 0x105u
#define PT_VENDOR_STRING_1 0x106u
#define PT_PCR_COUNT 0x112u
#define PT_MAX_COMMAND_SIZE 0x11Eu
#define PT_MAX_RESPONSE_SIZE 0x11Fu
#define PT_TOTAL_COMMANDS 0x129u

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

void hype_tpm2_reset(hype_tpm2_t *t, uint64_t (*entropy)(void *ctx), void *entropy_ctx) {
    unsigned i, j;
    for (i = 0; i < HYPE_TPM2_PCR_COUNT; i++) {
        for (j = 0; j < HYPE_SHA256_DIGEST_SIZE; j++) {
            t->pcr[i][j] = 0;
        }
    }
    t->started = 0;
    t->shutdown = 0;
    t->selftested = 0;
    t->entropy = entropy;
    t->entropy_ctx = entropy_ctx;
}

/* A bare header-only response. */
static unsigned int rsp_error(uint8_t *rsp, uint32_t rc) {
    wr16(rsp, TAG_NO_SESSIONS);
    wr32(rsp + 2, 10u);
    wr32(rsp + 6, rc);
    return 10u;
}

/* The commands this TPM implements, in ascending order (GetCapability contract). */
static const uint32_t g_commands[] = {CC_SelfTest, CC_Startup, CC_Shutdown, CC_StirRandom,
                                      CC_GetCapability, CC_GetRandom, CC_PCR_Read, CC_PCR_Extend};
#define N_COMMANDS (sizeof(g_commands) / sizeof(g_commands[0]))

static unsigned int do_get_capability(hype_tpm2_t *t, const uint8_t *p, unsigned int plen,
                                      uint8_t *rsp) {
    uint32_t cap, prop, count;
    unsigned int n = 0;
    uint8_t *b;

    (void)t;
    if (plen < 12u) return rsp_error(rsp, RC_COMMAND_SIZE);
    cap = rd32(p);
    prop = rd32(p + 4);
    count = rd32(p + 8);
    if (count > 32u) count = 32u;

    /* header + moreData(1) + capability(4), body appended below */
    wr16(rsp, TAG_NO_SESSIONS);
    wr32(rsp + 6, RC_SUCCESS);
    rsp[10] = 0; /* moreData = NO */
    wr32(rsp + 11, cap);
    b = rsp + 15;

    if (cap == CAP_TPM_PROPERTIES) {
        /* every property this TPM states, ascending from `prop` */
        struct { uint32_t id, val; } props[] = {
            {PT_FAMILY_INDICATOR, 0x322E3000u}, /* "2.0" */
            {PT_LEVEL, 0u},
            {PT_REVISION, 164u},               /* spec 1.64-style revision field */
            {PT_MANUFACTURER, 0x48595045u},    /* "HYPE" */
            {PT_VENDOR_STRING_1, 0x68797065u}, /* "hype" */
            {PT_PCR_COUNT, HYPE_TPM2_PCR_COUNT},
            {PT_MAX_COMMAND_SIZE, HYPE_TPM2_MAX_CMD},
            {PT_MAX_RESPONSE_SIZE, HYPE_TPM2_MAX_RSP},
            {PT_TOTAL_COMMANDS, (uint32_t)N_COMMANDS},
        };
        unsigned i;
        uint8_t *cnt = b;
        b += 4;
        for (i = 0; i < sizeof(props) / sizeof(props[0]) && n < count; i++) {
            if (props[i].id < prop) continue;
            wr32(b, props[i].id);
            wr32(b + 4, props[i].val);
            b += 8;
            n++;
        }
        wr32(cnt, n);
    } else if (cap == CAP_PCRS) {
        /* one bank: SHA-256, all 24 PCRs selected */
        wr32(b, 1u); /* count of TPMS_PCR_SELECTION */
        wr16(b + 4, ALG_SHA256);
        b[6] = 3; /* sizeofSelect */
        b[7] = 0xFF; b[8] = 0xFF; b[9] = 0xFF;
        b += 10;
    } else if (cap == CAP_COMMANDS) {
        unsigned i;
        uint8_t *cnt = b;
        b += 4;
        for (i = 0; i < N_COMMANDS && n < count; i++) {
            if (g_commands[i] < prop) continue;
            /* TPMA_CC: the command code in the low bits, no attributes this TPM needs to state */
            wr32(b, g_commands[i]);
            b += 4;
            n++;
        }
        wr32(cnt, n);
    } else {
        return rsp_error(rsp, RC_VALUE);
    }
    wr32(rsp + 2, (uint32_t)(b - rsp));
    return (unsigned int)(b - rsp);
}

static unsigned int do_pcr_read(hype_tpm2_t *t, const uint8_t *p, unsigned int plen,
                                uint8_t *rsp) {
    uint32_t nsel, i;
    uint8_t sel[3] = {0, 0, 0};
    uint8_t *b;
    unsigned int npcr = 0, pi;

    if (plen < 4u) return rsp_error(rsp, RC_COMMAND_SIZE);
    nsel = rd32(p);
    p += 4; plen -= 4;
    /* take the FIRST SHA-256 selection; ignore (and accept) others -- a reader asking for a bank
     * this TPM does not have gets an empty digest list for it, per the spec's select semantics */
    for (i = 0; i < nsel; i++) {
        uint16_t alg;
        uint8_t sz;
        if (plen < 3u) return rsp_error(rsp, RC_COMMAND_SIZE);
        alg = rd16(p);
        sz = p[2];
        if (plen < 3u + sz) return rsp_error(rsp, RC_COMMAND_SIZE);
        if (alg == ALG_SHA256 && sz >= 1u) {
            unsigned k;
            for (k = 0; k < 3u && k < sz; k++) sel[k] = p[3 + k];
        }
        p += 3u + sz; plen -= 3u + sz;
    }

    wr16(rsp, TAG_NO_SESSIONS);
    wr32(rsp + 6, RC_SUCCESS);
    b = rsp + 10;
    wr32(b, 1u); /* pcrUpdateCounter (monotonic enough at 1 for a read-only view) */
    b += 4;
    /* pcrSelectionOut: echo the SHA-256 selection back */
    wr32(b, 1u);
    wr16(b + 4, ALG_SHA256);
    b[6] = 3; b[7] = sel[0]; b[8] = sel[1]; b[9] = sel[2];
    b += 10;
    {
        uint8_t *cnt = b;
        b += 4;
        for (pi = 0; pi < HYPE_TPM2_PCR_COUNT && npcr < 8u; pi++) {
            if (!(sel[pi / 8u] & (1u << (pi % 8u)))) continue;
            wr16(b, HYPE_SHA256_DIGEST_SIZE);
            for (i = 0; i < HYPE_SHA256_DIGEST_SIZE; i++) b[2 + i] = t->pcr[pi][i];
            b += 2u + HYPE_SHA256_DIGEST_SIZE;
            npcr++;
        }
        wr32(cnt, npcr);
    }
    wr32(rsp + 2, (uint32_t)(b - rsp));
    return (unsigned int)(b - rsp);
}

static unsigned int do_pcr_extend(hype_tpm2_t *t, const uint8_t *hdr, unsigned int cmd_len,
                                  uint8_t *rsp) {
    /* TAG_SESSIONS: handle(4) + authSize(4) + auth area + TPML_DIGEST_VALUES */
    const uint8_t *p = hdr + 10;
    unsigned int left = cmd_len - 10u;
    uint32_t handle, auth_size, ndig, i;

    if (left < 8u) return rsp_error(rsp, RC_COMMAND_SIZE);
    handle = rd32(p);
    auth_size = rd32(p + 4);
    p += 8; left -= 8;
    if (handle >= HYPE_TPM2_PCR_COUNT) return rsp_error(rsp, RC_HANDLE);
    if (auth_size > left) return rsp_error(rsp, RC_COMMAND_SIZE);
    p += auth_size; left -= auth_size; /* the password session's contents are irrelevant here */
    if (left < 4u) return rsp_error(rsp, RC_COMMAND_SIZE);
    ndig = rd32(p);
    p += 4; left -= 4;
    for (i = 0; i < ndig; i++) {
        uint16_t alg;
        if (left < 2u) return rsp_error(rsp, RC_COMMAND_SIZE);
        alg = rd16(p);
        p += 2; left -= 2;
        if (alg != ALG_SHA256) return rsp_error(rsp, RC_VALUE);
        if (left < HYPE_SHA256_DIGEST_SIZE) return rsp_error(rsp, RC_COMMAND_SIZE);
        /* the extend: PCR = SHA-256(PCR || digest) */
        {
            hype_sha256_t s;
            uint8_t out[HYPE_SHA256_DIGEST_SIZE];
            unsigned k;
            hype_sha256_init(&s);
            hype_sha256_update(&s, t->pcr[handle], HYPE_SHA256_DIGEST_SIZE);
            hype_sha256_update(&s, p, HYPE_SHA256_DIGEST_SIZE);
            hype_sha256_final(&s, out);
            for (k = 0; k < HYPE_SHA256_DIGEST_SIZE; k++) t->pcr[handle][k] = out[k];
        }
        p += HYPE_SHA256_DIGEST_SIZE; left -= HYPE_SHA256_DIGEST_SIZE;
    }
    /* TAG_SESSIONS response: header + parameterSize(4) + (no params) + null auth response */
    wr16(rsp, TAG_SESSIONS);
    wr32(rsp + 2, 19u);
    wr32(rsp + 6, RC_SUCCESS);
    wr32(rsp + 10, 0u);  /* parameterSize */
    wr16(rsp + 14, 0u);  /* nonce (empty) */
    rsp[16] = 0;         /* session attributes: continueSession clear */
    wr16(rsp + 17, 0u);  /* hmac (empty) */
    return 19u;
}

static unsigned int do_get_random(hype_tpm2_t *t, const uint8_t *p, unsigned int plen,
                                  uint8_t *rsp) {
    uint16_t want;
    unsigned int i;
    if (plen < 2u) return rsp_error(rsp, RC_COMMAND_SIZE);
    want = rd16(p);
    if (want > 64u) want = 64u; /* the spec allows a short return; 64 covers every real caller */
    wr16(rsp, TAG_NO_SESSIONS);
    wr32(rsp + 2, 12u + want);
    wr32(rsp + 6, RC_SUCCESS);
    wr16(rsp + 10, want);
    for (i = 0; i < want; i++) {
        uint64_t e = t->entropy ? t->entropy(t->entropy_ctx) : 0x9E3779B97F4A7C15ull;
        rsp[12 + i] = (uint8_t)(e >> ((i % 8u) * 8u));
    }
    return 12u + want;
}

unsigned int hype_tpm2_execute(hype_tpm2_t *t, const uint8_t *cmd, unsigned int cmd_len,
                               uint8_t *rsp) {
    uint16_t tag;
    uint32_t size, cc;

    if (cmd == 0 || rsp == 0 || cmd_len < 10u) return rsp_error(rsp, RC_COMMAND_SIZE);
    tag = rd16(cmd);
    size = rd32(cmd + 2);
    cc = rd32(cmd + 6);
    if (size != cmd_len || size > HYPE_TPM2_MAX_CMD) return rsp_error(rsp, RC_COMMAND_SIZE);
    if (tag != TAG_NO_SESSIONS && tag != TAG_SESSIONS) return rsp_error(rsp, RC_COMMAND_SIZE);

    if (t->shutdown && cc != CC_Startup) return rsp_error(rsp, RC_INITIALIZE);
    if (!t->started && cc != CC_Startup && cc != CC_GetCapability && cc != CC_SelfTest) {
        return rsp_error(rsp, RC_INITIALIZE);
    }

    switch (cc) {
    case CC_Startup:
        if (cmd_len < 12u) return rsp_error(rsp, RC_COMMAND_SIZE);
        t->started = 1;
        t->shutdown = 0;
        return rsp_error(rsp, RC_SUCCESS); /* success is also just a header */
    case CC_Shutdown:
        t->shutdown = 1;
        return rsp_error(rsp, RC_SUCCESS);
    case CC_SelfTest:
        t->selftested = 1;
        return rsp_error(rsp, RC_SUCCESS);
    case CC_StirRandom:
        return rsp_error(rsp, RC_SUCCESS); /* entropy is host-supplied; stirring is a no-op */
    case CC_GetCapability:
        return do_get_capability(t, cmd + 10, cmd_len - 10u, rsp);
    case CC_GetRandom:
        return do_get_random(t, cmd + 10, cmd_len - 10u, rsp);
    case CC_PCR_Read:
        return do_pcr_read(t, cmd + 10, cmd_len - 10u, rsp);
    case CC_PCR_Extend:
        if (tag != TAG_SESSIONS) return rsp_error(rsp, RC_COMMAND_SIZE);
        return do_pcr_extend(t, cmd, cmd_len, rsp);
    default:
        return rsp_error(rsp, RC_COMMAND_CODE);
    }
}
