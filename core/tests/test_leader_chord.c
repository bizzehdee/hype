#include <stdio.h>
#include "../../arch/x86_64/cpu/leader_chord.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void feed_extended(hype_chord_state_t *state, uint8_t byte) {
    hype_chord_feed_scancode(state, HYPE_SCANCODE_EXTENDED_PREFIX);
    hype_chord_feed_scancode(state, byte);
}

static void hold_both_modifiers(hype_chord_state_t *state) {
    feed_extended(state, HYPE_SCANCODE_RIGHT_CTRL_MAKE);
    feed_extended(state, HYPE_SCANCODE_RIGHT_ALT_MAKE);
}

static void test_reset_clears_held_state(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    CHECK_HEX("right_ctrl_held starts clear", 0, state.right_ctrl_held);
    CHECK_HEX("right_alt_held starts clear", 0, state.right_alt_held);
    CHECK_HEX("pending_extended starts clear", 0, state.pending_extended);

    hold_both_modifiers(&state);
    hype_chord_state_reset(&state);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_D_MAKE);
    CHECK_HEX("D alone after reset produces no action", HYPE_CHORD_ACTION_NONE, result.action);
}

static void test_extended_prefix_alone_produces_no_action(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_EXTENDED_PREFIX);
    CHECK_HEX("bare 0xE0 produces no action", HYPE_CHORD_ACTION_NONE, result.action);
}

static void test_d_without_both_modifiers_is_ignored(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_D_MAKE);
    CHECK_HEX("D with no modifiers held", HYPE_CHORD_ACTION_NONE, result.action);

    hype_chord_state_reset(&state);
    feed_extended(&state, HYPE_SCANCODE_RIGHT_CTRL_MAKE);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_D_MAKE);
    CHECK_HEX("D with only right-ctrl held", HYPE_CHORD_ACTION_NONE, result.action);

    hype_chord_state_reset(&state);
    feed_extended(&state, HYPE_SCANCODE_RIGHT_ALT_MAKE);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_D_MAKE);
    CHECK_HEX("D with only right-alt held", HYPE_CHORD_ACTION_NONE, result.action);
}

static void test_toggle_dashboard(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    hold_both_modifiers(&state);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_D_MAKE);
    CHECK_HEX("Right-Ctrl+Right-Alt+D toggles dashboard",
              HYPE_CHORD_ACTION_TOGGLE_DASHBOARD, result.action);
}

static void test_return_to_dashboard_esc(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    hold_both_modifiers(&state);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_ESC_MAKE);
    CHECK_HEX("Right-Ctrl+Right-Alt+Esc returns to dashboard",
              HYPE_CHORD_ACTION_RETURN_TO_DASHBOARD, result.action);
}

static void test_jump_to_vm_1_through_9(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;
    uint8_t digit_scancode;
    uint8_t expected_index;

    for (digit_scancode = HYPE_SCANCODE_1_MAKE; digit_scancode <= HYPE_SCANCODE_9_MAKE; digit_scancode++) {
        expected_index = (uint8_t)(digit_scancode - HYPE_SCANCODE_1_MAKE + 1u);
        hype_chord_state_reset(&state);
        hold_both_modifiers(&state);
        result = hype_chord_feed_scancode(&state, digit_scancode);
        CHECK_HEX("digit key jumps to VM", HYPE_CHORD_ACTION_JUMP_TO_VM, result.action);
        CHECK_HEX("jump targets the right VM index", expected_index, result.vm_index);
    }
}

static void test_cycle_prev_next_arrows(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    hold_both_modifiers(&state);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_EXTENDED_PREFIX);
    CHECK_HEX("extended prefix before arrow produces no action yet",
              HYPE_CHORD_ACTION_NONE, result.action);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_LEFT_ARROW_MAKE);
    CHECK_HEX("Right-Ctrl+Right-Alt+Left cycles to previous VM",
              HYPE_CHORD_ACTION_CYCLE_PREV, result.action);

    hype_chord_state_reset(&state);
    hold_both_modifiers(&state);
    feed_extended(&state, HYPE_SCANCODE_RIGHT_ARROW_MAKE);
    result = hype_chord_feed_scancode(&state, 0); /* re-check nothing lingers */
    CHECK_HEX("byte after the arrow chord produces no further action",
              HYPE_CHORD_ACTION_NONE, result.action);
}

