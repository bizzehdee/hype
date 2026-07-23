#include "cfg.h"
#include "strutil.h"

enum {
    F_VCPUS = 1u << 0,
    F_CPU_SET = 1u << 1,
    F_MEM_MB = 1u << 2,
    F_BOOT = 1u << 3,
    F_INSTALL_MEDIA = 1u << 4,
    F_TARGET_DISK = 1u << 5,
    F_TARGET_DISK_SIZE_GB = 1u << 6,
    F_FIRMWARE = 1u << 7,
    F_OS_HINT = 1u << 8,
    F_NET_MODE = 1u << 9,
    F_NET_PEERS = 1u << 10,
    F_PARTITION = 1u << 11,
    F_ALLOW_OVERWRITE = 1u << 12
};

/* Parses a boolean value: true/false, yes/no, on/off, 1/0. */
static hype_cfg_status_t parse_bool_field(const char *val, int *out) {
    if (hype_streq(val, "true") || hype_streq(val, "yes") || hype_streq(val, "on") ||
        hype_streq(val, "1")) {
        *out = 1;
        return HYPE_CFG_OK;
    }
    if (hype_streq(val, "false") || hype_streq(val, "no") || hype_streq(val, "off") ||
        hype_streq(val, "0")) {
        *out = 0;
        return HYPE_CFG_OK;
    }
    return HYPE_CFG_ERR_BAD_VALUE;
}

static char *find_char(char *s, char c) {
    while (*s && *s != c) {
        s++;
    }
    return s;
}

/* Truncates at the first ';' (comment to end of line), then trims. */
static char *clean_line(char *line) {
    *find_char(line, ';') = '\0';
    return hype_str_trim(line);
}

static void zero_vm(hype_cfg_vm_t *vm) {
    unsigned char *b = (unsigned char *)vm;
    unsigned long long i;
    for (i = 0; i < sizeof(*vm); i++) {
        b[i] = 0;
    }
    vm->net_mode = HYPE_CFG_NET_NONE;
}

