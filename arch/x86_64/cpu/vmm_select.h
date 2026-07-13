#ifndef HYPE_ARCH_VMM_SELECT_H
#define HYPE_ARCH_VMM_SELECT_H

#include "cpu_features.h"
#include "vmm_ops.h"

/* Returns the vtable for the given kind, or 0 (NULL) for
 * HYPE_VMM_KIND_NONE. Pure lookup -- fully testable even though the
 * vmx_ops/svm_ops objects it returns pointers to contain
 * hardware-touching function pointers, since this function itself
 * never calls through them. */
const hype_vmm_ops_t *hype_vmm_ops_for_kind(hype_vmm_kind_t kind);

#endif /* HYPE_ARCH_VMM_SELECT_H */
