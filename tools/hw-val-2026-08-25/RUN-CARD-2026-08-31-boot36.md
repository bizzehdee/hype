# Boot 36, second attempt -- attach the console to the guest FIRST, then arm the Pico

The first attempt panicked, and it was worth it -- it found something that had been broken
since #604 landed and had been quietly costing every salvage attempt.

**The panic.** `PANIC: ... vector=14 (Page Fault) error_code=0x11 rip=0xddbbb668 cr2=0xddbbb668`.
When rip equals cr2 and the error code says "present page, instruction fetch", that is a
no-execute violation on the first instruction at that address -- and 0xddbbb668 is inside UEFI's
Runtime Services code, which hype had marked NX. hype reboots the host by calling ResetSystem()
through exactly that code. So **no host reboot has worked since the NX pass landed**, which is
why the DEADMAN warm reboot and `host reboot` have never once produced a salvage file. Fixed in
this build; Runtime Services code is now exempt, and the log lists every exempt range.

**Two tickets closed off the short run anyway.** The Pico held `h` for twelve seconds and hype
produced 95 repeats from one press -- #774, since USB HID devices do not repeat at all. And 95
is the bounded answer, not the unbounded one: 10 seconds of repeats at the 92 ms period is ~104,
twelve seconds would be ~126. That is #777's bound firing.

**What went wrong with the test, and it was my error not yours.** The Pico's typing went into
hype's own terminal, not the guest -- `TERMCMD: 'abcdefghijklmnopqrstuvwxyz0123456789'`, five
times, perfectly. Nothing had attached the console to vm0, so the guest never saw a keystroke
(`GUESTKBD vm0: routed=14` against 963 scancodes reaching hype). #773 needs those passes echoed
by the guest, so step 3 below is new and it matters.

## Before you boot

The Pico's firmware changed in boot 33. If you have not reflashed it since, do that first:
hold BOOTSEL while plugging it into this machine, drag `tools/pico-kbd/build/hype_pico_kbd.uf2`
onto the drive that appears, then put it back on the front hub.

**Leave both spare USB drives plugged in.** They are the condition #780 needs; a run without
them passes for the wrong reason.

## The sequence

1. Boot. Stay on the dashboard.
2. **Wait for the guest to reach a login prompt.** Do not skip ahead -- step 3 needs it there.
3. **Press Right-Ctrl + Right-Alt + 1** to attach the console to vm0. The screen should switch
   from the dashboard to the guest. Type a couple of characters and see them echo. **This is
   the step the last run was missing**; without it the Pico types into hype's terminal and
   #773 gets nothing.

   > **Both modifiers must be the RIGHT-hand ones.** The leader is Right-Ctrl + Right-Alt held
   > together; the left-hand keys do not form it. This is not pedantry -- #734 was a keyboard
   > folding its right modifiers onto the left usages, which broke every chord, and the last
   > run's log carries the near-miss line `chord key seen with only Right-Alt held`.
   >
   > `Right-Ctrl + Right-Alt + Right-arrow` gets to the same place by cycling
   > (dashboard -> vm0 -> vm1 -> ... -> dashboard) if the number key does not take.
   > `Right-Ctrl + Right-Alt + Esc` goes back to the dashboard.
   >
   > Once the view is on a VM, every key that is not a chord goes to that guest -- that is the
   > whole mechanism, and it is why the echo test in this step is worth the ten seconds.
4. **Press BOOTSEL once. Confirm the LED goes SOLID, not blinking, and that `a0001` appears in
   the guest.** If the LED is still blinking, press again and check.
5. Leave the machine alone for **90 minutes**. Nothing else to do.
6. Power off normally. Bring back `HYPE.LOG` and `RUN1A.LOG`, plus `HYPE.1.LOG`,
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