static hype_cfg_status_t parse_target_disk(char *val, hype_cfg_target_disk_t *td) {
    const char *prefix;
    hype_cfg_disk_kind_t kind;
    unsigned long long prefix_len;
    unsigned long long len;

    if (hype_strneq(val, "file:", 5)) {
        prefix = "file:";
        kind = HYPE_CFG_DISK_FILE;
    } else if (hype_strneq(val, "physical:", 9)) {
        prefix = "physical:";
        kind = HYPE_CFG_DISK_PHYSICAL;
    } else {
        return HYPE_CFG_ERR_BAD_VALUE;
    }

    prefix_len = hype_strlen(prefix);
    len = hype_strlcpy(td->path_or_id, val + prefix_len, HYPE_CFG_PATH_MAX);
    if (len >= HYPE_CFG_PATH_MAX) {
        return HYPE_CFG_ERR_VALUE_TOO_LONG;
    }
    if (td->path_or_id[0] == '\0') {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    td->kind = kind;
    return HYPE_CFG_OK;
}

static int cpu_set_contains(const hype_cfg_vm_t *vm, unsigned int core) {
    unsigned int i;
    for (i = 0; i < vm->cpu_set_count; i++) {
        if (vm->cpu_set[i] == core) {
            return 1;
        }
    }
    return 0;
}

static hype_cfg_status_t cpu_set_add(hype_cfg_vm_t *vm, unsigned int core) {
    if (cpu_set_contains(vm, core)) {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    if (vm->cpu_set_count >= HYPE_CFG_MAX_CPUS) {
        return HYPE_CFG_ERR_TOO_MANY_ENTRIES;
    }
    vm->cpu_set[vm->cpu_set_count++] = core;
    return HYPE_CFG_OK;
}

/* Splits `val` on ',' in place, trims each piece, and hands it to `handle_one`. */
static hype_cfg_status_t for_each_comma_piece(char *val, hype_cfg_vm_t *vm,
                                               hype_cfg_status_t (*handle_one)(char *, hype_cfg_vm_t *)) {
    char *tok = val;

    if (*val == '\0') {
        return HYPE_CFG_ERR_BAD_VALUE;
    }

    for (;;) {
        char *comma = find_char(tok, ',');
        int has_more = (*comma == ',');
        hype_cfg_status_t st;

        if (has_more) {
            *comma = '\0';
        }

        st = handle_one(hype_str_trim(tok), vm);
        if (st != HYPE_CFG_OK) {
            return st;
        }

        if (!has_more) {
            break;
        }
        tok = comma + 1;
    }
    return HYPE_CFG_OK;
}

static hype_cfg_status_t cpu_set_piece(char *piece, hype_cfg_vm_t *vm) {
    char *dash = find_char(piece, '-');
    unsigned long long a, b, c;

    if (*dash == '-') {
        *dash = '\0';
        if (hype_parse_uint(hype_str_trim(piece), &a) != 0 ||
            hype_parse_uint(hype_str_trim(dash + 1), &b) != 0) {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        if (a > b || b > 0xFFFFFFFFULL) {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        for (c = a; c <= b; c++) {
            hype_cfg_status_t st = cpu_set_add(vm, (unsigned int)c);
            if (st != HYPE_CFG_OK) {
                return st;
            }
        }
        return HYPE_CFG_OK;
    }

    if (hype_parse_uint(piece, &a) != 0 || a > 0xFFFFFFFFULL) {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    return cpu_set_add(vm, (unsigned int)a);
}

static hype_cfg_status_t parse_cpu_set(char *val, hype_cfg_vm_t *vm) {
    vm->cpu_set_count = 0;
    return for_each_comma_piece(val, vm, cpu_set_piece);
}

static hype_cfg_status_t net_peer_piece(char *name, hype_cfg_vm_t *vm) {
    unsigned long long len;
    unsigned int i;

    if (*name == '\0') {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    for (i = 0; i < vm->net_peers_count; i++) {
        if (hype_streq(vm->net_peers[i], name)) {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
    }
    if (vm->net_peers_count >= HYPE_CFG_MAX_PEERS) {
        return HYPE_CFG_ERR_TOO_MANY_ENTRIES;
    }
    len = hype_strlcpy(vm->net_peers[vm->net_peers_count], name, HYPE_CFG_NAME_MAX);
    if (len >= HYPE_CFG_NAME_MAX) {
        return HYPE_CFG_ERR_VALUE_TOO_LONG;
    }
    vm->net_peers_count++;
    return HYPE_CFG_OK;
}

static hype_cfg_status_t parse_net_peers(char *val, hype_cfg_vm_t *vm) {
    vm->net_peers_count = 0;
    return for_each_comma_piece(val, vm, net_peer_piece);
}

static hype_cfg_status_t parse_uint_field(char *val, unsigned int *out) {
    unsigned long long v;
    if (hype_parse_uint(val, &v) != 0 || v == 0 || v > 0xFFFFFFFFULL) {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    *out = (unsigned int)v;
    return HYPE_CFG_OK;
}

static hype_cfg_status_t apply_field(hype_cfg_vm_t *vm, unsigned int *seen, char *key, char *val) {
    if (hype_streq(key, "vcpus")) {
        hype_cfg_status_t st;
        if (*seen & F_VCPUS) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_uint_field(val, &vm->vcpus);
        if (st != HYPE_CFG_OK) return st;
        *seen |= F_VCPUS;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "cpu_set")) {
        hype_cfg_status_t st;
        if (*seen & F_CPU_SET) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_cpu_set(val, vm);
        if (st != HYPE_CFG_OK) return st;
        vm->has_cpu_set = 1;
        *seen |= F_CPU_SET;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "mem_mb")) {
        hype_cfg_status_t st;
        if (*seen & F_MEM_MB) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_uint_field(val, &vm->mem_mb);
        if (st != HYPE_CFG_OK) return st;
        *seen |= F_MEM_MB;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "boot")) {
        if (*seen & F_BOOT) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "installer")) vm->boot = HYPE_CFG_BOOT_INSTALLER;
        else if (hype_streq(val, "disk")) vm->boot = HYPE_CFG_BOOT_DISK;
        else return HYPE_CFG_ERR_BAD_VALUE;
        *seen |= F_BOOT;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "install_media")) {
        unsigned long long len;
        if (*seen & F_INSTALL_MEDIA) return HYPE_CFG_ERR_DUPLICATE_KEY;
        len = hype_strlcpy(vm->install_media, val, HYPE_CFG_PATH_MAX);
        if (len >= HYPE_CFG_PATH_MAX) return HYPE_CFG_ERR_VALUE_TOO_LONG;
        vm->has_install_media = 1;
        *seen |= F_INSTALL_MEDIA;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "target_disk")) {
        hype_cfg_status_t st;
        if (*seen & F_TARGET_DISK) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_target_disk(val, &vm->target_disk);
        if (st != HYPE_CFG_OK) return st;
        *seen |= F_TARGET_DISK;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "target_disk_size_gb")) {
        hype_cfg_status_t st;
        if (*seen & F_TARGET_DISK_SIZE_GB) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_uint_field(val, &vm->target_disk_size_gb);
        if (st != HYPE_CFG_OK) return st;
        vm->has_target_disk_size_gb = 1;
        *seen |= F_TARGET_DISK_SIZE_GB;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "firmware")) {
        if (*seen & F_FIRMWARE) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "uefi")) vm->firmware = HYPE_CFG_FW_UEFI;
        else if (hype_streq(val, "legacy")) vm->firmware = HYPE_CFG_FW_LEGACY;
        else return HYPE_CFG_ERR_BAD_VALUE;
        *seen |= F_FIRMWARE;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "os_hint")) {
        if (*seen & F_OS_HINT) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "windows")) vm->os_hint = HYPE_CFG_OS_WINDOWS;
        else if (hype_streq(val, "linux")) vm->os_hint = HYPE_CFG_OS_LINUX;
        else if (hype_streq(val, "bsd")) vm->os_hint = HYPE_CFG_OS_BSD;
        else if (hype_streq(val, "none")) vm->os_hint = HYPE_CFG_OS_NONE;
        else return HYPE_CFG_ERR_BAD_VALUE;
        *seen |= F_OS_HINT;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "net_mode")) {
        if (*seen & F_NET_MODE) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "none")) vm->net_mode = HYPE_CFG_NET_NONE;
        else if (hype_streq(val, "nat")) vm->net_mode = HYPE_CFG_NET_NAT;
        else return HYPE_CFG_ERR_BAD_VALUE;
        *seen |= F_NET_MODE;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "net_peers")) {
        hype_cfg_status_t st;
        if (*seen & F_NET_PEERS) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_net_peers(val, vm);
        if (st != HYPE_CFG_OK) return st;
        *seen |= F_NET_PEERS;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "partition")) {
        if (*seen & F_PARTITION) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "whole")) {
            vm->target_disk.partition = 0; /* whole disk */
        } else {
            hype_cfg_status_t st = parse_uint_field(val, &vm->target_disk.partition);
            if (st != HYPE_CFG_OK) return st;
            if (vm->target_disk.partition == 0) return HYPE_CFG_ERR_BAD_VALUE; /* 1-based */
        }
        *seen |= F_PARTITION;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "allow_overwrite")) {
        hype_cfg_status_t st;
        if (*seen & F_ALLOW_OVERWRITE) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_bool_field(val, &vm->target_disk.allow_overwrite);
        if (st != HYPE_CFG_OK) return st;
        *seen |= F_ALLOW_OVERWRITE;
        return HYPE_CFG_OK;
    }
    return HYPE_CFG_ERR_UNKNOWN_KEY;
}

