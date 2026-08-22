#include <stdio.h>
#include "../fs_owner_guard.h"

static int failures;

#define CHECK(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)

static void test_unbound_permits_any_core(void) {
    hype_fs_owner_guard_t g;
    hype_fs_owner_guard_init(&g);
    CHECK("unbound guard permits core 0", hype_fs_owner_guard_check(&g, 0u));
    CHECK("unbound guard permits core 7", hype_fs_owner_guard_check(&g, 7u));
}

static void test_bound_permits_only_owner(void) {
    hype_fs_owner_guard_t g;
    hype_fs_owner_guard_init(&g);
    hype_fs_owner_guard_bind(&g, 3u);
    CHECK("bound guard permits the bound owner", hype_fs_owner_guard_check(&g, 3u));
    CHECK("bound guard refuses a different core", !hype_fs_owner_guard_check(&g, 4u));
    CHECK("bound guard refuses core 0 too, if that isn't the owner", !hype_fs_owner_guard_check(&g, 0u));
}

static void test_rebind_changes_owner(void) {
    hype_fs_owner_guard_t g;
    hype_fs_owner_guard_init(&g);
    hype_fs_owner_guard_bind(&g, 1u);
    CHECK("owner 1 permitted before rebind", hype_fs_owner_guard_check(&g, 1u));
    hype_fs_owner_guard_bind(&g, 2u);
    CHECK("owner 1 refused after rebind to 2", !hype_fs_owner_guard_check(&g, 1u));
    CHECK("owner 2 permitted after rebind", hype_fs_owner_guard_check(&g, 2u));
}

static void test_null_guards(void) {
    /* init/bind on NULL must not crash; check on NULL always refuses -- a
     * missing guard is far more likely a wiring bug than a legitimate
     * always-open path (see the header's contract). */
    hype_fs_owner_guard_init(0);
    hype_fs_owner_guard_bind(0, 5u);
    CHECK("check on a NULL guard refuses", !hype_fs_owner_guard_check(0, 5u));
}

int main(void) {
    test_unbound_permits_any_core();
    test_bound_permits_only_owner();
    test_rebind_changes_owner();
    test_null_guards();

    if (failures) {
        printf("%d test(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
