#include <stdio.h>
#include "../../arch/x86_64/svm/svm.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_efer_with_svme(void) {
    /* A realistic EFER for a running 64-bit kernel: SCE|LME|LMA, no
     * SVME yet. */
    uint64_t efer = 0x00000d01ULL;
    uint64_t result = hype_svm_efer_with_svme(efer);

    CHECK_HEX("SVME bit gets set", HYPE_EFER_SVME, result & HYPE_EFER_SVME);
    CHECK_HEX("existing bits (SCE|LME|LMA) are preserved", efer, result & ~HYPE_EFER_SVME);
}

static void test_efer_with_svme_idempotent(void) {
    uint64_t efer = HYPE_EFER_SVME | 0x500ULL;
    uint64_t result = hype_svm_efer_with_svme(efer);
    CHECK_HEX("already-set SVME stays set, nothing else changes", efer, result);
}

int main(void) {
    test_efer_with_svme();
    test_efer_with_svme_idempotent();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
