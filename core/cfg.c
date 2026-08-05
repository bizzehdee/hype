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
    F_ALLOW_OVERWRITE = 1u << 12,
    F_MEDIA_DISK = 1u << 13,
    F_DISKS = 1u << 14,
    F_CDROMS = 1u << 15,
    F_BOOT_ORDER = 1u << 16 /* #323 */
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


/*
 * #222: the cpu-list destination, so `[hype] host_cpu_budget` reuses this exact range/list grammar
 * (`0-3`, `0,1,2`) rather than growing a second parser that would inevitably diverge on the edges.
 * Same single-threaded, one-key-at-a-time reasoning as g_list_dst below.
 */
static unsigned int *g_cpu_dst;
static unsigned int *g_cpu_dst_count;

static int cpu_dst_contains(unsigned int core) {
    unsigned int i;
    for (i = 0; i < *g_cpu_dst_count; i++) {
        if (g_cpu_dst[i] == core) {
            return 1;
        }
    }
    return 0;
}

static hype_cfg_status_t cpu_set_add(hype_cfg_vm_t *vm, unsigned int core) {
    (void)vm;
    if (cpu_dst_contains(core)) {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    if (*g_cpu_dst_count >= HYPE_CFG_MAX_CPUS) {
        return HYPE_CFG_ERR_TOO_MANY_ENTRIES;
    }
    g_cpu_dst[(*g_cpu_dst_count)++] = core;
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
    g_cpu_dst = vm->cpu_set;
    g_cpu_dst_count = &vm->cpu_set_count;
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

/*
 * #222 (§5.2): the disks/cdroms/boot_order id lists.
 *
 * for_each_comma_piece only passes the VM, so which list is being filled is carried in a file-local
 * pointer. Safe because the parser is single-threaded and strictly one key at a time -- but stated,
 * because a static here would be a bug the moment that stopped being true.
 */
static char (*g_list_dst)[HYPE_CFG_NAME_MAX];
static unsigned int *g_list_count;
static unsigned int g_list_cap;

static hype_cfg_status_t id_list_piece(char *name, hype_cfg_vm_t *vm) {
    unsigned int i;

    (void)vm;
    if (*name == '\0') {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    /* A repeated id would attach the same device twice -- for a writable disk that is two guest
     * devices over one backing store, i.e. guaranteed corruption. Refused here rather than left to
     * admission, because it needs no cross-entity knowledge to see. */
    for (i = 0; i < *g_list_count; i++) {
        if (hype_streq(g_list_dst[i], name)) {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
    }
    if (*g_list_count >= g_list_cap) {
        return HYPE_CFG_ERR_TOO_MANY_ENTRIES;
    }
    if (hype_strlcpy(g_list_dst[*g_list_count], name, HYPE_CFG_NAME_MAX) >= HYPE_CFG_NAME_MAX) {
        return HYPE_CFG_ERR_VALUE_TOO_LONG;
    }
    (*g_list_count)++;
    return HYPE_CFG_OK;
}

static hype_cfg_status_t parse_id_list(char *val, hype_cfg_vm_t *vm,
                                       char (*dst)[HYPE_CFG_NAME_MAX], unsigned int *count,
                                       unsigned int cap) {
    *count = 0;
    g_list_dst = dst;
    g_list_count = count;
    g_list_cap = cap;
    return for_each_comma_piece(val, vm, id_list_piece);
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
    if (hype_streq(key, "media_disk")) {
        /* #323: the drive the media lives on, by serial/GUID. Optional -- absent means
         * auto-detect, preserving the pre-#323 behaviour for every existing config. */
        unsigned long long len;
        if (*seen & F_MEDIA_DISK) return HYPE_CFG_ERR_DUPLICATE_KEY;
        len = hype_strlcpy(vm->media_disk, val, HYPE_CFG_PATH_MAX);
        if (len >= HYPE_CFG_PATH_MAX) return HYPE_CFG_ERR_VALUE_TOO_LONG;
        if (vm->media_disk[0] == '\0') return HYPE_CFG_ERR_BAD_VALUE;
        vm->has_media_disk = 1;
        *seen |= F_MEDIA_DISK;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "disks")) {
        hype_cfg_status_t st;
        if (*seen & F_DISKS) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_id_list(val, vm, vm->disks, &vm->disks_count, HYPE_CFG_MAX_VM_DISKS);
        if (st != HYPE_CFG_OK) return st;
        *seen |= F_DISKS;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "cdroms")) {
        hype_cfg_status_t st;
        if (*seen & F_CDROMS) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_id_list(val, vm, vm->cdroms, &vm->cdroms_count, HYPE_CFG_MAX_VM_DISKS);
        if (st != HYPE_CFG_OK) return st;
        *seen |= F_CDROMS;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "boot_order")) {
        hype_cfg_status_t st;
        if (*seen & F_BOOT_ORDER) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_id_list(val, vm, vm->boot_order, &vm->boot_order_count,
                           HYPE_CFG_MAX_BOOT_ORDER);
        if (st != HYPE_CFG_OK) return st;
        *seen |= F_BOOT_ORDER;
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
    /*
     * #222 (§7): a VM says where its storage comes from EITHER inline (target_disk) OR by reference
     * (disks/cdroms) -- never both, since two answers to "which disk" cannot be reconciled and
     * picking one silently would be exactly the #285 failure again.
     */
    if ((seen & F_TARGET_DISK) && ((seen & F_DISKS) || (seen & F_CDROMS))) {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    /*
     * One of the two forms is required. §5.2 does say a VM with zero disks and zero cdroms is valid,
     * and this deliberately does NOT permit that yet: nothing can launch such a VM, and accepting it
     * would turn a misspelled `target_disk` line into a silently disk-less guest -- a config that
     * looks accepted and produces a machine that cannot boot. Revisit with #329, which is what will
     * make an attached-device list mean anything at launch.
     */
    if (!(seen & F_TARGET_DISK) && !(seen & F_DISKS) && !(seen & F_CDROMS)) {
        return HYPE_CFG_ERR_MISSING_REQUIRED;
    }
    if (!(seen & F_FIRMWARE)) return HYPE_CFG_ERR_MISSING_REQUIRED;
    if (!(seen & F_OS_HINT)) return HYPE_CFG_ERR_MISSING_REQUIRED;
    if (vm->boot == HYPE_CFG_BOOT_INSTALLER && !(seen & F_INSTALL_MEDIA)) {
        return HYPE_CFG_ERR_MISSING_REQUIRED;
    }
    return HYPE_CFG_OK;
}

/* #222 (§5.1): per-[hype] duplicate-key tracking. */
enum {
    H_CONFIG_VERSION = 1u << 0,
    H_HOST_CPU_BUDGET = 1u << 1,
    H_DEFAULT_NET_MODE = 1u << 2,
    H_DASHBOARD_VIEW = 1u << 3,
    H_AUTOSTART = 1u << 4
};

static void hype_globals_defaults(hype_cfg_hype_t *h) {
    unsigned char *p = (unsigned char *)h;
    unsigned long long i;
    for (i = 0; i < sizeof(*h); i++) {
        p[i] = 0;
    }
    /* Zero is the right default for every enum here (NET_NONE, VIEW_DASHBOARD, AUTOSTART_ALL) and
     * for the empty cpu budget meaning "all cores". config_version is the one exception. */
    h->config_version = 1u;
}

/* `autostart = a, b` -- the list form. `all` / `none` are handled by the caller. */
static hype_cfg_status_t autostart_piece(char *name, hype_cfg_vm_t *vm) {
    (void)vm;
    return id_list_piece(name, vm);
}

static hype_cfg_status_t apply_hype_field(hype_cfg_hype_t *h, unsigned int *seen, const char *key,
                                         char *val) {
    if (hype_streq(key, "config_version")) {
        hype_cfg_status_t st;
        if (*seen & H_CONFIG_VERSION) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_uint_field(val, &h->config_version);
        if (st != HYPE_CFG_OK) return st;
        *seen |= H_CONFIG_VERSION;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "host_cpu_budget")) {
        hype_cfg_status_t st;
        if (*seen & H_HOST_CPU_BUDGET) return HYPE_CFG_ERR_DUPLICATE_KEY;
        h->host_cpu_budget_count = 0;
        g_cpu_dst = h->host_cpu_budget;
        g_cpu_dst_count = &h->host_cpu_budget_count;
        /* Reuses cpu_set's own range/list grammar; the VM argument is unused by the destination. */
        st = for_each_comma_piece(val, (hype_cfg_vm_t *)0, cpu_set_piece);
        if (st != HYPE_CFG_OK) return st;
        h->has_host_cpu_budget = 1;
        *seen |= H_HOST_CPU_BUDGET;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "default_net_mode")) {
        if (*seen & H_DEFAULT_NET_MODE) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "none")) {
            h->default_net_mode = HYPE_CFG_NET_NONE;
        } else if (hype_streq(val, "nat")) {
            h->default_net_mode = HYPE_CFG_NET_NAT;
        } else {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        *seen |= H_DEFAULT_NET_MODE;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "dashboard_default_view")) {
        if (*seen & H_DASHBOARD_VIEW) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "dashboard")) {
            h->dashboard_default_view = HYPE_CFG_VIEW_DASHBOARD;
        } else if (hype_strneq(val, "vm:", 3)) {
            /* The VM named here need not exist yet -- sections may come later in the file, and
             * whether it exists at all is a cross-entity question for admission. */
            if (val[3] == '\0') return HYPE_CFG_ERR_BAD_VALUE;
            if (hype_strlcpy(h->dashboard_default_vm, val + 3, HYPE_CFG_NAME_MAX) >=
                HYPE_CFG_NAME_MAX) {
                return HYPE_CFG_ERR_VALUE_TOO_LONG;
            }
            h->dashboard_default_view = HYPE_CFG_VIEW_VM;
        } else {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        *seen |= H_DASHBOARD_VIEW;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "autostart")) {
        if (*seen & H_AUTOSTART) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "all")) {
            h->autostart = HYPE_CFG_AUTOSTART_ALL;
        } else if (hype_streq(val, "none")) {
            h->autostart = HYPE_CFG_AUTOSTART_NONE;
        } else {
            hype_cfg_status_t st;
            h->autostart_count = 0;
            g_list_dst = h->autostart_vms;
            g_list_count = &h->autostart_count;
            g_list_cap = HYPE_CFG_MAX_VMS;
            st = for_each_comma_piece(val, (hype_cfg_vm_t *)0, autostart_piece);
            if (st != HYPE_CFG_OK) return st;
            h->autostart = HYPE_CFG_AUTOSTART_LIST;
        }
        *seen |= H_AUTOSTART;
        return HYPE_CFG_OK;
    }
    return HYPE_CFG_ERR_UNKNOWN_KEY;
}

