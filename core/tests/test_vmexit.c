#include <stdio.h>
#include "../../arch/x86_64/cpu/vmexit.h"
#include "../../arch/x86_64/svm/vmcb.h"
#include "../../arch/x86_64/vmx/vmcs_fields.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), \
                   (long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_classify_svm(void) {
    CHECK_INT("SVM HLT classified as HLT", HYPE_VMEXIT_REASON_HLT,
              hype_vmexit_classify(HYPE_VMM_KIND_SVM, HYPE_SVM_EXITCODE_HLT));
    CHECK_INT("SVM shutdown classified as SHUTDOWN", HYPE_VMEXIT_REASON_SHUTDOWN,
              hype_vmexit_classify(HYPE_VMM_KIND_SVM, HYPE_SVM_EXITCODE_SHUTDOWN));
    CHECK_INT("SVM invalid-VMCB sentinel classified as ENTRY_FAILED", HYPE_VMEXIT_REASON_ENTRY_FAILED,
              hype_vmexit_classify(HYPE_VMM_KIND_SVM, HYPE_SVM_EXITCODE_INVALID));
    CHECK_INT("unrecognized SVM exit code classified as OTHER", HYPE_VMEXIT_REASON_OTHER,
              hype_vmexit_classify(HYPE_VMM_KIND_SVM, 0x99ULL));
}

static void test_classify_vmx(void) {
    CHECK_INT("VMX HLT classified as HLT", HYPE_VMEXIT_REASON_HLT,
              hype_vmexit_classify(HYPE_VMM_KIND_VMX, HYPE_VMX_EXIT_REASON_HLT));
    CHECK_INT("VMX triple fault classified as SHUTDOWN", HYPE_VMEXIT_REASON_SHUTDOWN,
              hype_vmexit_classify(HYPE_VMM_KIND_VMX, HYPE_VMX_EXIT_REASON_TRIPLE_FAULT));
    CHECK_INT("unrecognized VMX exit reason classified as OTHER", HYPE_VMEXIT_REASON_OTHER,
              hype_vmexit_classify(HYPE_VMM_KIND_VMX, 0x63ULL));
}

static void test_classify_none(void) {
    CHECK_INT("HYPE_VMM_KIND_NONE always classifies as OTHER", HYPE_VMEXIT_REASON_OTHER,
              hype_vmexit_classify(HYPE_VMM_KIND_NONE, HYPE_SVM_EXITCODE_HLT));
}

static void test_decide(void) {
    CHECK_INT("HLT decides GUEST_HALTED", HYPE_VMEXIT_ACTION_GUEST_HALTED,
              hype_vmexit_decide(HYPE_VMEXIT_REASON_HLT));
    CHECK_INT("SHUTDOWN decides FATAL", HYPE_VMEXIT_ACTION_FATAL,
              hype_vmexit_decide(HYPE_VMEXIT_REASON_SHUTDOWN));
    CHECK_INT("OTHER decides FATAL", HYPE_VMEXIT_ACTION_FATAL,
              hype_vmexit_decide(HYPE_VMEXIT_REASON_OTHER));
    CHECK_INT("ENTRY_FAILED decides FATAL", HYPE_VMEXIT_ACTION_FATAL,
              hype_vmexit_decide(HYPE_VMEXIT_REASON_ENTRY_FAILED));
}

/* Mock backend for hype_vmexit_dispatch_loop -- a fake hype_vcpu_ctx_t
 * (opaque to callers; this test never dereferences it, only forwards
 * the pointer, same as the real dispatch loop does) and a
 * test-controlled vcpu_run(). */
struct fake_ctx {
    int marker;
};

static int g_mock_entry_fails;
static uint64_t g_mock_reason;

static int mock_vcpu_run(hype_vcpu_ctx_t *ctx, hype_vmexit_info_t *info) {
    (void)ctx;
    if (g_mock_entry_fails) {
        return -1;
    }
    info->reason = g_mock_reason;
    info->qualification = 0;
    info->guest_rip = 0;
    return 0;
}

static void test_dispatch_loop_guest_halted(void) {
    struct fake_ctx fake;
    hype_vmm_ops_t ops = {"mock", 0, 0, 0, mock_vcpu_run};
    hype_vmexit_info_t info;

    g_mock_entry_fails = 0;
    g_mock_reason = HYPE_SVM_EXITCODE_HLT;

    int rc = hype_vmexit_dispatch_loop(&ops, (hype_vcpu_ctx_t *)&fake, HYPE_VMM_KIND_SVM, &info);
    CHECK_INT("HLT exit returns 0 (guest halted cleanly)", 0, rc);
}

static void test_dispatch_loop_fatal_exit(void) {
    struct fake_ctx fake;
    hype_vmm_ops_t ops = {"mock", 0, 0, 0, mock_vcpu_run};
    hype_vmexit_info_t info;

    g_mock_entry_fails = 0;
    g_mock_reason = HYPE_SVM_EXITCODE_SHUTDOWN;

    int rc = hype_vmexit_dispatch_loop(&ops, (hype_vcpu_ctx_t *)&fake, HYPE_VMM_KIND_SVM, &info);
    CHECK_INT("shutdown exit returns non-zero (fatal)", 1, rc != 0);
}

static void test_dispatch_loop_entry_failure(void) {
    struct fake_ctx fake;
    hype_vmm_ops_t ops = {"mock", 0, 0, 0, mock_vcpu_run};
    hype_vmexit_info_t info;

    g_mock_entry_fails = 1;

    int rc = hype_vmexit_dispatch_loop(&ops, (hype_vcpu_ctx_t *)&fake, HYPE_VMM_KIND_SVM, &info);
    CHECK_INT("VM-entry failure returns non-zero without inspecting info", 1, rc != 0);
}

int main(void) {
    test_classify_svm();
    test_classify_vmx();
    test_classify_none();
    test_decide();
    test_dispatch_loop_guest_halted();
    test_dispatch_loop_fatal_exit();
    test_dispatch_loop_entry_failure();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
