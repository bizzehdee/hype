/*
 * #778: a Raspberry Pi Pico that presents as a USB boot keyboard and types on a schedule.
 *
 * WHY THIS EXISTS
 *
 * Every USB input finding to date needed a human at the keyboard. Boots 8 to 21 each cost
 * the operator a cold boot, a session of typing, and a verbal report, and produced a run
 * that was neither repeatable nor precisely timed -- hype's log prefix is a byte OFFSET, not
 * a timestamp, so "it died about a minute in" was the best timing available.
 *
 * This board fixes both, and adds the thing no human can do overnight.
 *
 *  1. THE COUNTER IS THE CLOCK. Every COUNTER_PERIOD_MS it types an incrementing tag --
 *     A0001, A0002, ... -- so the last tag in HYPE.LOG names the exact second input stopped.
 *     No operator report and no wall clock needed.
 *
 *  2. THE SCRIPT IS FIXED. The same keys in the same order every run, so two runs are
 *     comparable and a change can be scored against the run before it.
 *
 *  3. IT UNPLUGS ITSELF. tud_disconnect()/tud_connect() detach and re-attach the device with
 *     no hand on it. Hot-plug is the path that has failed most often and the one that could
 *     never be tested unattended. The counter continues across a cycle, so the log shows
 *     directly whether input came back -- and if it did not, which tag it stopped at.
 *
 * The exercises are chosen from what has actually broken: chords with right-hand modifiers
 * (#734), a key held past the ten-second typematic bound (#777), fast bursts against slow
 * typing (the input-latency question), and deliberate idle gaps so "armed and silent" is
 * entered on purpose rather than by accident.
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "tusb.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"

/* Every knob the run depends on, in one place, because a run is only comparable to another
 * run with the same numbers. Change one and say so in the run card. */
#define COUNTER_PERIOD_MS     10000u /* one tag every 10 s -- this is the clock */
#define HOTPLUG_EVERY_TAGS       30u /* ~5 min between self hot-plugs */
#define HOTPLUG_DOWN_MS        3000u /* detached for 3 s, longer than any debounce */
#define TAP_MS                   12u /* press-to-release, about a real keystroke */
#define SLOW_CHAR_MS            500u
#define FAST_CHAR_MS              8u /* one per 1 ms frame is faster than any human */
#define MED_CHAR_MS              30u /* brisk human typing -- the control for the fast burst */
/*
 * #787: THREE seconds, not twelve.
 *
 * The board went permanently silent immediately after its twelve-second hold, twice -- boot 36
 * and boot 37 -- and that silence is what provokes the rest: 32 to 64 seconds later hype's
 * revive fires on the stalled endpoint, its Stop Endpoint never completes, and the whole
 * controller's command ring dies with it (#781).
 *
 * A twelve-second hold is twelve seconds of the device NAKing an armed interrupt-IN transfer,
 * which is far longer than anything else the script does -- every other exercise finishes
 * inside two seconds. Whether that length is the trigger is the cheapest question available,
 * and this is the one-line experiment that answers it: if the board survives its holds at
 * three seconds, the length matters; if it still dies, the hold is not the cause and the
 * correlation was coincidence.
 *
 * Nothing is lost by shortening it. It was twelve to cross #777's ten-second typematic bound,
 * and #774 and #777 both closed on boot 36's evidence -- 95 repeats from one press, which is
 * the bounded answer and not the unbounded one.
 */
#define HOLD_MS                3000u
/*
 * #773's measurement string, and why it looks like this.
 *
 * The question is whether a keypress that lands while the endpoint is unarmed is lost. That
 * produces no completion, so it appears in no counter -- the only place it shows up is the
 * difference between what was typed and what the guest received. Boot 32 tried it with a
 * human typing a pangram and the operator reported the obvious problem: at speed they
 * dropped and doubled keys themselves, so the measurement said more about the typist than
 * about hype.
 *
 * A STRICTLY INCREASING sequence removes the need to count anything. Every character in
 * 'a'..'z' then '0'..'9' appears exactly once and in order, so a dropped character leaves a
 * visible gap and a doubled one leaves a visible repeat -- both readable at a glance in the
 * guest's echo, with no reference copy to diff against and no ambiguity about WHICH
 * character went missing. A pangram cannot do that: it repeats letters, so a doubled 'o'
 * and a correctly-typed pair of 'o's look identical.
 */
