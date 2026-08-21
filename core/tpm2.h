#ifndef HYPE_CORE_TPM2_H
#define HYPE_CORE_TPM2_H

#include <stdint.h>
#include "sha256.h"

/*
 * #433: the guest TPM 2.0 command processor -- the pure half behind the CRB interface
 * (devices/tpm_crb.c). plan.md §10 decision 54: implemented in-tree rather than integrating
 * ms-tpm-20-ref/libtpms, whose crypto-library and allocation needs do not fit a freestanding
 * ring-0 module; the scope is what a Windows compatibility probe and a Linux tpm_crb driver
 * actually touch, and everything else answers TPM_RC_COMMAND_CODE honestly:
 *
 *   Startup / Shutdown / SelfTest         -- state gates
 *   GetCapability                          -- properties (family "2.0", manufacturer, PCR count),
 *                                             the PCR bank layout, the implemented command list
 *   PCR_Read / PCR_Extend                  -- 24 PCRs, one SHA-256 bank, real digest chaining
 *   GetRandom / StirRandom                 -- host-entropy backed (injected, testable)
 *
 * EK/SRK creation, NV storage and the attestation commands are the follow-up ticket named on
 * #433 -- they need key generation, which is where integrate-vs-implement gets revisited.
 *
 * Pure and per-instance: no file-global state (the multi-VM singleton class), no allocation,
 * fully unit-tested with byte-exact command/response vectors.
 */

#define HYPE_TPM2_PCR_COUNT 24u
#define HYPE_TPM2_MAX_CMD 1024u
#define HYPE_TPM2_MAX_RSP 1024u

typedef struct {
    uint8_t pcr[HYPE_TPM2_PCR_COUNT][HYPE_SHA256_DIGEST_SIZE];
    int started;      /* TPM2_Startup seen (CLEAR) */
    int shutdown;     /* TPM2_Shutdown seen -- further commands fail TPM_RC_INITIALIZE */
    int selftested;
    /* Entropy source, injected so tests are deterministic and the freestanding build wires
     * RDRAND/RDTSC mixing without this module knowing. */
    uint64_t (*entropy)(void *ctx);
    void *entropy_ctx;
} hype_tpm2_t;

void hype_tpm2_reset(hype_tpm2_t *t, uint64_t (*entropy)(void *ctx), void *entropy_ctx);

/*
 * Execute one command buffer, writing the response. `cmd_len` is what the guest submitted;
 * the response length is returned (always >= 10: even a malformed command gets a proper
 * error header). Never reads past cmd_len, never writes past HYPE_TPM2_MAX_RSP.
 */
unsigned int hype_tpm2_execute(hype_tpm2_t *t, const uint8_t *cmd, unsigned int cmd_len,
                               uint8_t *rsp);

#endif /* HYPE_CORE_TPM2_H */