static void test_arrows_without_both_modifiers_are_ignored(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    feed_extended(&state, HYPE_SCANCODE_RIGHT_CTRL_MAKE); /* only one modifier */
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_EXTENDED_PREFIX);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_LEFT_ARROW_MAKE);
    CHECK_HEX("Left arrow with only right-ctrl held produces no action",
              HYPE_CHORD_ACTION_NONE, result.action);

    hype_chord_state_reset(&state);
    feed_extended(&state, HYPE_SCANCODE_RIGHT_ALT_MAKE);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_EXTENDED_PREFIX);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_RIGHT_ARROW_MAKE);
    CHECK_HEX("Right arrow with only right-alt held produces no action",
              HYPE_CHORD_ACTION_NONE, result.action);
}

static void test_esc_without_both_modifiers_is_ignored(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    feed_extended(&state, HYPE_SCANCODE_RIGHT_ALT_MAKE); /* only one modifier */
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_ESC_MAKE);
    CHECK_HEX("Esc with only right-alt held produces no action",
              HYPE_CHORD_ACTION_NONE, result.action);
}

static void test_digit_without_both_modifiers_is_ignored(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    feed_extended(&state, HYPE_SCANCODE_RIGHT_CTRL_MAKE); /* only one modifier */
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_1_MAKE);
    CHECK_HEX("digit key with only right-ctrl held produces no action",
              HYPE_CHORD_ACTION_NONE, result.action);
}

static void test_cycle_next_arrow_reports_action_on_the_arrow_byte(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    hold_both_modifiers(&state);
    hype_chord_feed_scancode(&state, HYPE_SCANCODE_EXTENDED_PREFIX);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_RIGHT_ARROW_MAKE);
    CHECK_HEX("Right-Ctrl+Right-Alt+Right cycles to next VM",
              HYPE_CHORD_ACTION_CYCLE_NEXT, result.action);
}

static void test_releasing_either_modifier_cancels_the_chord(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    hold_both_modifiers(&state);
    feed_extended(&state, HYPE_SCANCODE_RIGHT_CTRL_BREAK);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_D_MAKE);
    CHECK_HEX("releasing right-ctrl cancels the chord", HYPE_CHORD_ACTION_NONE, result.action);

    hype_chord_state_reset(&state);
    hold_both_modifiers(&state);
    feed_extended(&state, HYPE_SCANCODE_RIGHT_ALT_BREAK);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_D_MAKE);
    CHECK_HEX("releasing right-alt cancels the chord", HYPE_CHORD_ACTION_NONE, result.action);
}

static void test_left_ctrl_alt_are_not_confused_with_right_variants(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    /* Left-Ctrl/Left-Alt share the same base byte as their right-side
     * counterparts but arrive with NO 0xE0 prefix -- must not satisfy
     * the chord. */
    hype_chord_feed_scancode(&state, HYPE_SCANCODE_RIGHT_CTRL_MAKE);
    hype_chord_feed_scancode(&state, HYPE_SCANCODE_RIGHT_ALT_MAKE);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_D_MAKE);
    CHECK_HEX("un-prefixed left-ctrl/left-alt bytes do not arm the chord",
              HYPE_CHORD_ACTION_NONE, result.action);
}

static void test_break_code_of_action_key_produces_no_action(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    hold_both_modifiers(&state);
    result = hype_chord_feed_scancode(&state, (uint8_t)(HYPE_SCANCODE_D_MAKE | 0x80u));
    CHECK_HEX("D's break code does not toggle the dashboard",
              HYPE_CHORD_ACTION_NONE, result.action);
}

static void test_unrelated_extended_key_is_ignored_and_held_state_survives(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    hold_both_modifiers(&state);
    /* Some other extended key (e.g. Home, 0xE0 0x47) that isn't part of
     * the chord at all -- must not disturb held state. */
    feed_extended(&state, 0x47u);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_D_MAKE);
    CHECK_HEX("chord still works after an unrelated extended key",
              HYPE_CHORD_ACTION_TOGGLE_DASHBOARD, result.action);
}

/* TERM-8 (#445): Print Screen's 4-byte make sequence `E0 2A E0 37`. */

static void feed_printscreen_make(hype_chord_state_t *state) {
    feed_extended(state, HYPE_SCANCODE_PRINTSCREEN_MAKE_1);
    feed_extended(state, HYPE_SCANCODE_PRINTSCREEN_MAKE_2);
}

static void test_screenshot_chord(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    hold_both_modifiers(&state);
    feed_extended(&state, HYPE_SCANCODE_PRINTSCREEN_MAKE_1);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_EXTENDED_PREFIX);
    CHECK_HEX("E0 before the second half produces no action yet", HYPE_CHORD_ACTION_NONE,
              result.action);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_PRINTSCREEN_MAKE_2);
    CHECK_HEX("Right-Ctrl+Right-Alt+PrtScn takes a screenshot", HYPE_CHORD_ACTION_SCREENSHOT,
              result.action);
}

