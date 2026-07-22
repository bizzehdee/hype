#include <stdio.h>
#include "../../arch/x86_64/cpu/host_input.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* Feed one scancode; report produced byte count + first byte + chord action. */
static hype_chord_action_t feed(hype_host_input_t *hi, uint8_t sc, unsigned *n, uint8_t *first) {
    uint8_t out[HYPE_KBD_DECODE_MAX_OUT];
    hype_chord_result_t r = hype_host_input_feed(hi, sc, out, sizeof(out), n);
    *first = (*n > 0) ? out[0] : 0;
    return r.action;
}

/* Press the leader (Right-Ctrl + Right-Alt), each as an E0-prefixed make. */
static void press_leader(hype_host_input_t *hi) {
    unsigned n; uint8_t b;
    feed(hi, 0xE0, &n, &b); feed(hi, 0x1D, &n, &b); /* Right-Ctrl make */
    feed(hi, 0xE0, &n, &b); feed(hi, 0x38, &n, &b); /* Right-Alt  make */
}
static void release_leader(hype_host_input_t *hi) {
    unsigned n; uint8_t b;
    feed(hi, 0xE0, &n, &b); feed(hi, 0x9D, &n, &b); /* Right-Ctrl break */
    feed(hi, 0xE0, &n, &b); feed(hi, 0xB8, &n, &b); /* Right-Alt  break */
}

int main(void) {
    hype_host_input_t hi;
    unsigned n; uint8_t b;
    hype_chord_action_t a;

    /* --- ordinary typing routes to the guest --- */
    hype_host_input_reset(&hi);
    a = feed(&hi, 0x1E, &n, &b); /* 'a' */
    CHECK_HEX("'a' no chord", HYPE_CHORD_ACTION_NONE, a);
    CHECK_HEX("'a' -> 1 byte", 1u, n);
    CHECK_HEX("'a' -> 'a'", 'a', b);
    /* up arrow (no leader) -> ESC [ A (3 bytes) */
    feed(&hi, 0xE0, &n, &b);
    a = feed(&hi, 0x48, &n, &b);
    CHECK_HEX("up-arrow no chord", HYPE_CHORD_ACTION_NONE, a);
    CHECK_HEX("up-arrow -> 3 bytes", 3u, n);
    CHECK_HEX("up-arrow first = ESC", 0x1B, b);

    /* --- leader + D = toggle dashboard, swallowed (no guest byte) --- */
    hype_host_input_reset(&hi);
    press_leader(&hi);
    a = feed(&hi, 0x20, &n, &b); /* 'D' */
    CHECK_HEX("leader+D -> TOGGLE_DASHBOARD", HYPE_CHORD_ACTION_TOGGLE_DASHBOARD, a);
    CHECK_HEX("leader+D swallowed (0 guest bytes)", 0u, n);

    /* --- leader + '1' = jump to VM 1 --- */
    hype_host_input_reset(&hi);
    press_leader(&hi);
    {
        uint8_t out[HYPE_KBD_DECODE_MAX_OUT];
        hype_chord_result_t r = hype_host_input_feed(&hi, 0x02, out, sizeof(out), &n); /* '1' */
        CHECK_HEX("leader+1 -> JUMP_TO_VM", HYPE_CHORD_ACTION_JUMP_TO_VM, r.action);
        CHECK_HEX("leader+1 vm_index=1", 1u, r.vm_index);
        CHECK_HEX("leader+1 swallowed", 0u, n);
    }

    /* --- leader + Left arrow = CYCLE_PREV, NOT a guest arrow --- */
    hype_host_input_reset(&hi);
    press_leader(&hi);
    feed(&hi, 0xE0, &n, &b);
    a = feed(&hi, 0x4B, &n, &b); /* Left */
    CHECK_HEX("leader+Left -> CYCLE_PREV", HYPE_CHORD_ACTION_CYCLE_PREV, a);
    CHECK_HEX("leader+Left swallowed (not a guest arrow)", 0u, n);

    /* --- after releasing the leader, typing routes to the guest again (and the
     *     swallowed E0 sequences didn't desync the decoder) --- */
    release_leader(&hi);
    a = feed(&hi, 0x1E, &n, &b); /* 'a' */
    CHECK_HEX("post-leader 'a' no chord", HYPE_CHORD_ACTION_NONE, a);
    CHECK_HEX("post-leader 'a' -> 1 byte", 1u, n);
    CHECK_HEX("post-leader 'a' -> 'a' (no desync)", 'a', b);

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
