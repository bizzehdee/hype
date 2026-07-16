#include "pit.h"

#define HYPE_PIT_CHANNEL0_DATA 0x40u
#define HYPE_PIT_CHANNEL1_DATA 0x41u
#define HYPE_PIT_CHANNEL2_DATA 0x42u
#define HYPE_PIT_MODE_COMMAND 0x43u

static void channel_reset(hype_pit_emu_channel_t *ch) {
    ch->reload = 0;
    ch->counter = 0;
    ch->mode = 0;
    ch->access_mode = 3;
    ch->bcd = 0;
    ch->rw_toggle = 0;
    ch->latched_value = 0;
    ch->latch_pending = 0;
    ch->read_toggle = 0;
}

void hype_pit_emu_reset(hype_pit_emu_t *pit) {
    int i;
    for (i = 0; i < 3; i++) {
        channel_reset(&pit->channels[i]);
    }
    pit->port61 = 0;
    pit->ch2_out = 0;
    pit->refresh_toggle = 0;
}

static void write_command(hype_pit_emu_t *pit, uint8_t value) {
    uint8_t channel_sel = (uint8_t)((value >> 6) & 0x03u);
    uint8_t rw = (uint8_t)((value >> 4) & 0x03u);
    uint8_t mode = (uint8_t)((value >> 1) & 0x07u);
    uint8_t bcd = value & 0x01u;
    hype_pit_emu_channel_t *ch;

    if (channel_sel == 3u) {
        return; /* read-back command -- not supported by this stub */
    }
    ch = &pit->channels[channel_sel];

    if (rw == 0u) {
        /* Latch command: snapshot the live counter; programming is untouched. */
        ch->latched_value = ch->counter;
        ch->latch_pending = 1;
        ch->read_toggle = 0;
        return;
    }

    ch->access_mode = rw;
    ch->mode = mode;
    ch->bcd = bcd;
    ch->rw_toggle = 0;
    /* Real hardware: programming a new mode doesn't reload the
     * counter until the guest actually writes a new count. */

    /* Programming channel 2 drives its OUT pin low until the (about to
     * be loaded) count reaches terminal count -- a PIT-based calibration
     * writes this control word (0xb0: ch2, mode 0) then polls port 0x61
     * bit 5 for OUT to go high, so it must start low here. */
    if (channel_sel == 2u) {
        pit->ch2_out = 0;
    }
}

static void write_data(hype_pit_emu_channel_t *ch, uint8_t value) {
    switch (ch->access_mode) {
        case 1: /* lobyte only */
            ch->reload = (uint16_t)((ch->reload & 0xFF00u) | value);
            ch->counter = ch->reload;
            return;
        case 2: /* hibyte only */
            ch->reload = (uint16_t)((ch->reload & 0x00FFu) | ((uint16_t)value << 8));
            ch->counter = ch->reload;
            return;
        case 3: /* lobyte then hibyte -- counter only reloads once both arrive */
            if (ch->rw_toggle == 0) {
                ch->reload = (uint16_t)((ch->reload & 0xFF00u) | value);
                ch->rw_toggle = 1;
            } else {
                ch->reload = (uint16_t)((ch->reload & 0x00FFu) | ((uint16_t)value << 8));
                ch->rw_toggle = 0;
                ch->counter = ch->reload;
            }
            return;
        default: /* access_mode 0 (latch) is not a valid data-write state */
            return;
    }
}

int hype_pit_emu_io_write(hype_pit_emu_t *pit, uint16_t port, uint8_t value) {
    switch (port) {
        case HYPE_PIT_CHANNEL0_DATA:
            write_data(&pit->channels[0], value);
            return 0;
        case HYPE_PIT_CHANNEL1_DATA:
            write_data(&pit->channels[1], value);
            return 0;
        case HYPE_PIT_CHANNEL2_DATA:
            write_data(&pit->channels[2], value);
            return 0;
        case HYPE_PIT_MODE_COMMAND:
            write_command(pit, value);
            return 0;
        default:
            return -1;
    }
}

