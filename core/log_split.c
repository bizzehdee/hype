#include "log_split.h"

/* Matches `lit` at rec[*pos]; advances *pos past it and returns 1 on success. */
static int eat_lit(const char *rec, unsigned int len, unsigned int *pos, const char *lit) {
    unsigned int p = *pos;
    unsigned int i = 0;
    while (lit[i] != '\0') {
        if (p + i >= len || rec[p + i] != lit[i]) return 0;
        i++;
    }
    *pos = p + i;
    return 1;
}

/* Reads a decimal run at rec[*pos] into *out. Returns 0 if no digit is present
 * or the value would exceed a sane VM index (guards a malformed record from
 * producing a wild index that a caller might use to select a sink). */
static int eat_uint(const char *rec, unsigned int len, unsigned int *pos, unsigned int *out) {
    unsigned int p = *pos;
    unsigned int v = 0;
    unsigned int digits = 0;
    while (p < len && rec[p] >= '0' && rec[p] <= '9') {
        v = v * 10u + (unsigned int)(rec[p] - '0');
        if (v > 9999u) return 0;
        p++;
        digits++;
    }
    if (digits == 0u) return 0;
    *pos = p;
    *out = v;
    return 1;
}

/*
 * Parses "fw-1 vm<N> ttyS<M>| ". On success returns 1, sets *vm to N and *body
 * to the offset just past "fw-1 vm<N> " -- i.e. the start of "ttyS<M>| ", which
 * the per-VM file keeps.
 */
static int parse_guest_prefix(const char *rec, unsigned int len, unsigned int *vm,
                              unsigned int *body) {
    unsigned int pos = 0;
    unsigned int n = 0;
    unsigned int port = 0;
    unsigned int after_vm;

    if (rec == 0) return 0;
    if (!eat_lit(rec, len, &pos, "fw-1 vm")) return 0;
    if (!eat_uint(rec, len, &pos, &n)) return 0;
    if (!eat_lit(rec, len, &pos, " ")) return 0;
    after_vm = pos;
    if (!eat_lit(rec, len, &pos, "ttyS")) return 0;
    if (!eat_uint(rec, len, &pos, &port)) return 0;
    if (!eat_lit(rec, len, &pos, "| ")) return 0;

    *vm = n;
    *body = after_vm;
    return 1;
}

int hype_log_record_vm(const char *rec, unsigned int len) {
    unsigned int vm = 0;
    unsigned int body = 0;
    if (!parse_guest_prefix(rec, len, &vm, &body)) return HYPE_LOG_VM_HYPE;
    return (int)vm;
}

unsigned int hype_log_record_body_off(const char *rec, unsigned int len) {
    unsigned int vm = 0;
    unsigned int body = 0;
    if (!parse_guest_prefix(rec, len, &vm, &body)) return 0u;
    return body;
}
