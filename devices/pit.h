#ifndef HYPE_DEVICES_PIT_H
#define HYPE_DEVICES_PIT_H

#include <stdint.h>

/*
 * Guest-visible 8254 PIT emulation (M3-4). NOT arch/x86_64/cpu/pit.c,
 * which programs the HOST's own real PIT for this hypervisor's own
 * timer tick -- this is a from-scratch register-level model that a
 * guest's port I/O (0x40-0x42 channel data, 0x43 mode/command,
 * write-only) gets routed to once an SVM IOIO intercept exists to
 * route it (M3-5's job), so a guest's own PIT programming (channel
 * mode, lobyte/hibyte access, latch-and-read) completes correctly
 * instead of hanging or reading back garbage.
 *
 * "Stub" scope: correct mode/access-mode/BCD programming per channel,
 * lobyte/hibyte read/write sequencing, and the latch command --
 * exactly what a guest's own PIT-programming and calibration-read code
 * paths touch. Each channel's live counter only advances when the
 * caller calls hype_pit_emu_tick() -- this project decides the
 * real-time cadence that represents (e.g. once per host PIT tick,
 * arch/x86_64/cpu/timer.c), the same "reusable primitive, caller wires
 * the cadence" split used throughout this project.
 */

typedef struct {
    uint16_t reload;        /* last value written via lobyte/hibyte or latch source */
    uint16_t counter;       /* live down-counter */
    uint8_t mode;            /* operating mode 0-5 (Table on the 8254 datasheet) */
    uint8_t access_mode;     /* 0 = latch count, 1 = lobyte only, 2 = hibyte only, 3 = lobyte/hibyte */
    uint8_t bcd;             /* 1 = BCD counting (unused by this stub's counting logic) */
    uint8_t rw_toggle;       /* access_mode 3 write sequencing: 0 = expect lobyte next, 1 = hibyte */
    uint16_t latched_value;  /* snapshot taken by the latch command */
    uint8_t latch_pending;   /* 1 if a latch command is outstanding -- next read(s) return latched_value */
    uint8_t read_toggle;     /* access_mode 3 read sequencing: 0 = next read returns lobyte, 1 = hibyte */
} hype_pit_emu_channel_t;

typedef struct {
    hype_pit_emu_channel_t channels[3];
} hype_pit_emu_t;

/* Resets all three channels to their post-power-on state (mode 0,
 * access mode 3, zeroed counters) -- call on every (re)start. */
void hype_pit_emu_reset(hype_pit_emu_t *pit);

/*
 * Handles a guest OUT to one of the four PIT ports (0x40/0x41/0x42
 * data, 0x43 mode/command). Returns 0 if `port` is one of the four
 * this device owns, non-zero otherwise.
 */
int hype_pit_emu_io_write(hype_pit_emu_t *pit, uint16_t port, uint8_t value);

/*
 * Handles a guest IN from one of the three data ports (0x40/0x41/0x42
 * -- 0x43 is write-only, matching real hardware), filling *out_value.
 * Returns 0 if `port` is recognized, non-zero otherwise.
 */
int hype_pit_emu_io_read(hype_pit_emu_t *pit, uint16_t port, uint8_t *out_value);

/*
 * Advances every channel's live counter down by one. On reaching 0:
 * modes 2 (rate generator) and 3 (square wave) -- the two modes real
 * OSes actually use for a periodic timer interrupt -- auto-reload
 * from `reload`; every other mode just stays at 0 (terminal count
 * reached; this stub doesn't model the exact OUT-pin/one-shot
 * semantics of modes 0/1/4/5, which don't matter without real
 * interrupt injection wired to this device yet anyway).
 */
void hype_pit_emu_tick(hype_pit_emu_t *pit);

#endif /* HYPE_DEVICES_PIT_H */
