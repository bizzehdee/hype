#include <stdio.h>
#include "../vm_lifecycle.h"

static int failures = 0;
#define CHECK(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)
#define EQ(desc, a, b) CHECK(desc, (a) == (b))

int main(void) {
    /* --- Stop / Resume (M8-5) --- */
    EQ("running+stop -> paused", HYPE_VM_PAUSED, hype_vm_lifecycle_next(HYPE_VM_RUNNING, HYPE_VM_EV_STOP));
    EQ("paused+resume -> running", HYPE_VM_RUNNING, hype_vm_lifecycle_next(HYPE_VM_PAUSED, HYPE_VM_EV_RESUME));
    EQ("paused+stop stays paused (idempotent)", HYPE_VM_PAUSED, hype_vm_lifecycle_next(HYPE_VM_PAUSED, HYPE_VM_EV_STOP));
    EQ("running+resume no-op", HYPE_VM_RUNNING, hype_vm_lifecycle_next(HYPE_VM_RUNNING, HYPE_VM_EV_RESUME));

    /* --- Shutdown (M8-6): orderly, guest reaches S5, or times out --- */
    EQ("running+shutdown -> shutting", HYPE_VM_SHUTTING, hype_vm_lifecycle_next(HYPE_VM_RUNNING, HYPE_VM_EV_SHUTDOWN));
    EQ("shutting+S5 -> off", HYPE_VM_OFF, hype_vm_lifecycle_next(HYPE_VM_SHUTTING, HYPE_VM_EV_S5));
    EQ("shutting+timeout -> off", HYPE_VM_OFF, hype_vm_lifecycle_next(HYPE_VM_SHUTTING, HYPE_VM_EV_TIMEOUT));
    EQ("paused+shutdown -> shutting", HYPE_VM_SHUTTING, hype_vm_lifecycle_next(HYPE_VM_PAUSED, HYPE_VM_EV_SHUTDOWN));
    EQ("shutting+shutdown stays shutting", HYPE_VM_SHUTTING, hype_vm_lifecycle_next(HYPE_VM_SHUTTING, HYPE_VM_EV_SHUTDOWN));

    /* --- guest self-poweroff from RUNNING --- */
    EQ("running+S5 -> off", HYPE_VM_OFF, hype_vm_lifecycle_next(HYPE_VM_RUNNING, HYPE_VM_EV_S5));

    /* --- Force power off (M8-7): unconditional from any live state --- */
    EQ("running+force -> off", HYPE_VM_OFF, hype_vm_lifecycle_next(HYPE_VM_RUNNING, HYPE_VM_EV_FORCE_OFF));
    EQ("paused+force -> off", HYPE_VM_OFF, hype_vm_lifecycle_next(HYPE_VM_PAUSED, HYPE_VM_EV_FORCE_OFF));
    EQ("shutting+force -> off", HYPE_VM_OFF, hype_vm_lifecycle_next(HYPE_VM_SHUTTING, HYPE_VM_EV_FORCE_OFF));
    EQ("starting+force -> off", HYPE_VM_OFF, hype_vm_lifecycle_next(HYPE_VM_STARTING, HYPE_VM_EV_FORCE_OFF));
    EQ("off+force stays off", HYPE_VM_OFF, hype_vm_lifecycle_next(HYPE_VM_OFF, HYPE_VM_EV_FORCE_OFF));

    /* --- Start (M8-4): OFF -> STARTING -> (re-init) -> RUNNING --- */
    EQ("off+start -> starting", HYPE_VM_STARTING, hype_vm_lifecycle_next(HYPE_VM_OFF, HYPE_VM_EV_START));
    EQ("starting+started -> running", HYPE_VM_RUNNING, hype_vm_lifecycle_next(HYPE_VM_STARTING, HYPE_VM_EV_STARTED));
    EQ("running+start no-op", HYPE_VM_RUNNING, hype_vm_lifecycle_next(HYPE_VM_RUNNING, HYPE_VM_EV_START));
    EQ("off+resume no-op", HYPE_VM_OFF, hype_vm_lifecycle_next(HYPE_VM_OFF, HYPE_VM_EV_RESUME));
    EQ("off+S5 no-op", HYPE_VM_OFF, hype_vm_lifecycle_next(HYPE_VM_OFF, HYPE_VM_EV_S5));

    /* --- runs() predicate --- */
    CHECK("running runs", hype_vm_lifecycle_runs(HYPE_VM_RUNNING));
    CHECK("shutting runs (must reach S5)", hype_vm_lifecycle_runs(HYPE_VM_SHUTTING));
    CHECK("paused does not run", !hype_vm_lifecycle_runs(HYPE_VM_PAUSED));
    CHECK("off does not run", !hype_vm_lifecycle_runs(HYPE_VM_OFF));
    CHECK("starting does not run", !hype_vm_lifecycle_runs(HYPE_VM_STARTING));

    /* --- names --- */
    CHECK("name running", hype_vm_lifecycle_name(HYPE_VM_RUNNING)[0] == 'r');
    CHECK("name paused", hype_vm_lifecycle_name(HYPE_VM_PAUSED)[0] == 'p');
    CHECK("name off", hype_vm_lifecycle_name(HYPE_VM_OFF)[0] == 'o');
    CHECK("name shutting", hype_vm_lifecycle_name(HYPE_VM_SHUTTING)[0] == 's');
    CHECK("name starting", hype_vm_lifecycle_name(HYPE_VM_STARTING)[0] == 's');
    CHECK("name default", hype_vm_lifecycle_name((hype_vm_lifecycle_t)99)[0] == '?');

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
