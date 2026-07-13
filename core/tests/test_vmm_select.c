#include <stdio.h>
#include "../../arch/x86_64/cpu/vmm_select.h"
#include "../../arch/x86_64/svm/svm.h"
#include "../../arch/x86_64/vmx/vmx.h"

static int failures = 0;

static void test_select_vmx(void) {
    const hype_vmm_ops_t *ops = hype_vmm_ops_for_kind(HYPE_VMM_KIND_VMX);
    if (ops != &hype_vmx_ops) {
        printf("FAIL: HYPE_VMM_KIND_VMX did not select &hype_vmx_ops\n");
        failures++;
    }
}

static void test_select_svm(void) {
    const hype_vmm_ops_t *ops = hype_vmm_ops_for_kind(HYPE_VMM_KIND_SVM);
    if (ops != &hype_svm_ops) {
        printf("FAIL: HYPE_VMM_KIND_SVM did not select &hype_svm_ops\n");
        failures++;
    }
}

static void test_select_none(void) {
    const hype_vmm_ops_t *ops = hype_vmm_ops_for_kind(HYPE_VMM_KIND_NONE);
    if (ops != 0) {
        printf("FAIL: HYPE_VMM_KIND_NONE should select NULL\n");
        failures++;
    }
}

static void test_select_invalid_kind(void) {
    /* Any value outside the enum's defined range falls through to the
     * same "default" path as NONE, distinctly from the explicit
     * HYPE_VMM_KIND_NONE case above. */
    const hype_vmm_ops_t *ops = hype_vmm_ops_for_kind((hype_vmm_kind_t)99);
    if (ops != 0) {
        printf("FAIL: an out-of-range kind should select NULL\n");
        failures++;
    }
}

int main(void) {
    test_select_vmx();
    test_select_svm();
    test_select_none();
    test_select_invalid_kind();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