/* #222 (§5.3): per-[disk.*] duplicate-key tracking, independent of the [vm.*] flags above. */
enum {
    D_TYPE = 1u << 0,
    D_BACKING = 1u << 1,
    D_PATH = 1u << 2,
    D_FORMAT = 1u << 3,
    D_SIZE_GB = 1u << 4,
    D_ID_MATCH = 1u << 5,
    D_PARTITION = 1u << 6,
    D_BUS = 1u << 7,
    D_READ_ONLY = 1u << 8,
    D_ALLOW_OVERWRITE = 1u << 9,
    D_SOURCE_DISK = 1u << 10
};

static void zero_disk(hype_cfg_disk_t *d) {
    unsigned char *p = (unsigned char *)d;
    unsigned long long i;
    for (i = 0; i < sizeof(*d); i++) {
        p[i] = 0;
    }
    /* The zero values ARE the documented defaults: type=disk, backing=file, format=raw,
     * partition=0 (whole), bus=DEFAULT (resolved per os_hint, §5.6), read_only=false. */
}

static hype_cfg_status_t apply_disk_str(char *dst, int *has, unsigned int *seen, unsigned int bit,
                                        const char *val) {
    if (*seen & bit) return HYPE_CFG_ERR_DUPLICATE_KEY;
    if (hype_strlcpy(dst, val, HYPE_CFG_PATH_MAX) >= HYPE_CFG_PATH_MAX) {
        return HYPE_CFG_ERR_VALUE_TOO_LONG;
    }
    if (dst[0] == '\0') return HYPE_CFG_ERR_BAD_VALUE;
    *has = 1;
    *seen |= bit;
    return HYPE_CFG_OK;
}

