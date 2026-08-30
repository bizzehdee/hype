# Boot 36 -- eight tickets, and the only thing you have to get right is arming the Pico

Everything that broke boots 32 to 35 is fixed. The guest boots, the media resolves with the
spare drives attached, and the Keychron recovers on its own. What is missing is a clean run
with the Pico actually typing, because that is where four of these eight tickets live.

Boot 35's Pico reported **nothing at all** -- `arms=8, reports=0`, including across 96 seconds
while its controller was still healthy. If it had been armed and sending its ten-second tags,
hype would have seen about nine of them. Zero reports on a working controller is what a
disarmed board looks like.

## Before you boot

The Pico's firmware changed in boot 33. If you have not reflashed it since, do that first:
hold BOOTSEL while plugging it into this machine, drag `tools/pico-kbd/build/hype_pico_kbd.uf2`
onto the drive that appears, then put it back on the front hub.

**Leave both spare USB drives plugged in.** They are the condition #780 needs; a run without
them passes for the wrong reason.

## The sequence

1. Boot. Stay on the dashboard.
2. **Press BOOTSEL once. Confirm the LED goes SOLID, not blinking, and that `a0001` appears.**
   This is the whole run. If the LED is still blinking, press again and check.
3. Confirm the guest reaches a login prompt.
4. Leave the machine alone for **90 minutes**. Nothing else to do.
5. Power off normally. Bring back `HYPE.LOG` and `RUN1A.LOG`, plus `HYPE.1.LOG`,
   `RUN1A.1.LOG` and `hype-log-prev.txt` if a warm reboot happened.

## The eight

| Ticket | What it needs from the run |
| --- | --- |
| **#773** | The Pico types `abcdefghijklmnopqrstuvwxyz0123456789`, four passes at 8 ms per character and four at 30 ms. The guest echoes it into `RUN1A.LOG`. The string is strictly increasing, so a dropped character leaves a gap and a doubled one a repeat -- nothing to count. Both speeds clean closes it; losses only at speed mean the unarmed window is real. |
| **#774** | The Pico holds `h` for 12 seconds every sixth tag. A run of repeats in the echo is the fix working -- hype synthesises typematic, the device does not. |
| **#777** | That same hold crosses the 10-second bound, so the run of repeats must STOP before the release. |
| **#775** | `HIDTICK ... revives= revive_fail=` -- revives climbing with `revive_fail=0` and `reports=` resuming after each. Boot 35 showed this four times on controller[1] already. |
| **#780** | The boot medium keeps its own serial with the spare drives attached, and the guest boots. Confirmed once in boot 34; a second clean run closes it. |
| **#641** | A measurement, not a pass: `APVCPU vm0/N: exits=` and `PERF: hlt_wait=`. It was 328.9M exits in 84 minutes on boot 31. |
| **#426** | The standing HW-VAL gate -- NVMe and xHCI ring math on the shared DMA facility, no regression. The run completing is the evidence. |
| **#775 / #781e** | If controller[2]'s command ring wedges again, `cmdring timeouts= guard= recoveries=` and a `DEAD` marker say so. Not a pass condition here; it feeds #781. |

## What would make this run worth repeating rather than reading

- **The Pico never armed.** `reports=0` on `cafe:4b44` -- then four of the eight got nothing,
  same as boot 35. Check the LED at step 2 and this cannot happen.
- **The guest not booting.** Then #773, #774 and #777 have nothing to echo into. Step 3 catches
  it in the first two minutes rather than the ninety-first.

## About the controllers, because it changes what "input works" means

The Keychron is on controller[1] with the log stick. The Pico and the Logitech receiver are on
controller[2], which is where the command ring wedged in boots 31, 34 and 35. If controller[2]
stops again, **the Keychron will keep working and the machine will feel completely fine** --
the Pico's `reports=` count in `HIDTICK` is the witness for that controller, not your hands.
