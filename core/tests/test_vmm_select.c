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

/*
 * #242 regression guard. The AP landing enables the VMM on its own core via
 * ops->enable_on; it used to call SVM's version directly, which on Intel wrote
 * EFER.SVME -- reserved there -- and #GP'd on every boot. A backend that leaves
 * enable_on NULL would silently bring an AP up with no VMM enabled and fault at
 * the first VMRUN/VMLAUNCH instead, so assert BOTH are wired, and that each
 * backend has its own (a copy-paste of the wrong one is the original bug back).
 */
static void test_both_backends_wire_percore_enable(void) {
    if (hype_svm_ops.enable_on == 0) {
        printf("FAIL: hype_svm_ops.enable_on is NULL -- an AP could not enable SVM\n");
        failures++;
    }
    if (hype_vmx_ops.enable_on == 0) {
        printf("FAIL: hype_vmx_ops.enable_on is NULL -- an AP could not enable VMX\n");
        failures++;
    }
    if (hype_svm_ops.enable_on == hype_vmx_ops.enable_on) {
        printf("FAIL: SVM and VMX share one enable_on -- one vendor is running the "
               "other's per-core enable (that IS #242)\n");
        failures++;
    }
    /* Same for the BSP's entry point, for the same reason. */
    if (hype_svm_ops.enable == hype_vmx_ops.enable) {
        printf("FAIL: SVM and VMX share one enable\n");
        failures++;
    }
}

int main(void) {
    test_select_vmx();
    test_select_svm();
    test_select_none();
    test_select_invalid_kind();
    test_both_backends_wire_percore_enable();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
