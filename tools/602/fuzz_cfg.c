/*
 * #602: host-side libFuzzer harness over core/cfg.c, hype.cfg's own never-fail parser.
 *
 * docs/hype-cfg-spec.md 4.3 ("per-section resilience"): a malformed [vm.*]/[disk.*]/
 * [nic.*]/[switch.*] is reported and SKIPPED, and a malformed [hype] falls back to
 * defaults with `hype.malformed` set -- neither is a parse failure. Only a
 * document-structural problem (an unterminated section header, a key before any
 * section) legitimately returns a non-OK hype_cfg_result_t. So the assertion here is
 * NOT "parsing always succeeds" -- it is "parsing never crashes, and whatever it
 * returns is internally consistent": every count stays within its declared array
 * capacity, every enum stays in its defined range, and `malformed` is exactly 0 or 1.
 * A violation calls abort() so libFuzzer/ASan reports it as a crash, exactly like a
 * memory-safety bug -- this is the "no crash, malformed set correctly" contract the
 * ticket names, made mechanically checkable.
 *
 * `hype_cfg_parse()` takes a mutable, NUL-terminated buffer and may use it as scratch
 * space, so the fuzz input is copied into a fixed buffer first (test_cfg.c's own
 * parse_copy() does the same for the same reason).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/cfg.h"

#define FUZZ_CFG_BUF_SIZE 16384u

static void check(int cond, const char *why) {
    if (!cond) {
        /* abort() rather than a silent return: this is the harness's own assertion
         * that the never-fail contract held, and a violation must surface as a crash
         * for libFuzzer to bisect and file, same as a real memory-safety bug would.
         * Printed first (not just passed to abort()) so replaying a saved crash file
         * says which invariant failed without needing a debugger. */
        fprintf(stderr, "fuzz_cfg: invariant violated: %s\n", why);
        abort();
    }
}

