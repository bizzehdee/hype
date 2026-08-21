#include "vm_delete.h"

/* Freestanding: no libc. Same tiny helpers every core module carries. */
static int vmd_streq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static unsigned vmd_cat(char *out, unsigned pos, unsigned cap, const char *s) {
    while (*s != '\0' && pos + 1u < cap) {
        out[pos++] = *s++;
    }
    out[pos] = '\0';
    return pos;
}

static void vmd_copy(char *dst, unsigned cap, const char *src) {
    unsigned i = 0;
    if (src != 0) {
        while (src[i] != '\0' && i + 1u < cap) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

void hype_vm_delete_begin(hype_vm_delete_t *d, const char *vm_name, const char *disks_note) {
    d->step = HYPE_VMD_CONFIRM1;
    vmd_copy(d->vm_name, sizeof(d->vm_name), vm_name);
    vmd_copy(d->disks_note, sizeof(d->disks_note), disks_note);
    d->error[0] = '\0';
    d->have_error = 0;
}

hype_vmd_step_t hype_vm_delete_feed(hype_vm_delete_t *d, const char *line) {
    if (d == 0) {
        return HYPE_VMD_ABORTED;
    }
    if (d->step == HYPE_VMD_CONFIRMED || d->step == HYPE_VMD_ABORTED) {
        return d->step; /* terminal states are inert */
    }
    d->error[0] = '\0';
    d->have_error = 0;
    if (line == 0) {
        line = "";
    }
    if (d->step == HYPE_VMD_CONFIRM1) {
        if (vmd_streq(line, "yes")) {
            d->step = HYPE_VMD_CONFIRM2;
        } else if (vmd_streq(line, "no")) {
            d->step = HYPE_VMD_ABORTED;
        } else {
            /* An empty line or anything else neither confirms nor aborts: both are decisions the
             * operator has not made, and a destructive flow must not guess either way. */
            vmd_copy(d->error, sizeof(d->error), "answer yes or no");
            d->have_error = 1;
        }
        return d->step;
    }
    /* HYPE_VMD_CONFIRM2 */
    if (vmd_streq(line, "delete")) {
        d->step = HYPE_VMD_CONFIRMED;
    } else if (vmd_streq(line, "cancel")) {
        d->step = HYPE_VMD_ABORTED;
    } else {
        /* `yes` is DELIBERATELY not accepted here -- the second gate exists so that a habitual
         * yes cannot complete a deletion. */
        vmd_copy(d->error, sizeof(d->error), "type delete to proceed, or cancel");
        d->have_error = 1;
    }
    return d->step;
}

const char *hype_vm_delete_render(const hype_vm_delete_t *d, char *out, unsigned cap) {
    unsigned pos = 0;

    if (out == 0 || cap == 0u) {
        return "";
    }
    out[0] = '\0';
    if (d == 0) {
        return out;
    }
    if (d->step == HYPE_VMD_CONFIRMED) {
        (void)vmd_cat(out, 0, cap, "delete: confirmed");
        return out;
    }
    if (d->step == HYPE_VMD_ABORTED) {
        (void)vmd_cat(out, 0, cap, "delete: cancelled -- nothing changed");
        return out;
    }
    if (d->have_error) {
        pos = vmd_cat(out, pos, cap, "INVALID -- ");
        pos = vmd_cat(out, pos, cap, d->error);
        pos = vmd_cat(out, pos, cap, "\n");
    }
    pos = vmd_cat(out, pos, cap, "delete> ");
    if (d->step == HYPE_VMD_CONFIRM1) {
        pos = vmd_cat(out, pos, cap, "delete '");
        pos = vmd_cat(out, pos, cap, d->vm_name);
        (void)vmd_cat(out, pos, cap, "' -- are you sure? (yes|no)");
    } else {
        pos = vmd_cat(out, pos, cap, "this will delete '");
        pos = vmd_cat(out, pos, cap, d->vm_name);
        pos = vmd_cat(out, pos, cap, "' from hype.cfg. This cannot be undone. Backing disks stay "
                                     "in place: ");
        pos = vmd_cat(out, pos, cap, d->disks_note[0] != '\0' ? d->disks_note : "none");
        (void)vmd_cat(out, pos, cap, "  (delete|cancel)");
    }
    return out;
}
