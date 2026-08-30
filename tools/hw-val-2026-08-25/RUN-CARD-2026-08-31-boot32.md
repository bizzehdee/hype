# Boot 32 -- the command-ring fix, on the run that found the bug

Boot 31 answered the question. All input stopped at t=9.9 minutes -- Keychron, Pico and
Logitech receiver within half a minute of each other, and the hub status endpoints with
them. Three keyboards and five hub devices do not fail together; one controller does. They
all sit on controller[2] (2f:00.3), and its COMMAND RING stopped answering at that moment.

hype's answer to a command that never completes was to give up on that command and enqueue
the next one behind it. So every command for the remaining 74 minutes timed out -- 614 of
them, one second each, taken out of the 125 Hz input tick. Reviving a deaf endpoint needs
two commands, so nothing could be revived, and the counter said `revives=0` because the
failure path incremented nothing. The log was healthy for all 84 minutes and never said the
ring had stopped, because nothing looked at it.

## What this build changes

- **The command ring recovers itself.** On a timeout hype now reads USBSTS, aborts the
  command (CRCR.CA), waits for the ring to stop, drains what the abort posts, and restarts
  the ring at its base -- xHCI 4.6.1.2. Four recoveries, then it declares the ring dead.
- **A dead ring fails fast.** Once given up on, commands fail immediately instead of waiting
  a second each. The dashboard and the guests stay responsive even in the worst case.
- **A FAILED revive is now visible.** `HIDTICK` carries `revive_fail=` next to `revives=`,
  plus the controller's `cmdring timeouts=/recoveries=` and a `DEAD` marker.
- **A STOPPED completion (cc 26/27/28) is no longer treated as a transfer failure.** It is
  what a Stop Endpoint retires outstanding transfers with. hype used to "recover" the
  endpoint on it, issuing Reset Endpoint against a Stopped endpoint -- boot 31 shows exactly
  that line immediately before the command ring stopped answering.
- **The DEADMAN counts a failed revive.** It previously needed a revive to have SUCCEEDED,
  so it would have declined to fire in exactly boot 31's case.

## The sequence

1. Boot, stay on the dashboard, press BOOTSEL on the Pico, confirm `a0001`.
2. Type on the Keychron for ~30 s, then leave the machine alone.
3. Run for **at least 90 minutes** -- boot 31 died at 9.9 min and ran 84, so this needs to
   cover the same ground and more.
4. Come back and try both keyboards. **Whether they work or not is the result**; note which.
5. Power off normally. Bring back `HYPE.LOG` and `RUN1A.LOG`. If a warm reboot happened
   (DEADMAN, or you typed `host reboot`), also bring `HYPE.1.LOG`, `RUN1A.1.LOG` and
   `hype-log-prev.txt`.

## What each outcome means

- **Input alive at the end, `cmdring timeouts=0`**: the ring never stopped this run. Good,
  but it does not prove the recovery works -- the recovery was never exercised.
- **Input alive, `cmdring recoveries=` non-zero**: the ring stopped and hype brought it
  back. That is the fix working, and it is the outcome to hope for.
- **Input dead, `revive_fail=` climbing and `cmdring ... DEAD`**: the ring stopped and could
  not be restarted. The fix did not save the run, but it named the failure honestly instead
  of grinding silently -- and the DEADMAN should have warm-rebooted the machine for you.
- **Input dead with `cmdring timeouts=0`**: a different fault from boot 31's. Say so; that
  would be new.