static void test_screenshot_without_both_modifiers_is_ignored(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    feed_extended(&state, HYPE_SCANCODE_RIGHT_CTRL_MAKE); /* only one modifier */
    feed_printscreen_make(&state);
    result = hype_chord_feed_scancode(&state, 0);
    CHECK_HEX("PrtScn with only right-ctrl held produces no action", HYPE_CHORD_ACTION_NONE,
              result.action);
}

static void test_printscreen_second_half_alone_is_not_mistaken_for_the_chord(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    /* `E0 37` (keypad '*' extended) without the preceding `E0 2A` first -- must not fire,
     * even with both modifiers held, since it is genuinely a different key. */
    hype_chord_state_reset(&state);
    hold_both_modifiers(&state);
    feed_extended(&state, HYPE_SCANCODE_PRINTSCREEN_MAKE_2);
    result = hype_chord_feed_scancode(&state, 0);
    CHECK_HEX("E0 37 alone is not Print Screen", HYPE_CHORD_ACTION_NONE, result.action);
}

static void test_printscreen_sequence_interrupted_does_not_fire_later(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    hold_both_modifiers(&state);
    feed_extended(&state, HYPE_SCANCODE_PRINTSCREEN_MAKE_1); /* E0 2A: step armed */
    /* Something else entirely arrives instead of the completing E0 37 -- the OSDev-
     * documented sequence is atomic on real hardware, so this proves it aborts cleanly. */
    hype_chord_feed_scancode(&state, HYPE_SCANCODE_D_MAKE); /* also fires TOGGLE_DASHBOARD */
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_EXTENDED_PREFIX);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_PRINTSCREEN_MAKE_2);
    CHECK_HEX("a stray E0 37 after an interruption does not fire a stale screenshot",
              HYPE_CHORD_ACTION_NONE, result.action);
}

/*
 * #458: the encoding this chord ACTUALLY receives.
 *
 * The tests above all feed `E0 2A E0 37`, which is Print Screen pressed with no modifiers. With
 * Alt held -- and the leader always holds it -- the keyboard suppresses the `E0 2A` fake shift
 * and reports SysRq: a bare, non-extended 0x54. So the suite passed while the hotkey could not
 * fire at all. These cover the real sequence, captured from a raw scancode trace.
 */

static void test_screenshot_chord_via_sysrq(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    hold_both_modifiers(&state);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_SYSRQ_MAKE);
    CHECK_HEX("Right-Ctrl+Right-Alt+PrtScn (reported as SysRq 0x54) takes a screenshot",
              HYPE_CHORD_ACTION_SCREENSHOT, result.action);
}

static void test_screenshot_via_sysrq_replays_the_captured_trace(void) {
    /*
     * Byte for byte what the host input poll recorded, including the keyboard's own
     * release-and-repress of Right-Alt around the key. Exactly one action must come out.
     */
    static const uint8_t trace[] = {
        0xE0, 0x1D,       /* RCtrl make */
        0xE0, 0x38,       /* RAlt make */
        0xE0, 0xB8,       /* RAlt break */
        0xE0, 0x38,       /* RAlt make */
        0x54,             /* Print Screen -> SysRq make */
        0xD4,             /* SysRq break */
        0xE0, 0xB8,       /* RAlt break */
        0xE0, 0x9D,       /* RCtrl break */
    };
    hype_chord_state_t state;
    unsigned i, shots = 0;

    hype_chord_state_reset(&state);
    for (i = 0; i < sizeof(trace) / sizeof(trace[0]); i++) {
        if (hype_chord_feed_scancode(&state, trace[i]).action == HYPE_CHORD_ACTION_SCREENSHOT) {
            shots++;
        }
    }
    CHECK_HEX("the captured chord trace fires exactly one screenshot", 1u, shots);
}

static void test_sysrq_without_both_modifiers_is_ignored(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    feed_extended(&state, HYPE_SCANCODE_RIGHT_CTRL_MAKE); /* only one modifier */
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_SYSRQ_MAKE);
    CHECK_HEX("SysRq with only right-ctrl held produces no action", HYPE_CHORD_ACTION_NONE,
              result.action);

    /* And with no leader at all -- a bare SysRq must never be swallowed as a chord. */
    hype_chord_state_reset(&state);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_SYSRQ_MAKE);
    CHECK_HEX("bare SysRq is not a chord", HYPE_CHORD_ACTION_NONE, result.action);
}

static void test_sysrq_break_produces_no_action(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    /* The release must not fire a second screenshot, and must not be read as anything else. */
    hype_chord_state_reset(&state);
    hold_both_modifiers(&state);
    (void)hype_chord_feed_scancode(&state, HYPE_SCANCODE_SYSRQ_MAKE);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_SYSRQ_BREAK);
    CHECK_HEX("SysRq break is not an action", HYPE_CHORD_ACTION_NONE, result.action);
}

