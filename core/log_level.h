#ifndef HYPE_CORE_LOG_LEVEL_H
#define HYPE_CORE_LOG_LEVEL_H

/*
 * #533: the post-ExitBootServices log level, chosen in hype.cfg.
 *
 * hype's logging was all-or-nothing: every diagnostic, always. That is the right default for
 * bring-up and it is why several bugs here were catchable at all, but it has costs -- the on-stick
 * drain is the busiest writer hype has (#522 measured 370-1126 ms for a single 4 KiB slice on real
 * hardware), and the 2 MiB capture buffer fills on long runs and then latches truncated, so the END
 * of a long run is the part most likely to be missing.
 *
 * Two rules this module exists to keep:
 *
 *  - **Named levels, never numbers.** A number in a config file is unreadable six months later.
 *  - **Anything unreadable means DEBUG.** No config, a broken config, an unknown level name: every
 *    one of those falls back to logging everything. A host that cannot read its config is exactly
 *    the host whose log matters most, and a quiet log there would be the worst possible failure --
 *    the same reasoning #370 records about diagnostics that hide what they exist to show.
 */

typedef enum {
    HYPE_LOG_ERROR = 0, /* it failed, or was refused: panics, admission refusals, I/O failures */
    HYPE_LOG_WARN = 1,  /* it is not what was asked for, but it continues: caps, fallbacks */
    HYPE_LOG_INFO = 2,  /* what hype decided and did: config outcomes, carves, VM lifecycle */
    HYPE_LOG_DEBUG = 3  /* everything, including per-exit and per-device detail */
} hype_log_level_t;

/* Parses "error"/"warn"/"info"/"debug", case-sensitive like every other config value. Returns 0 and
 * writes *out on success; returns -1 and leaves *out untouched otherwise, so a caller that
 * pre-seeded DEBUG keeps it. */
int hype_log_level_parse(const char *name, hype_log_level_t *out);

const char *hype_log_level_name(hype_log_level_t level);

/* 1 when a message of `msg` level should be emitted at the `current` setting. */
int hype_log_level_enabled(hype_log_level_t current, hype_log_level_t msg);

#endif /* HYPE_CORE_LOG_LEVEL_H */