#define TYPETEST_STR "abcdefghijklmnopqrstuvwxyz0123456789"
#define TYPETEST_REPS             4u
/*
 * THE BOARD STARTS DISARMED, AND ONLY A HUMAN ARMS IT.
 *
 * This is a keyboard. The instant it enumerates it can type into whatever has focus, and the
 * script sends words followed by Enter plus Ctrl and Alt chords -- into a focused terminal
 * that is a command being run.
 *
 * The first version used a 30-second timer for this and it was wrong: a timer runs whether
 * or not anyone is ready, so flashing the board and leaving it plugged in meant it began
 * typing into the operator's session, which is exactly what happened. It typed its tags into
 * the terminal that had focus.
 *
 * A timer cannot know where the board is. A button press can only come from someone holding
 * it. So: nothing is typed until BOOTSEL is pressed. Plug it into the machine under test,
 * press once, walk away. Replugging disarms it again, which is the safe default -- a board
 * that re-arms itself on power-up is a board that types into the next thing it is plugged
 * into.
 *
 * LED blinking = disarmed and safe. LED solid = armed and typing.
 */

static uint32_t g_tag;      /* the counter -- monotonic across hot-plug cycles ON PURPOSE */
static uint32_t g_phase;    /* which exercise runs between this tag and the next */

static uint32_t now_ms(void) { return (uint32_t)(time_us_64() / 1000ull); }

/* Pump the USB stack while waiting. Nothing here may busy-wait without calling tud_task():
 * the device would stop answering the host mid-delay and look like it had crashed. */
static void pump_ms(uint32_t ms) {
    uint32_t t0 = now_ms();
    while ((uint32_t)(now_ms() - t0) < ms) {
        tud_task();
        sleep_ms(1);
    }
}

/*
 * Read BOOTSEL as a button. There is no GPIO for it: the pin is shared with the flash chip
 * select, so reading it means briefly driving QSPI CS low.
 *
 * MUST live in RAM, and must NOT be inlined. Code normally executes from flash over that same
 * QSPI bus, so a version of this compiled into flash pulls CS low and then tries to fetch
 * its own next instruction through the pin it just took away -- the chip hangs on the spot.
 * The first cut was a plain static function and did exactly that: the board went dark and
 * stopped enumerating. The second used __not_in_flash_func, which was still not enough --
 * the compiler INLINED it into main(), which lives in flash, so the section attribute went
 * with the discarded copy. Verify with nm: this symbol must exist and its address must start
 * 0x2000 (RAM), not 0x1000 (flash).
 */
static bool __no_inline_not_in_flash_func(bootsel_pressed)(void) {
    const uint32_t CS_PIN_INDEX = 1;
    uint32_t flags = save_and_disable_interrupts();
    bool pressed;
    unsigned int i;

    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (i = 0; i < 1000; i++) __asm volatile("nop");
    pressed = (sio_hw->gpio_hi_in & (1u << CS_PIN_INDEX)) == 0;
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    restore_interrupts(flags);
    return pressed;
}

/* Blink slowly and answer the host, but type nothing, until BOOTSEL is pressed. */
static void wait_for_arm(void) {
    uint32_t last = now_ms();
    bool led = false;

    while (!bootsel_pressed()) {
        tud_task();
        sleep_ms(1);
        if ((uint32_t)(now_ms() - last) >= 400u) {
            last = now_ms();
            led = !led;
            gpio_put(PICO_DEFAULT_LED_PIN, led);
        }
    }
    /* Debounce, and wait for release so the press is not read twice. */
    while (bootsel_pressed()) { tud_task(); sleep_ms(1); }
    pump_ms(50);
}

static void wait_ready(void) {
    /* Bounded: if the host never makes us ready the run must still make progress rather
     * than wedge silently, which would be indistinguishable from hype going deaf. */
    uint32_t t0 = now_ms();
    while (!(tud_mounted() && tud_hid_ready())) {
        tud_task();
        sleep_ms(1);
        if ((uint32_t)(now_ms() - t0) > 5000u) return;
    }
}

static void send(uint8_t mod, uint8_t key) {
    uint8_t kc[6] = {key, 0, 0, 0, 0, 0};
    wait_ready();
    if (!tud_mounted()) return;
    (void)tud_hid_keyboard_report(0, mod, kc);
}

static void release(void) { send(0, 0); }

static void tap(uint8_t mod, uint8_t key) {
    send(mod, key);
    pump_ms(TAP_MS);
    release();
    pump_ms(TAP_MS);
}

/* ASCII to HID usage, for the small alphabet this script uses. Returns 0 for anything else,
 * which sends nothing rather than a wrong key. */
static uint8_t usage_of(char c) {
    if (c >= 'a' && c <= 'z') return (uint8_t)(HID_KEY_A + (c - 'a'));
    if (c >= '1' && c <= '9') return (uint8_t)(HID_KEY_1 + (c - '1'));
    if (c == '0') return HID_KEY_0;
    if (c == '\n') return HID_KEY_ENTER;
    if (c == ' ') return HID_KEY_SPACE;
    return 0;
}

