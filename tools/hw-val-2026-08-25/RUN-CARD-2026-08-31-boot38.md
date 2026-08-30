# Boot 38 -- does the Pico survive a SHORT hold, and does KBDCHARS see the typing

Boot 37 answered a question nobody had asked. The Pico stopped after its twelve-second hold --
your account, and the log agrees: `reports=` froze at 1737 and never moved. Then, 32 to 64
seconds later, hype's revive fired on that stalled endpoint, its **Stop Endpoint never
completed**, and controller[2]'s command ring died with it. The line before the timeout reads
`cmdring timeouts=0`: the ring was healthy right up to the command hype itself issued.

So the wedge is **provoked, not spontaneous**, which is why it has never once touched
controller[1] -- the Keychron went silent and was revived eight times in that same run without
incident.

This run tests the cheapest explanation.

## Two changes

- **The Pico's hold is 3 seconds now, not 12.** Twelve seconds is twelve seconds of the device
  NAKing an armed transfer, far longer than anything else the script does. If the board
  survives its holds at three, the length is the trigger and #787 has its answer. If it dies
  anyway, the hold is not the cause and the correlation was coincidence. Either outcome is
  worth the boot.
- **`KBDCHARS` records what hype hands the guest.** #773 has now failed to be measured twice --
  once because the console was not attached, once because `RUN1A.LOG` captured the heartbeat
  and not one character of the typing. It is recorded at the hand-over point now, and unlike
  the last two attempts it has been watched working: `tools/773` types a known string in QEMU
  and gets it back verbatim.

## Before you boot

**The Pico's firmware changed again -- reflash it, this run depends on it.**
hold BOOTSEL while plugging it into this machine, drag `tools/pico-kbd/build/hype_pico_kbd.uf2`
onto the drive that appears, then put it back on the front hub.

**Leave both spare USB drives plugged in.** They are the condition #780 needs; a run without
them passes for the wrong reason.

## The sequence

1. Boot. Stay on the dashboard.
2. **Press Right-Ctrl + Right-Alt + 1 first** -- you cannot see the guest until you do, and
   the dashboard will not tell you it has reached a login prompt.

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
3. **Wait for `localhost login:` on that screen, then type `root` and Enter** to get a shell
   prompt (`localhost:~#`). **This is the step the last run was missing** -- without a focused,
   logged-in guest the Pico types into hype's own terminal and #773 gets nothing.

   The input script already logged a shell in, but on **ttyS0**, the serial console -- a
   different tty from the one the screen shows. That is why the screen still sits at a login
   prompt with the heartbeat running: two consoles, two logins. The script cannot reach the
   screen tty, so this one is yours.

   The Pico must land at a **shell** prompt, not a login prompt: at a login prompt the alphabet
   pass becomes a rejected username and never echoes, which is exactly the measurement #773
   needs.

4. **Press BOOTSEL once. Confirm the LED goes SOLID, not blinking, and that `a0001` appears in
   the guest.** If the LED is still blinking, press again and check.
5. Leave the machine alone for **90 minutes**. Nothing else to do.
6. Power off normally. Bring back `HYPE.LOG` and `RUN1A.LOG`, plus `HYPE.1.LOG`,
   `RUN1A.1.LOG` and `hype-log-prev.txt` if a warm reboot happened.

## What this run answers

| Ticket | What to read | Says |
| --- | --- | --- |
| **#787** | `HIDTICK[...] cafe:4b44 ... reports=` after each hold | climbing past every hold = the board survives a 3 s hold and the twelve seconds were the trigger. Frozen = the hold length is not the cause |
| **#773** | `fw-1 KBDCHARS: last N of M handed to the guest: [...]` | the alphabet passes, verbatim. Strictly increasing, so a gap or a repeat is visible at a glance. Both speeds clean closes it; losses only in the fast passes mean the unarmed window is real |
| **#781** | `ctrl<N> command ring stopped answering` | **absent** is the result to hope for. If the Pico survives its holds and the ring never wedges, the two are linked and the fix is upstream of the reset |
| **#775** | `revives= revive_fail=` | revives climbing with `revive_fail=0` |
| **#780** | `media: registered` and the guest booting | the boot medium keeps its own serial with the spare drives attached |
| **#641** | `APVCPU vm0/N: exits=` | a measurement, not a pass |
| **#426** | the run completing | the standing HW-VAL gate |

Every revive and command-ring line now names its controller (`ctrl1` / `ctrl2`). Boot 37 needed
three cross-referenced HIDTICK lines to work out which one had died, because slot id 5 was in
use on both.

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
