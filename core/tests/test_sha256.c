#include <stdio.h>
#include <string.h>
#include "../sha256.h"

static int failures = 0;
#define CHECK(msg, cond) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; } } while (0)

static void hex(const uint8_t *d, char *out) {
    static const char *h = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) { out[i * 2] = h[d[i] >> 4]; out[i * 2 + 1] = h[d[i] & 15]; }
    out[64] = 0;
}

static void vec(const char *msg, unsigned long len, const char *want, const char *name) {
    uint8_t d[32]; char got[65];
    hype_sha256(msg, len, d);
    hex(d, got);
    if (strcmp(got, want) != 0) { printf("FAIL: %s: got %s\n", name, got); failures++; }
}

int main(void) {
    /* FIPS 180-4 / NIST CAVS vectors */
    vec("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty");
    vec("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
    vec("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "448-bit");
    {
        static char m[1000000];
        memset(m, 'a', sizeof(m));
        vec(m, sizeof(m), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "1M a");
    }
    /* streaming == one-shot across odd split points */
    {
        uint8_t a[32], b[32];
        hype_sha256_t s;
        static uint8_t m[517];
        unsigned i;
        for (i = 0; i < sizeof(m); i++) m[i] = (uint8_t)(i * 7u);
        hype_sha256(m, sizeof(m), a);
        hype_sha256_init(&s);
        hype_sha256_update(&s, m, 63);
        hype_sha256_update(&s, m + 63, 1);
        hype_sha256_update(&s, m + 64, 130);
        hype_sha256_update(&s, m + 194, sizeof(m) - 194);
        hype_sha256_final(&s, b);
        CHECK("streaming matches one-shot", memcmp(a, b, 32) == 0);
    }
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