static hype_cfg_status_t validate_required(const hype_cfg_vm_t *vm, unsigned int seen) {
    if (!(seen & F_VCPUS)) return HYPE_CFG_ERR_MISSING_REQUIRED;
    if (!(seen & F_MEM_MB)) return HYPE_CFG_ERR_MISSING_REQUIRED;
    if (!(seen & F_BOOT)) return HYPE_CFG_ERR_MISSING_REQUIRED;
    if (!(seen & F_TARGET_DISK)) return HYPE_CFG_ERR_MISSING_REQUIRED;
    if (!(seen & F_FIRMWARE)) return HYPE_CFG_ERR_MISSING_REQUIRED;
    if (!(seen & F_OS_HINT)) return HYPE_CFG_ERR_MISSING_REQUIRED;
    if (vm->boot == HYPE_CFG_BOOT_INSTALLER && !(seen & F_INSTALL_MEDIA)) {
        return HYPE_CFG_ERR_MISSING_REQUIRED;
    }
    return HYPE_CFG_OK;
}

static hype_cfg_status_t process_section_header(char *line, hype_cfg_t *out, int *cur,
                                                 unsigned int *seen) {
    unsigned long long len = hype_strlen(line);
    char *body;
    char *name;
    unsigned long long name_len;
    unsigned int i;

    if (len < 2 || line[len - 1] != ']') {
        return HYPE_CFG_ERR_SYNTAX;
    }
    line[len - 1] = '\0';
    body = line + 1;
    if (!hype_strneq(body, "vm.", 3)) {
        return HYPE_CFG_ERR_SYNTAX;
    }
    name = body + 3;
    if (*name == '\0') {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    for (i = 0; i < out->vm_count; i++) {
        if (hype_streq(out->vms[i].name, name)) {
            return HYPE_CFG_ERR_DUPLICATE_VM_NAME;
        }
    }
    if (out->vm_count >= HYPE_CFG_MAX_VMS) {
        return HYPE_CFG_ERR_TOO_MANY_VMS;
    }

    *cur = (int)out->vm_count;
    zero_vm(&out->vms[*cur]);
    name_len = hype_strlcpy(out->vms[*cur].name, name, HYPE_CFG_NAME_MAX);
    if (name_len >= HYPE_CFG_NAME_MAX) {
        return HYPE_CFG_ERR_VALUE_TOO_LONG;
    }
    seen[*cur] = 0;
    out->vm_count++;
    return HYPE_CFG_OK;
}

static hype_cfg_status_t process_key_value(char *line, hype_cfg_t *out, int cur,
                                            unsigned int *seen) {
    char *eq;
    char *key;
    char *val;

    if (cur < 0) {
        return HYPE_CFG_ERR_KEY_BEFORE_SECTION;
    }

    eq = find_char(line, '=');
    if (*eq != '=') {
        return HYPE_CFG_ERR_SYNTAX;
    }
    *eq = '\0';
    key = hype_str_trim(line);
    val = hype_str_trim(eq + 1);
    if (*key == '\0') {
        return HYPE_CFG_ERR_SYNTAX;
    }

    return apply_field(&out->vms[cur], &seen[cur], key, val);
}

hype_cfg_result_t hype_cfg_parse(char *text, hype_cfg_t *out) {
    hype_cfg_result_t res;
    unsigned int seen[HYPE_CFG_MAX_VMS];
    int cur = -1;
    unsigned int line_no = 0;
    char *p = text;
    unsigned int i;

    res.status = HYPE_CFG_OK;
    res.line = 0;
    out->vm_count = 0;
    for (i = 0; i < HYPE_CFG_MAX_VMS; i++) {
        seen[i] = 0;
    }

    while (*p) {
        char *line_start = p;
        char *line;
        hype_cfg_status_t st;

        while (*p && *p != '\n') {
            p++;
        }
        if (*p == '\n') {
            *p = '\0';
            p++;
        }
        line_no++;

        line = clean_line(line_start);
        if (*line == '\0') {
            continue;
        }

        if (line[0] == '[') {
            st = process_section_header(line, out, &cur, seen);
        } else {
            st = process_key_value(line, out, cur, seen);
        }

        if (st != HYPE_CFG_OK) {
            res.status = st;
            res.line = line_no;
            return res;
        }
    }

    for (i = 0; i < out->vm_count; i++) {
        hype_cfg_status_t st = validate_required(&out->vms[i], seen[i]);
        if (st != HYPE_CFG_OK) {
            res.status = st;
            res.line = 0;
            return res;
        }
    }

    return res;
}
