# Boot 39 -- the run that is finally allowed to last 90 minutes

Boot 38 did not crash. hype rebooted the host on purpose, and the log line said "all guests
down" while the guest was healthy and had heartbeated nine seconds earlier. It was the
**DEADMAN**: the Pico goes silent around five minutes in, five minutes later the deadman fires,
and a ninety-minute run ends at eleven. That has now happened twice, taking #775, #780, #641
and #426 with it both times.

The deadman exists to warm-reboot into the RT-1b scan when the log is dying and the in-RAM
buffer is the only copy. **The log is not dying any more** -- boots 34 through 38 all kept it to
the last byte -- so it now only fires when there is genuinely something to salvage: a failing
flush, or a backlog past the dashboard's alert threshold. With a healthy log it says so and
stays up, because the remaining eighty minutes are worth more than a salvage of a file that is
already on disk.

The reboot line no longer misdescribes itself either. It states the actual reason.

## What boot 38 settled

- **#773 closed.** 1,395 characters through `KBDCHARS`, every alphabet pass intact, not one
  missing character at 8 ms or at 30 ms.
- **#787: the hold is not the trigger.** At a 3-second hold the board survived all three of its
  holds and then died at a0022 in the middle of ordinary typing.
- **#604 confirmed on hardware.** The host reboot completed with no page fault -- the first one
  that has worked since the NX pass landed.
- **#788 raised** for a single doubled character in 1,395.

## The sequence

Unchanged, and the order still matters.

1. Boot, stay on the dashboard.
2. **Right-Ctrl + Right-Alt + 1** (both modifiers right-hand). You cannot see the guest until
   you do.
3. Wait for `localhost login:`, type **`root`** and Enter, get `localhost:~#`.
4. **BOOTSEL once** -- LED solid, `a0001` appears in the guest.
5. Leave it **90 minutes**. It should now actually get there.
6. Power off normally. Bring back `HYPE.LOG` and `RUN1A.LOG`.

The Pico will very likely go deaf around five minutes in and take controller[2] with it. **That
is expected now and is not a reason to stop.** You should see it say so:

```
fw-1 DEADMAN: the tag keyboard (cafe:4b44 slot5) is dead after N revive(s) and M failed
revive(s) -- NOT rebooting, the log is healthy and the rest of the run is worth more
```

## What the 90 minutes is for

| Ticket | What to read | Passes when |
| --- | --- | --- |
| **#775** | `revives= revive_fail=` on the Keychron | revives climbing with `revive_fail=0` and reports resuming after each. This is controller[1], which keeps working |
| **#780** | `media: registered` and the guest booting | the boot medium keeps its own serial with the spare drives attached |
| **#641** | `APVCPU vm0/N: exits=` and `PERF: hlt_wait=` | a measurement, not a pass |
| **#426** | the run reaching 90 minutes | the standing HW-VAL gate |
| **#787** | when the Pico dies, and after what | more data on whether it is volume or elapsed time. It died at 2,406 reports last run and 1,737 the run before |
| **#781** | `ctrl<N> command ring stopped answering` | the label is fixed -- last run it said ctrl1 for what the enumeration calls controller[2] |