static hype_cfg_status_t apply_disk_field(hype_cfg_disk_t *d, unsigned int *seen, const char *key,
                                          char *val) {
    if (hype_streq(key, "type")) {
        if (*seen & D_TYPE) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "disk")) {
            d->type = HYPE_CFG_DISK_TYPE_DISK;
        } else if (hype_streq(val, "cdrom")) {
            d->type = HYPE_CFG_DISK_TYPE_CDROM;
            /* §5.3: a cdrom is always read-only, and the operator does not get to say otherwise --
             * a writable ISO is not a thing, and honouring read_only=false would arm a write path
             * against installer media. */
            d->read_only = 1;
        } else {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        *seen |= D_TYPE;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "backing")) {
        if (*seen & D_BACKING) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "file")) {
            d->backing = HYPE_CFG_BACKING_FILE;
        } else if (hype_streq(val, "physical")) {
            d->backing = HYPE_CFG_BACKING_PHYSICAL;
        } else {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        d->has_backing = 1;
        *seen |= D_BACKING;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "path")) {
        return apply_disk_str(d->path, &d->has_path, seen, D_PATH, val);
    }
    if (hype_streq(key, "source_disk")) {
        return apply_disk_str(d->source_disk, &d->has_source_disk, seen, D_SOURCE_DISK, val);
    }
    if (hype_streq(key, "id_match")) {
        return apply_disk_str(d->id_match, &d->has_id_match, seen, D_ID_MATCH, val);
    }
    if (hype_streq(key, "format")) {
        if (*seen & D_FORMAT) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "raw")) {
            d->format = HYPE_CFG_FORMAT_RAW;
        } else if (hype_streq(val, "qcow2")) {
            d->format = HYPE_CFG_FORMAT_QCOW2;
        } else {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        *seen |= D_FORMAT;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "size_gb")) {
        hype_cfg_status_t st;
        if (*seen & D_SIZE_GB) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_uint_field(val, &d->size_gb);
        if (st != HYPE_CFG_OK) return st;
        d->has_size_gb = 1;
        *seen |= D_SIZE_GB;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "partition")) {
        if (*seen & D_PARTITION) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "whole")) {
            d->partition = 0u; /* 0 is the sentinel for whole-disk, matching target_disk */
        } else {
            hype_cfg_status_t st = parse_uint_field(val, &d->partition);
            if (st != HYPE_CFG_OK) return st; /* parse_uint_field already rejects 0 */
        }
        *seen |= D_PARTITION;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "bus")) {
        if (*seen & D_BUS) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "virtio-blk")) {
            d->bus = HYPE_CFG_BUS_VIRTIO_BLK;
        } else if (hype_streq(val, "ahci-sata")) {
            d->bus = HYPE_CFG_BUS_AHCI_SATA;
        } else if (hype_streq(val, "nvme")) {
            d->bus = HYPE_CFG_BUS_NVME;
        } else if (hype_streq(val, "ahci-atapi")) {
            d->bus = HYPE_CFG_BUS_AHCI_ATAPI;
        } else {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        *seen |= D_BUS;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "read_only")) {
        hype_cfg_status_t st;
        int v = 0;
        if (*seen & D_READ_ONLY) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_bool_field(val, &v);
        if (st != HYPE_CFG_OK) return st;
        /* A cdrom stays read-only whatever this says (see `type` above); anything else applies. */
        if (d->type != HYPE_CFG_DISK_TYPE_CDROM) {
            d->read_only = v;
        }
        d->has_read_only = 1;
        *seen |= D_READ_ONLY;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "allow_overwrite")) {
        hype_cfg_status_t st;
        if (*seen & D_ALLOW_OVERWRITE) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_bool_field(val, &d->allow_overwrite);
        if (st != HYPE_CFG_OK) return st;
        *seen |= D_ALLOW_OVERWRITE;
        return HYPE_CFG_OK;
    }
    return HYPE_CFG_ERR_UNKNOWN_KEY;
}

