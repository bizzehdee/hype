# Boot 31 -- repeat boot 30 unchanged, to find out whether the log death is gone

Boot 30 ran 35 minutes with the log alive the whole time: 4,022,111 bytes on the stick,
`flush_fail_streak=0`, `stalled=0`, worst backlog 32 KB and always caught up. The four boots
before it all lost the log inside the first few minutes. Same build, same stick, opposite
outcome.

One run cannot tell a fix from luck. This boot is the same build again, changed in nothing,
to see whether the log survives a second time.

## The sequence

1. Boot, stay on the dashboard, press BOOTSEL on the Pico, confirm `a0001`.
2. Type on the Keychron for ~30 s, then leave the machine alone.
3. Leave it running for **at least 35 minutes** -- the same length as boot 30, so the two
   runs compare directly. Longer is better.
4. If the dashboard shows `** LOG N KB BEHIND **`, note roughly when, and leave it running.
5. If input dies and does not come back, wait five minutes: the DEADMAN warm-reboots the
   machine on its own to salvage the in-RAM log. If input still works but the log has died,
   type `host reboot`.
6. Power off normally at the end.
7. Bring back `HYPE.LOG` and `RUN1A.LOG`. Also bring `HYPE.1.LOG`, `RUN1A.1.LOG` and
   `hype-log-prev.txt` **if a warm reboot happened** -- they only exist in that case.

## What boot 30 already answered

- **The log flush is not the thing that is broken.** Across 35 minutes it drained 7.58 MB in
  1369 slices, never failed a write, never stalled, never fell more than 32 KB behind.
- **The guests read the ISO fine.** No media errors, no I/O errors, the guest reached the
  login prompt and its heartbeat ran to the end of the run.
- **The endpoint revive works.** The Keychron's interrupt-IN endpoint went silent 29 times
  and the revive restored it every time; reports kept arriving afterwards. This is the boot
  29 question, answered: a revived endpoint does come back.

## The new finding to watch

The Pico left the bus and came back **five times, at 6.9, 13.3, 19.7, 26.1 and 32.5 minutes**
-- every 6.4 minutes, to the tenth of a minute. hype saw the hub report the port empty,
released the slot, and re-enumerated the device about 4 seconds later. Input kept working
because the hot-plug path recovered it each time.

A 6.4-minute period that exact is not a loose cable. Watch whether boot 31 shows the same
five-or-six departures at the same spacing. If it does, the cadence is the lead on what
detaches the device.
