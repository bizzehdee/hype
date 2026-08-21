#include <stdio.h>
#include <string.h>
#include "../l2switch.h"

static int failures = 0;

#define CHECK(msg, cond) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; } \
} while (0)

static const uint8_t MAC_A[6] = {0x52, 0x54, 0x00, 0x00, 0x00, 0x0A};
static const uint8_t MAC_B[6] = {0x52, 0x54, 0x00, 0x00, 0x00, 0x0B};
static const uint8_t MAC_C[6] = {0x52, 0x54, 0x00, 0x00, 0x00, 0x0C};
static const uint8_t MAC_BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* A minimal Ethernet frame: dst(6) src(6) type(2) + a little payload. */
static unsigned int mkframe(uint8_t *out, const uint8_t *dst, const uint8_t *src) {
    memcpy(out, dst, 6);
    memcpy(out + 6, src, 6);
    out[12] = 0x08;
    out[13] = 0x00;
    memset(out + 14, 0x5A, 32);
    return 46u;
}

static void sw3(hype_l2switch_t *sw, int nat) {
    hype_l2sw_init(sw, nat);
    CHECK("member 0", hype_l2sw_add_member(sw, 4) == 0); /* vm4 */
    CHECK("member 1", hype_l2sw_add_member(sw, 7) == 1); /* vm7 */
    CHECK("member 2", hype_l2sw_add_member(sw, 9) == 2); /* vm9 */
}

static void test_broadcast_floods_to_all_but_sender(void) {
    hype_l2switch_t sw;
    uint8_t f[64];
    hype_l2sw_verdict_t v;

    sw3(&sw, 0);
    v = hype_l2sw_classify(&sw, 0, f, mkframe(f, MAC_BCAST, MAC_A));
    CHECK("flood mask = members 1+2", v.deliver_mask == 0x6u);
    CHECK("no uplink on a private switch", v.to_uplink == 0);
}

static void test_learned_unicast_goes_to_exactly_one_member(void) {
    hype_l2switch_t sw;
    uint8_t f[64];
    hype_l2sw_verdict_t v;

    sw3(&sw, 0);
    /* B (slot 1) sends anything -- its MAC is learned. */
    (void)hype_l2sw_classify(&sw, 1, f, mkframe(f, MAC_BCAST, MAC_B));
    /* A (slot 0) then unicasts to B: exactly slot 1, no flood. */
    v = hype_l2sw_classify(&sw, 0, f, mkframe(f, MAC_B, MAC_A));
    CHECK("unicast hits only member 1", v.deliver_mask == 0x2u);
    CHECK("table hit does not offer uplink", v.to_uplink == 0);
}

static void test_unknown_unicast_floods(void) {
    hype_l2switch_t sw;
    uint8_t f[64];
    hype_l2sw_verdict_t v;

    sw3(&sw, 2 /* truthy */);
    v = hype_l2sw_classify(&sw, 2, f, mkframe(f, MAC_C, MAC_A));
    CHECK("unknown unicast floods to 0+1", v.deliver_mask == 0x3u);
    CHECK("nat switch offers the uplink", v.to_uplink == 1);
}

static void test_mac_move_relearns(void) {
    hype_l2switch_t sw;
    uint8_t f[64];
    hype_l2sw_verdict_t v;

    sw3(&sw, 0);
    (void)hype_l2sw_classify(&sw, 0, f, mkframe(f, MAC_BCAST, MAC_A)); /* A at slot 0 */
    (void)hype_l2sw_classify(&sw, 2, f, mkframe(f, MAC_BCAST, MAC_A)); /* A moves to slot 2 */
    CHECK("move counted", sw.moved == 1u);
    v = hype_l2sw_classify(&sw, 1, f, mkframe(f, MAC_A, MAC_B));
    CHECK("traffic follows the move", v.deliver_mask == 0x4u);
}

