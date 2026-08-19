#include "log_level.h"
#include "strutil.h"

int hype_log_level_parse(const char *name, hype_log_level_t *out) {
    if (name == 0 || out == 0) {
        return -1;
    }
    if (hype_streq(name, "error")) {
        *out = HYPE_LOG_ERROR;
    } else if (hype_streq(name, "warn")) {
        *out = HYPE_LOG_WARN;
    } else if (hype_streq(name, "info")) {
        *out = HYPE_LOG_INFO;
    } else if (hype_streq(name, "debug")) {
        *out = HYPE_LOG_DEBUG;
    } else {
        return -1;
    }
    return 0;
}

const char *hype_log_level_name(hype_log_level_t level) {
    switch (level) {
        case HYPE_LOG_ERROR: return "error";
        case HYPE_LOG_WARN: return "warn";
        case HYPE_LOG_INFO: return "info";
        case HYPE_LOG_DEBUG: return "debug";
    }
    return "debug"; /* an out-of-range level logs everything, never nothing */
}

int hype_log_level_enabled(hype_log_level_t current, hype_log_level_t msg) {
    return (int)msg <= (int)current;
}