static uint8_t read_data(hype_pit_emu_channel_t *ch) {
    uint16_t value = ch->latch_pending ? ch->latched_value : ch->counter;

    switch (ch->access_mode) {
        case 1: /* lobyte only */
            ch->latch_pending = 0;
            return (uint8_t)(value & 0xFFu);
        case 2: /* hibyte only */
            ch->latch_pending = 0;
            return (uint8_t)((value >> 8) & 0xFFu);
        case 3: /* lobyte then hibyte */
            if (ch->read_toggle == 0) {
                ch->read_toggle = 1;
                return (uint8_t)(value & 0xFFu);
            }
            ch->read_toggle = 0;
            ch->latch_pending = 0;
            return (uint8_t)((value >> 8) & 0xFFu);
        default:
            return 0;
    }
}

int hype_pit_emu_io_read(hype_pit_emu_t *pit, uint16_t port, uint8_t *out_value) {
    switch (port) {
        case HYPE_PIT_CHANNEL0_DATA:
            *out_value = read_data(&pit->channels[0]);
            return 0;
        case HYPE_PIT_CHANNEL1_DATA:
            *out_value = read_data(&pit->channels[1]);
            return 0;
        case HYPE_PIT_CHANNEL2_DATA:
            *out_value = read_data(&pit->channels[2]);
            return 0;
        default:
            return -1;
    }
}

void hype_pit_emu_tick(hype_pit_emu_t *pit) {
    int i;
    for (i = 0; i < 3; i++) {
        hype_pit_emu_channel_t *ch = &pit->channels[i];
        if (ch->counter == 0) {
            if (ch->mode == 2u || ch->mode == 3u) {
                ch->counter = ch->reload;
            }
            continue;
        }
        ch->counter--;
        /* Channel 2, mode 0 (one-shot): OUT (read back on port 0x61 bit
         * 5) goes high when the counter reaches terminal count. This is
         * what a guest's PIT-based TSC/delay calibration polls to learn
         * the count finished. */
        if (i == 2 && ch->counter == 0 && ch->mode == 0u) {
            pit->ch2_out = 1;
        }
    }
}

unsigned hype_pit_emu_advance(hype_pit_emu_t *pit, uint64_t ticks) {
    int i;
    unsigned ch0_wraps = 0;
    if (ticks == 0) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        hype_pit_emu_channel_t *ch = &pit->channels[i];
        if (ch->mode == 2u || ch->mode == 3u) {
            /* Periodic: the counter cycles within [1, reload]. A 0 reload
             * means the full 65536-count period. */
            uint32_t period = (ch->reload != 0u) ? (uint32_t)ch->reload : 0x10000u;
            uint32_t cur = (ch->counter != 0u) ? (uint32_t)ch->counter : period;
            uint32_t step = (uint32_t)(ticks % period);
            /* Count terminal-count crossings for channel 0 (IRQ0): the
             * first crossing is after `cur` ticks, then every `period`. */
            if (i == 0) {
                if (ticks >= (uint64_t)cur) {
                    ch0_wraps = (unsigned)(1u + (ticks - (uint64_t)cur) / (uint64_t)period);
                }
            }
            cur = (step < cur) ? (cur - step) : (period - (step - cur));
            ch->counter = (uint16_t)cur;
        } else {
            /* One-shot (modes 0/1/4/5): counts down to 0 and stops. */
            if (ticks >= (uint64_t)ch->counter) {
                ch->counter = 0;
                if (i == 2 && ch->mode == 0u) {
                    pit->ch2_out = 1;
                }
            } else {
                ch->counter = (uint16_t)((uint64_t)ch->counter - ticks);
            }
        }
    }
    return ch0_wraps;
}

void hype_pit_emu_port61_write(hype_pit_emu_t *pit, uint8_t value) {
    pit->port61 = (uint8_t)(value & HYPE_PIT_PORT61_WRITABLE);
}

uint8_t hype_pit_emu_port61_read(hype_pit_emu_t *pit) {
    uint8_t value = (uint8_t)(pit->port61 & HYPE_PIT_PORT61_WRITABLE);

    /* Bit 4 is the RAM-refresh clock: on real hardware it toggles
     * continuously, so flip it every read -- a guest that times a delay
     * by watching it always sees it change rather than spinning. */
    pit->refresh_toggle = (uint8_t)(pit->refresh_toggle ^ 1u);
    if (pit->refresh_toggle) {
        value |= HYPE_PIT_PORT61_REFRESH;
    }
    if (pit->ch2_out) {
        value |= HYPE_PIT_PORT61_CH2_OUT;
    }
    return value;
}