/*
 * §5.3 required fields, checked when the section ends. Returns OK or the reason the device is
 * unusable -- the caller SKIPS it rather than failing the whole parse (§4.3).
 */
static hype_cfg_status_t validate_disk(const hype_cfg_disk_t *d, unsigned int seen) {
    if (d->type == HYPE_CFG_DISK_TYPE_CDROM) {
        /* backing defaults to file for a cdrom, so only the path is genuinely required. */
        if (!d->has_path) return HYPE_CFG_ERR_MISSING_REQUIRED;
        return HYPE_CFG_OK;
    }
    /* A hard disk must say where its bytes come from: `backing` is required rather than defaulted,
     * because guessing between a file and a REAL DRIVE is not a guess worth making. */
    if (!(seen & D_BACKING)) return HYPE_CFG_ERR_MISSING_REQUIRED;
    if (d->backing == HYPE_CFG_BACKING_FILE && !d->has_path) return HYPE_CFG_ERR_MISSING_REQUIRED;
    if (d->backing == HYPE_CFG_BACKING_PHYSICAL) {
        /* Without id_match the phys_guard (#122/#124) has nothing to match the enumerated drive
         * against, so the write could never be armed anyway -- and a physical target that silently
         * matched "any drive" is the worst possible reading of an omission. */
        if (!d->has_id_match) return HYPE_CFG_ERR_MISSING_REQUIRED;
        if (d->has_path) return HYPE_CFG_ERR_BAD_VALUE; /* path is meaningless for a real drive */
    }
    return HYPE_CFG_OK;
}