static void check_invariants(const hype_cfg_t *cfg, hype_cfg_result_t res) {
    unsigned int i;

    check(res.status >= HYPE_CFG_OK && res.status <= HYPE_CFG_ERR_MISSING_REQUIRED,
          "result.status outside its declared enum range");
    check(cfg->hype.malformed == 0 || cfg->hype.malformed == 1, "malformed is not 0/1");
    /* §4.3: a malformed [hype] still leaves every global at a USABLE value, never an
     * out-of-range one a consumer would have to re-validate. */
    check(cfg->hype.log_level == HYPE_LOG_ERROR || cfg->hype.log_level == HYPE_LOG_WARN ||
              cfg->hype.log_level == HYPE_LOG_INFO || cfg->hype.log_level == HYPE_LOG_DEBUG,
          "log_level outside its enum range");
    check(cfg->hype.cpu_avg_window_secs >= 1u, "cpu_avg_window_secs clamped below 1 (spec: >=1)");
    check(cfg->hype.host_cpu_budget_count <= HYPE_CFG_MAX_CPUS, "host_cpu_budget_count overflow");
    check(cfg->hype.autostart_count <= HYPE_CFG_MAX_VMS, "autostart_count overflow");
    check(cfg->vm_count <= cfg->vm_cap, "vm_count exceeds the caller-owned vm_cap");
    check(cfg->disk_count <= HYPE_CFG_MAX_DISKS, "disk_count overflow");
    check(cfg->nic_count <= HYPE_CFG_MAX_NICS, "nic_count overflow");
    check(cfg->switch_count <= HYPE_CFG_MAX_SWITCHES, "switch_count overflow");
    check(cfg->section_count <= HYPE_CFG_MAX_SECTIONS, "section_count overflow");

    /*
     * The compaction that follows a dropped [vm.*]/[disk.*] (core/cfg.c's remap[] blocks
     * around the "§4.3: a malformed [disk.*] is dropped" comment) can leave a section
     * table entry with its KIND still VM/DISK but its INDEX remapped to -1 -- "this
     * entity was dropped". hype_cfg_serialize()'s switch on `sec->kind` does not check
     * for that before indexing `cfg->vms[sec->index]` / `cfg->disks[sec->index]` (found
     * by this fuzzer: cfg.c:2564, `disks[16]` indexed at -1, UBSan array-bounds). `vms`
     * is a caller-owned POINTER rather than a fixed array, so the equivalent
     * `vms[-1]` read does not trip UBSan's array-bounds check the way `disks[-1]` does
     * -- it silently reads whatever host memory precedes the VM storage and would
     * serialize it into the written-back config, which is the "silent" failure this
     * ticket's harnesses exist to catch, not merely a louder one. Checked directly
     * here rather than relying on a sanitizer to notice.
     *
     * #673: hype_cfg_serialize() now guards each of these cases on `index >= 0` (the
     * minimal safe fix landed alongside this harness, so #602's own fuzzing could get
     * past this known/filed issue rather than re-finding it on every run) -- so -1 is
     * no longer unsafe to serialize, only unusual, and is allowed here. What must never
     * happen is an index that is neither -1 NOR a valid live slot: that would be a
     * DIFFERENT, worse bug (reading some other entity's struct) that #673's fix does
     * not protect against and this harness should still fail loudly on. */
    for (i = 0; i < cfg->section_count; i++) {
        hype_cfg_section_kind_t kind = cfg->sections[i].kind;
        int idx = cfg->sections[i].index;
        if (kind == HYPE_CFG_SECTION_VM) {
            check(idx == -1 || (unsigned int)idx < cfg->vm_count,
                  "a VM-kind section's index is neither -1 nor a valid live VM slot");
        } else if (kind == HYPE_CFG_SECTION_DISK) {
            check(idx == -1 || (unsigned int)idx < cfg->disk_count,
                  "a disk-kind section's index is neither -1 nor a valid live disk slot");
        } else if (kind == HYPE_CFG_SECTION_NIC) {
            check(idx == -1 || (unsigned int)idx < cfg->nic_count,
                  "a NIC-kind section's index is neither -1 nor a valid live NIC slot");
        } else if (kind == HYPE_CFG_SECTION_SWITCH) {
            check(idx == -1 || (unsigned int)idx < cfg->switch_count,
                  "a switch-kind section's index is neither -1 nor a valid live switch slot");
        }
    }

    for (i = 0; i < cfg->vm_count; i++) {
        /* Every retained VM name must itself be NUL-terminated within its fixed field --
         * a name that fills HYPE_CFG_NAME_MAX with no terminator would turn every later
         * strlen/strcmp on it into a read past the field. */
        const char *name = cfg->vms[i].name;
        size_t j;
        int terminated = 0;
        for (j = 0; j < HYPE_CFG_NAME_MAX; j++) {
            if (name[j] == '\0') {
                terminated = 1;
                break;
            }
        }
        check(terminated, "a VM name is not NUL-terminated within HYPE_CFG_NAME_MAX");
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static char buf[FUZZ_CFG_BUF_SIZE];
    static char reparse_buf[FUZZ_CFG_BUF_SIZE];
    static char serialized[65536];
    hype_cfg_t cfg;
    hype_cfg_result_t res;
    size_t n = size < FUZZ_CFG_BUF_SIZE - 1u ? size : FUZZ_CFG_BUF_SIZE - 1u;

    if (n != 0) {
        memcpy(buf, data, n);
    }
    buf[n] = '\0';

    hype_cfg_init(&cfg);
    res = hype_cfg_parse(buf, &cfg);
    check_invariants(&cfg, res);

    /* Round-trip: a config the parser accepted (status OK) must serialize and re-parse
     * cleanly. Skipped when the parser itself says the round trip is unsafe
     * (retained_overflow / truncated), per hype_cfg_serialize()'s own documented
     * contract.
     *
     * NOT asserted: that a malformed [hype] stays malformed after the round trip.
     * §4.3's fallback replaces a bad global VALUE with a clean default, and
     * serialize_hype() emits that (now-clean) default -- so a malformed original can
     * legitimately reparse as not-malformed; that is the fallback working as
     * documented, not a defect (found the hard way: an earlier version of this
     * harness asserted equality here and flagged this correct behavior as a false
     * positive). What must never happen is the other direction: round-tripping a
     * config that was NOT malformed must never produce something that reparses AS
     * malformed -- that would mean serialize_hype() itself emitted something the
     * parser cannot read back, a genuine bug this harness should still catch. */
    if (res.status == HYPE_CFG_OK) {
        hype_cfg_serialize_result_t sres = hype_cfg_serialize(&cfg, serialized, sizeof(serialized));
        if (!sres.refused_overflow && !sres.truncated) {
            hype_cfg_t cfg2;
            hype_cfg_result_t res2;
            size_t slen = sres.len < sizeof(reparse_buf) - 1u ? sres.len : sizeof(reparse_buf) - 1u;
            memcpy(reparse_buf, serialized, slen);
            reparse_buf[slen] = '\0';

            hype_cfg_init(&cfg2);
            res2 = hype_cfg_parse(reparse_buf, &cfg2);
            check_invariants(&cfg2, res2);
            check(res2.status == HYPE_CFG_OK, "serialized output failed to re-parse as OK");
            check(cfg.hype.malformed != 0 || cfg2.hype.malformed == 0,
                  "round trip introduced a NEW [hype] malformed state that was not there before");
        }
    }

    return 0;
}