static void test_unrelated_plain_key_is_ignored_and_held_state_survives(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    hype_chord_state_reset(&state);
    hold_both_modifiers(&state);
    hype_chord_feed_scancode(&state, 0x1Eu); /* 'A' make code, unrelated */
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_D_MAKE);
    CHECK_HEX("chord still works after an unrelated plain key",
              HYPE_CHORD_ACTION_TOGGLE_DASHBOARD, result.action);
}

/*
 * #568: a screenshot chord that arrives WITHOUT both modifiers must be counted, not merely
 * ignored. On the Intel laptop the operator pressed the chord repeatedly and nothing happened;
 * with no trace at all, a key the keyboard never emitted looked identical to a chord that was
 * seen and rejected. Those are different faults -- one is a missing key, the other missing
 * modifiers -- and they need different fixes, so the counter has to tell them apart.
 */
static void test_near_miss_distinguishes_pressed_from_never_arrived(void) {
    hype_chord_state_t state;
    hype_chord_result_t result;

    /* SysRq with no modifiers: no action, but it WAS seen. */
    hype_chord_state_reset(&state);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_SYSRQ_MAKE);
    CHECK_HEX("bare SysRq takes no action", HYPE_CHORD_ACTION_NONE, result.action);
    CHECK_HEX("bare SysRq is counted as a near miss", 1u, state.screenshot_near_miss);

    /* Print Screen's full two-byte sequence with no modifiers: same. */
    hype_chord_state_reset(&state);
    feed_extended(&state, HYPE_SCANCODE_PRINTSCREEN_MAKE_1);
    /* Both halves are EXTENDED-prefixed (E0 2A E0 37); the near-miss lives in that branch, so
     * feeding the second byte bare would test a different path entirely. */
    hype_chord_feed_scancode(&state, HYPE_SCANCODE_EXTENDED_PREFIX);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_PRINTSCREEN_MAKE_2);
    CHECK_HEX("bare PrtScn takes no action", HYPE_CHORD_ACTION_NONE, result.action);
    CHECK_HEX("bare PrtScn is counted as a near miss", 1u, state.screenshot_near_miss);

    /* A key that never arrives leaves the counter at zero -- the whole point of the distinction. */
    hype_chord_state_reset(&state);
    (void)hype_chord_feed_scancode(&state, HYPE_SCANCODE_RIGHT_CTRL_MAKE);
    CHECK_HEX("a modifier alone is not a near miss", 0u, state.screenshot_near_miss);

    /* And a SUCCESSFUL chord is not a near miss either. */
    hype_chord_state_reset(&state);
    feed_extended(&state, HYPE_SCANCODE_RIGHT_CTRL_MAKE);
    feed_extended(&state, HYPE_SCANCODE_RIGHT_ALT_MAKE);
    result = hype_chord_feed_scancode(&state, HYPE_SCANCODE_SYSRQ_MAKE);
    CHECK_HEX("the full chord still fires", HYPE_CHORD_ACTION_SCREENSHOT, result.action);
    CHECK_HEX("a successful chord is not a near miss", 0u, state.screenshot_near_miss);
}

int main(void) {
    test_reset_clears_held_state();
    test_extended_prefix_alone_produces_no_action();
    test_d_without_both_modifiers_is_ignored();
    test_toggle_dashboard();
    test_return_to_dashboard_esc();
    test_jump_to_vm_1_through_9();
    test_cycle_prev_next_arrows();
    test_arrows_without_both_modifiers_are_ignored();
    test_esc_without_both_modifiers_is_ignored();
    test_digit_without_both_modifiers_is_ignored();
    test_cycle_next_arrow_reports_action_on_the_arrow_byte();
    test_releasing_either_modifier_cancels_the_chord();
    test_left_ctrl_alt_are_not_confused_with_right_variants();
    test_break_code_of_action_key_produces_no_action();
    test_unrelated_extended_key_is_ignored_and_held_state_survives();
    test_unrelated_plain_key_is_ignored_and_held_state_survives();
    test_screenshot_chord();
    test_screenshot_chord_via_sysrq();
    test_screenshot_via_sysrq_replays_the_captured_trace();
    test_sysrq_without_both_modifiers_is_ignored();
    test_sysrq_break_produces_no_action();
    test_screenshot_without_both_modifiers_is_ignored();
    test_printscreen_second_half_alone_is_not_mistaken_for_the_chord();
    test_printscreen_sequence_interrupted_does_not_fire_later();
    test_near_miss_distinguishes_pressed_from_never_arrived();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