static void type_str(const char *s, uint32_t gap_ms) {
    for (; *s; s++) {
        uint8_t u = usage_of(*s);
        if (u == 0) continue;
        tap(0, u);
        if (gap_ms > (uint32_t)(2u * TAP_MS)) pump_ms(gap_ms - 2u * TAP_MS);
    }
}

/* The tag. Typed as a letter and four digits so it is greppable in a log full of hex, and
 * so a partial line still identifies which tag it was. */
static void type_tag(void) {
    char buf[8];
    uint32_t n = g_tag;
    buf[0] = 'a';
    buf[1] = (char)('0' + (n / 1000u) % 10u);
    buf[2] = (char)('0' + (n / 100u) % 10u);
    buf[3] = (char)('0' + (n / 10u) % 10u);
    buf[4] = (char)('0' + n % 10u);
    buf[5] = '\n';
    buf[6] = 0;
    type_str(buf, 0);
}

/*
 * Chords, including the RIGHT-hand modifiers. #734 closed on a fault where a board's right
 * Ctrl and right Alt folded onto their left-hand usages and broke every chord, so both sides
 * are sent deliberately and separately.
 */
static void do_chords(void) {
    tap(KEYBOARD_MODIFIER_LEFTCTRL,  HID_KEY_A);
    pump_ms(60);
    tap(KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_B);
    pump_ms(60);
    tap(KEYBOARD_MODIFIER_LEFTALT,   HID_KEY_C);
    pump_ms(60);
    tap(KEYBOARD_MODIFIER_RIGHTALT,  HID_KEY_D);
    pump_ms(60);
}

/*
 * #773: the same known string several times over, at one speed.
 *
 * Repeated because a single clean pass proves little -- the unarmed window is a few
 * milliseconds wide, so a drop is a probabilistic event and four passes at 8 ms per
 * character is 144 chances rather than 36. Each pass ends with Enter so the guest echoes it
 * as its own line, which keeps the passes separable in RUN1A.LOG even if one is mangled.
 */
static void do_type_test(uint32_t gap_ms) {
    unsigned int i;
    for (i = 0; i < TYPETEST_REPS; i++) {
        type_str(TYPETEST_STR, gap_ms);
        tap(0, HID_KEY_ENTER);
        pump_ms(120);
    }
}

/*
 * A key held past the typematic bound. hype synthesises repeat because USB HID devices do
 * not, and #777 bounds a single hold to ten seconds and then emits the BREAK -- because a
 * report that goes missing is indistinguishable from a key still held, which locked a key on
 * for the operator in boot 19. Holding for twelve seconds crosses that bound on purpose.
 */
static void do_hold(void) {
    send(0, HID_KEY_H);
    pump_ms(HOLD_MS);
    release();
}

/* Detach and re-attach, with no hand on the board. */
static void do_hotplug(void) {
    tud_disconnect();
    pump_ms(HOTPLUG_DOWN_MS);
    tud_connect();
    /* Give the host time to enumerate before the next tag, or the tag that proves input
     * resumed would be typed into a device that is not claimed yet. */
    pump_ms(3000);
    wait_ready();
}

int main(void) {
    stdio_init_all();
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    tusb_init();

    /* Disarmed until a human says otherwise. See the comment on arming above. */
    wait_for_arm();
    gpio_put(PICO_DEFAULT_LED_PIN, 1); /* solid = armed */
    wait_ready();

    for (;;) {
        uint32_t t0;

        g_tag++;
        type_tag();

        if (HOTPLUG_EVERY_TAGS != 0u && (g_tag % HOTPLUG_EVERY_TAGS) == 0u) {
            do_hotplug();
        }

        /* One exercise per gap, rotating, so every behaviour is covered every six tags and
         * the pattern is obvious in the log rather than random.
         *
         * Phases 2 and 5 are #773's typing test at two speeds. Running the SAME string at
         * both is the whole point: if the fast pass drops characters and the medium pass
         * does not, the loss is rate-dependent and the unarmed window is the cause. If both
         * are clean, the window is closed. If both drop, it is not about the window at all.
         */
        switch (g_phase % 6u) {
        case 0: break;                            /* idle: armed and silent, on purpose */
        case 1: type_str("slow", SLOW_CHAR_MS); break;
        case 2: do_type_test(FAST_CHAR_MS); break;  /* #773: faster than any human */
        case 3: do_chords(); break;
        case 4: do_hold(); break;
        case 5: do_type_test(MED_CHAR_MS); break;   /* #773 control: brisk but unhurried */
        default: break;
        }
        g_phase++;

        /* Hold the cadence at COUNTER_PERIOD_MS regardless of how long the exercise took, so
         * the tag really is a clock. An exercise that overruns simply shortens its own gap. */
        t0 = now_ms();
        while ((uint32_t)(now_ms() - t0) < COUNTER_PERIOD_MS) {
            tud_task();
            sleep_ms(1);
        }
    }
}