static void test_sender_never_receives_own_frame(void) {
    hype_l2switch_t sw;
    uint8_t f[64];
    hype_l2sw_verdict_t v;

    sw3(&sw, 1);
    (void)hype_l2sw_classify(&sw, 0, f, mkframe(f, MAC_BCAST, MAC_A));
    /* A unicasts to its own MAC: nothing is delivered (no hairpin). */
    v = hype_l2sw_classify(&sw, 0, f, mkframe(f, MAC_A, MAC_A));
    CHECK("no hairpin", v.deliver_mask == 0u);
}

static void test_multicast_source_is_never_learned(void) {
    hype_l2switch_t sw;
    uint8_t f[64];
    hype_l2sw_verdict_t v;

    sw3(&sw, 0);
    /* A hostile guest claims the broadcast address as its source. */
    (void)hype_l2sw_classify(&sw, 2, f, mkframe(f, MAC_BCAST, MAC_BCAST));
    CHECK("nothing learned from a multicast source", sw.learned == 0u);
    /* Frames to broadcast still flood rather than unicasting to the poisoner. */
    v = hype_l2sw_classify(&sw, 0, f, mkframe(f, MAC_BCAST, MAC_A));
    CHECK("broadcast still floods", v.deliver_mask == 0x6u);
}

static void test_short_frame_and_bad_slot_yield_empty_verdict(void) {
    hype_l2switch_t sw;
    uint8_t f[64];
    hype_l2sw_verdict_t v;

    sw3(&sw, 1);
    mkframe(f, MAC_B, MAC_A);
    v = hype_l2sw_classify(&sw, 0, f, 13u);
    CHECK("short frame delivers nowhere", v.deliver_mask == 0u && v.to_uplink == 0);
    v = hype_l2sw_classify(&sw, 5u, f, 46u);
    CHECK("bad slot delivers nowhere", v.deliver_mask == 0u);
    v = hype_l2sw_classify(&sw, 0, 0, 46u);
    CHECK("null frame delivers nowhere", v.deliver_mask == 0u);
}

static void test_table_eviction_keeps_working(void) {
    hype_l2switch_t sw;
    uint8_t f[64];
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, 0};
    unsigned int i;
    hype_l2sw_verdict_t v;

    sw3(&sw, 0);
    /* Learn table-size + 4 distinct MACs from slot 1; the table must cycle, not wedge. */
    for (i = 0; i < HYPE_L2SW_MAC_TABLE + 4u; i++) {
        mac[5] = (uint8_t)i;
        mac[4] = (uint8_t)(i >> 8);
        (void)hype_l2sw_classify(&sw, 1, f, mkframe(f, MAC_BCAST, mac));
    }
    /* The most recent MAC must be resident. */
    mac[5] = (uint8_t)(HYPE_L2SW_MAC_TABLE + 3u);
    mac[4] = 0;
    v = hype_l2sw_classify(&sw, 0, f, mkframe(f, mac, MAC_A));
    CHECK("recently learned MAC forwards after eviction", v.deliver_mask == 0x2u);
}

static void test_member_slots(void) {
    hype_l2switch_t sw;
    unsigned int i;
    hype_l2sw_init(&sw, 0);
    for (i = 0; i < HYPE_L2SW_MAX_MEMBERS; i++) {
        CHECK("add member", hype_l2sw_add_member(&sw, 100u + i) == (int)i);
    }
    CHECK("full switch refuses", hype_l2sw_add_member(&sw, 999u) == -1);
    CHECK("slot lookup", hype_l2sw_member_slot(&sw, 107u) == 7);
    CHECK("non-member lookup", hype_l2sw_member_slot(&sw, 42u) == -1);
}

int main(void) {
    test_broadcast_floods_to_all_but_sender();
    test_learned_unicast_goes_to_exactly_one_member();
    test_unknown_unicast_floods();
    test_mac_move_relearns();
    test_sender_never_receives_own_frame();
    test_multicast_source_is_never_learned();
    test_short_frame_and_bad_slot_yield_empty_verdict();
    test_table_eviction_keeps_working();
    test_member_slots();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