/*
 * #222: retain a line verbatim, attached to the section it appeared in.
 *
 * Capacity exhaustion sets retained_overflow rather than failing the parse: a config must not become
 * unbootable because it carries one comment too many. The flag is what stops the CONFIG-3 serializer
 * writing back a file whose tail it never captured.
 */
static void retain_line(hype_cfg_t *out, const char *raw, int raw_truncated) {
    unsigned long long len;

    if (raw_truncated) {
        /* The caller's snapshot already lost bytes, so what is here is a PREFIX of the operator's
         * line. Writing a prefix back would corrupt the file, so keep none of it -- and flag it. */
        out->retained_overflow = 1;
        return;
    }
    if (out->retained_count >= HYPE_CFG_MAX_RETAINED) {
        out->retained_overflow = 1;
        return;
    }
    len = hype_strlcpy(out->retained[out->retained_count].text, raw, HYPE_CFG_LINE_MAX);
    if (len >= HYPE_CFG_LINE_MAX) {
        out->retained_overflow = 1;
        return;
    }
    out->retained[out->retained_count].section = (int)out->section_count - 1;
    out->retained_count++;
}

/* Records a section header in file order. Returns the new section index, or -1 if full. */
static int add_section(hype_cfg_t *out, hype_cfg_section_kind_t kind, const char *name,
                       const char *raw, int index) {
    hype_cfg_section_t *sec;

    if (out->section_count >= HYPE_CFG_MAX_SECTIONS) {
        out->retained_overflow = 1;
        return -1;
    }
    sec = &out->sections[out->section_count];
    sec->kind = kind;
    sec->index = index;
    if (hype_strlcpy(sec->name, name, HYPE_CFG_NAME_MAX) >= HYPE_CFG_NAME_MAX ||
        hype_strlcpy(sec->raw, raw, HYPE_CFG_LINE_MAX) >= HYPE_CFG_LINE_MAX) {
        out->retained_overflow = 1;
    }
    out->section_count++;
    return (int)out->section_count - 1;
}

