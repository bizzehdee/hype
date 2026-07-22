#include "vm_lifecycle.h"

hype_vm_lifecycle_t hype_vm_lifecycle_next(hype_vm_lifecycle_t state, hype_vm_event_t ev) {
    /* FORCE_OFF is unconditional from any live state -- an operator kill always
     * wins (M8-7). It's a no-op only if already OFF. */
    if (ev == HYPE_VM_EV_FORCE_OFF) {
        return HYPE_VM_OFF;
    }
    switch (state) {
        case HYPE_VM_RUNNING:
            if (ev == HYPE_VM_EV_STOP)     return HYPE_VM_PAUSED;
            if (ev == HYPE_VM_EV_SHUTDOWN) return HYPE_VM_SHUTTING;
            if (ev == HYPE_VM_EV_S5)       return HYPE_VM_OFF; /* guest self-poweroff */
            return state;
        case HYPE_VM_PAUSED:
            if (ev == HYPE_VM_EV_RESUME)   return HYPE_VM_RUNNING;
            if (ev == HYPE_VM_EV_SHUTDOWN) return HYPE_VM_SHUTTING;
            return state;
        case HYPE_VM_SHUTTING:
            if (ev == HYPE_VM_EV_S5)       return HYPE_VM_OFF;
            if (ev == HYPE_VM_EV_TIMEOUT)  return HYPE_VM_OFF; /* escalate to off */
            return state;
        case HYPE_VM_OFF:
            if (ev == HYPE_VM_EV_START)    return HYPE_VM_STARTING;
            return state;
        case HYPE_VM_STARTING:
            if (ev == HYPE_VM_EV_STARTED)  return HYPE_VM_RUNNING;
            return state;
        default:
            return state;
    }
}

int hype_vm_lifecycle_runs(hype_vm_lifecycle_t state) {
    return state == HYPE_VM_RUNNING || state == HYPE_VM_SHUTTING;
}

const char *hype_vm_lifecycle_name(hype_vm_lifecycle_t state) {
    switch (state) {
        case HYPE_VM_RUNNING:  return "running";
        case HYPE_VM_PAUSED:   return "paused";
        case HYPE_VM_SHUTTING: return "stopping";
        case HYPE_VM_OFF:      return "off";
        case HYPE_VM_STARTING: return "starting";
        default:               return "?";
    }
}