static hype_cfg_status_t process_section_header(char *line, hype_cfg_t *out, int *cur,
                                                 unsigned int *seen, const char *raw, int *cur_disk,
                                                 unsigned int *disk_seen, int *cur_is_hype,
                                                 unsigned int *in_hype_seen) {
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
    if (hype_streq(body, "hype")) {
        /* §5.1: the master section has no instance name. A second one is a duplicate, and silently
         * merging two would make which value wins depend on file order. */
        if (*in_hype_seen != 0u || out->hype.malformed) {
            return HYPE_CFG_ERR_DUPLICATE_KEY;
        }
        *cur = -1;
        *cur_disk = -1;
        *cur_is_hype = 1;
        (void)add_section(out, HYPE_CFG_SECTION_HYPE, "", raw, -1);
        return HYPE_CFG_OK;
    }
    if (hype_strneq(body, "disk.", 5)) {
        /* #222 (§5.3): a named device. Same shape as a VM section: the id must be non-empty and
         * unique, since `disks = a, b` resolves by id and a duplicate would make which device is
         * meant depend on parse order. */
        name = body + 5;
        if (*name == '\0') {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        for (i = 0; i < out->disk_count; i++) {
            if (hype_streq(out->disks[i].id, name)) {
                return HYPE_CFG_ERR_DUPLICATE_VM_NAME; /* same class of error: a duplicated id */
            }
        }
        if (out->disk_count >= HYPE_CFG_MAX_DISKS) {
            return HYPE_CFG_ERR_TOO_MANY_ENTRIES;
        }
        *cur = -1; /* not a VM: VM keys must not land in a disk section */
        *cur_is_hype = 0;
        *cur_disk = (int)out->disk_count;
        zero_disk(&out->disks[*cur_disk]);
        if (hype_strlcpy(out->disks[*cur_disk].id, name, HYPE_CFG_NAME_MAX) >= HYPE_CFG_NAME_MAX) {
            return HYPE_CFG_ERR_VALUE_TOO_LONG;
        }
        *disk_seen = 0;
        (void)add_section(out, HYPE_CFG_SECTION_DISK, name, raw, *cur_disk);
        out->disk_count++;
        return HYPE_CFG_OK;
    }
    if (!hype_strneq(body, "vm.", 3)) {
        /*
         * #222 (spec §4.1): an unknown section KIND is retained, not rejected. A `[disk.*]` or
         * `[nic.*]` section from a newer hype must not stop an older one booting, and the lines
         * inside it must survive a write-back. Its keys are retained too -- see process_key_value's
         * cur < 0 path, which is why `cur` is cleared here.
         */
        *cur = -1;
        *cur_disk = -1;
        *cur_is_hype = 0;
        (void)add_section(out, HYPE_CFG_SECTION_UNKNOWN, "", raw, -1);
        out->unknown_count++;
        return HYPE_CFG_OK;
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
    *cur_disk = -1;
    *cur_is_hype = 0;
    zero_vm(&out->vms[*cur]);
    name_len = hype_strlcpy(out->vms[*cur].name, name, HYPE_CFG_NAME_MAX);
    if (name_len >= HYPE_CFG_NAME_MAX) {
        return HYPE_CFG_ERR_VALUE_TOO_LONG;
    }
    seen[*cur] = 0;
    (void)add_section(out, HYPE_CFG_SECTION_VM, name, raw, *cur);
    out->vm_count++;
    return HYPE_CFG_OK;
}

static hype_cfg_status_t process_key_value(char *line, hype_cfg_t *out, int cur,
                                            unsigned int *seen, const char *raw, int raw_truncated,
                                            int cur_disk, unsigned int *disk_seen, int cur_is_hype,
                                            unsigned int *in_hype_seen) {
    char *eq;
    char *key;
    char *val;
    hype_cfg_status_t st;

    if (cur < 0 && cur_disk < 0 && !cur_is_hype) {
        /*
         * #222: no CURRENT typed section. Two different cases, deliberately distinguished: inside a
         * retained unknown section the line is retained (section_count > 0), whereas a key before
         * ANY section header is still a hard error -- it belongs to nothing and is far more likely a
         * typo'd header than a forward-compatible extension.
         */
        if (out->section_count > 0u &&
            out->sections[out->section_count - 1u].kind == HYPE_CFG_SECTION_UNKNOWN) {
            retain_line(out, raw, raw_truncated);
            out->unknown_count++;
            return HYPE_CFG_OK;
        }
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

    if (cur_is_hype) {
        st = apply_hype_field(&out->hype, in_hype_seen, key, val);
        if (st != HYPE_CFG_OK && st != HYPE_CFG_ERR_UNKNOWN_KEY) {
            /*
             * §4.3: a malformed [hype] falls back to global DEFAULTS instead of failing the parse.
             * Deliberately a different rule from [vm.*]: a bad global cannot make a VM wrong, it can
             * only make hype behave as it did before the section existed. The flag is what stops
             * that being silent -- a rejected `host_cpu_budget` that just vanished would look like
             * it had been applied.
             */
            hype_globals_defaults(&out->hype);
            out->hype.malformed = 1;
            *in_hype_seen = 0;
            return HYPE_CFG_OK;
        }
    } else {
        st = (cur_disk >= 0) ? apply_disk_field(&out->disks[cur_disk], disk_seen, key, val)
                             : apply_field(&out->vms[cur], &seen[cur], key, val);
    }
    if (st == HYPE_CFG_ERR_UNKNOWN_KEY) {
        /* #222: retained, not rejected -- the whole point of the keystone. Note `line` has been
         * mutated (the '=' overwritten), which is why retention works off the untouched `raw`. */
        retain_line(out, raw, raw_truncated);
        out->unknown_count++;
        return HYPE_CFG_OK;
    }
    return st;
}

hype_cfg_result_t hype_cfg_parse(char *text, hype_cfg_t *out) {
    hype_cfg_result_t res;
    unsigned int seen[HYPE_CFG_MAX_VMS];
    unsigned int disk_seen = 0;
    unsigned int in_hype_seen = 0;
    int cur = -1;
    int cur_disk = -1;
    int cur_is_hype = 0;
    unsigned int line_no = 0;
    char *p = text;
    unsigned int i;

    res.status = HYPE_CFG_OK;
    res.line = 0;
    out->vm_count = 0;
    hype_globals_defaults(&out->hype);
    out->disk_count = 0;
    out->skipped_disks = 0;
    out->section_count = 0;
    out->retained_count = 0;
    out->retained_overflow = 0;
    out->unknown_count = 0;
    for (i = 0; i < HYPE_CFG_MAX_VMS; i++) {
        seen[i] = 0;
    }

    while (*p) {
        char *line_start = p;
        char *line;
        char raw[HYPE_CFG_LINE_MAX];
        int raw_truncated;
        hype_cfg_status_t st;

        while (*p && *p != '\n') {
            p++;
        }
        if (*p == '\n') {
            *p = '\0';
            p++;
        }
        line_no++;

        /* #222: snapshot the line BEFORE clean_line() strips its comment and trims it in place.
         * Retention has to re-emit what the operator wrote, comment included, so it cannot work off
         * the cleaned form. Truncation MUST be detected here rather than inside retain_line(): by
         * then the bytes are already gone and the prefix looks like a complete short line. */
        raw_truncated = hype_strlcpy(raw, line_start, HYPE_CFG_LINE_MAX) >= HYPE_CFG_LINE_MAX;

        line = clean_line(line_start);
        if (*line == '\0') {
            /* Comment-only lines are retained (a serializer must preserve them); pure blanks are
             * not -- they carry nothing a re-emitted file needs, and capacity is finite. */
            if (raw[0] != '\0') {
                retain_line(out, raw, raw_truncated);
            }
            continue;
        }

        if (line[0] == '[') {
            st = process_section_header(line, out, &cur, seen, raw, &cur_disk, &disk_seen,
                                        &cur_is_hype, &in_hype_seen);
        } else {
            st = process_key_value(line, out, cur, seen, raw, raw_truncated, cur_disk,
                                   &disk_seen, cur_is_hype, &in_hype_seen);
        }

        if (st != HYPE_CFG_OK) {
            res.status = st;
            res.line = line_no;
            return res;
        }
    }

    /*
     * §4.3: a malformed [disk.*] is dropped, not fatal. Compacted in place so disks[] stays dense
     * and `disk_count` means what it says -- a caller iterating 0..disk_count must not have to know
     * some entries are corpses. Dropping loses which id was bad, which is why skipped_disks exists
     * for the caller to report: a device that vanishes silently is how a VM ends up with no boot
     * disk for no visible reason.
     *
     * disk_seen only tracks the LAST disk section's keys, so required-field validation can only be
     * done for that one; every earlier disk is validated on the fields it actually set. That is
     * enough: the required-field checks read has_* flags, and only D_BACKING needs the seen mask
     * (has_backing carries the same information).
     */
    {
        int remap[HYPE_CFG_MAX_DISKS];
        unsigned int w = 0;
        unsigned int si;

        /*
         * Two passes with an explicit remap, NOT an index comparison after the fact. The obvious
         * version -- retarget moved entries, then clear any section index >= the new count -- is
         * wrong: with disks[0] bad and disks[1] good, the good one moves to 0 and the BAD section's
         * stale index 0 is still in range, so two headers end up pointing at the same device.
         */
        for (i = 0; i < out->disk_count; i++) {
            unsigned int sm = out->disks[i].has_backing ? D_BACKING : 0u;
            if (validate_disk(&out->disks[i], sm) != HYPE_CFG_OK) {
                remap[i] = -1;
                out->skipped_disks++;
                continue;
            }
            remap[i] = (int)w;
            if (w != i) {
                /* Byte copy, not `out->disks[w] = out->disks[i]`: freestanding hype has no libc, and
                 * whole-struct assignment of anything containing an array emits a memcpy call that
                 * fails at EFI link. Only `make all` catches it -- the host test build links fine. */
                unsigned char *dst = (unsigned char *)&out->disks[w];
                const unsigned char *src = (const unsigned char *)&out->disks[i];
                unsigned long long b;
                for (b = 0; b < sizeof(out->disks[0]); b++) {
                    dst[b] = src[b];
                }
            }
            w++;
        }
        for (si = 0; si < out->section_count; si++) {
            if (out->sections[si].kind == HYPE_CFG_SECTION_DISK && out->sections[si].index >= 0) {
                out->sections[si].index = remap[out->sections[si].index];
            }
        }
        out->disk_count = w;
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

hype_cfg_ram_status_t hype_cfg_resolve_mem_mb(unsigned int cfg_mem_mb, unsigned int default_mb,
                                              unsigned int min_mb, unsigned int max_mb,
                                              unsigned int *out_mb) {
    unsigned int applied;
    hype_cfg_ram_status_t st;

    /* Inverted limits: the floor wins. A guest that is too small fails loudly at
     * its own boot; one sized past the platform's ceiling corrupts whatever lives
     * above it, which is far harder to attribute. */
    if (max_mb < min_mb) {
        max_mb = min_mb;
    }

    if (cfg_mem_mb == 0u) {
        applied = default_mb;
        st = HYPE_CFG_RAM_DEFAULTED;
    } else if (cfg_mem_mb < min_mb) {
        applied = min_mb;
        st = HYPE_CFG_RAM_CLAMPED_LOW;
    } else if (cfg_mem_mb > max_mb) {
        applied = max_mb;
        st = HYPE_CFG_RAM_CLAMPED_HIGH;
    } else {
        applied = cfg_mem_mb;
        st = HYPE_CFG_RAM_APPLIED;
    }

    /* The DEFAULT is clamped too. A built-in default that exceeds the platform
     * ceiling is still an overrun, and the compile-time override
     * (-DHYPE_FW_1_GUEST_RAM_MB=N) can set it to anything. */
    if (applied < min_mb) {
        applied = min_mb;
    }
    if (applied > max_mb) {
        applied = max_mb;
    }

    if (out_mb != (unsigned int *)0) {
        *out_mb = applied;
    }
    return st;
}

const char *hype_cfg_ram_status_str(hype_cfg_ram_status_t st) {
    switch (st) {
        case HYPE_CFG_RAM_DEFAULTED: return "built-in default (no hype.cfg value)";
        case HYPE_CFG_RAM_APPLIED: return "from hype.cfg";
        case HYPE_CFG_RAM_CLAMPED_LOW: return "hype.cfg value RAISED to the minimum";
        case HYPE_CFG_RAM_CLAMPED_HIGH: return "hype.cfg value LOWERED to the maximum";
        default: return "unknown";
    }
}

uint64_t hype_cfg_size_gb_to_bytes(unsigned int gb) {
    /* unsigned int caps at ~4.3e9, so 4.3e9 * 2^30 stays inside 64 bits -- no overflow guard
     * needed, and one would be dead code. */
    return (uint64_t)gb * 1024ull * 1024ull * 1024ull;
}

hype_cfg_bus_t hype_cfg_resolve_bus(const hype_cfg_disk_t *disk, hype_cfg_os_hint_t os_hint) {
    if (disk == 0) {
        return HYPE_CFG_BUS_DEFAULT;
    }
    /* An optical drive is ATAPI whatever anything else says: it is the only optical front-end hype
     * has, and a cdrom on virtio-blk is not a thing a guest would recognise. */
    if (disk->type == HYPE_CFG_DISK_TYPE_CDROM) {
        return HYPE_CFG_BUS_AHCI_ATAPI;
    }
    if (disk->bus != HYPE_CFG_BUS_DEFAULT) {
        return disk->bus; /* explicit always wins */
    }
    if (os_hint == HYPE_CFG_OS_WINDOWS) {
        return HYPE_CFG_BUS_AHCI_SATA;
    }
    return HYPE_CFG_BUS_VIRTIO_BLK;
}
