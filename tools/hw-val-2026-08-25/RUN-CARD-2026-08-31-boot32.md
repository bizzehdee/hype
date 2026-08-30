# Boot 32 -- the command-ring fix, plus a harvest of On Hold tickets

Boot 31 answered why input dies. Every device on controller[2] (2f:00.3) went silent within
half a minute of t=9.9 min and stayed silent for the remaining 74 minutes of an 84-minute
run -- three keyboards and five hub devices together, because the controller's COMMAND RING
stopped answering and hype went on enqueuing behind the wedge. Fixed in `b0669e3`
(plan.md decision 74, ticket #779).

The log itself has now survived two long runs in a row, so this run can also collect evidence
for tickets that have been sitting On Hold waiting for exactly this machine.

## Part 1 -- the main run (no action needed)

1. Boot, stay on the dashboard, press BOOTSEL on the Pico, confirm `a0001`.
2. Type on the Keychron for ~30 s (part 2 makes this useful), then leave the machine alone.
3. Leave it running for **at least 90 minutes**.
4. Come back and try both keyboards. **Whether they still work is the result.**

### What this settles on its own, with no extra work

- **#779 / #775** -- the command ring and the endpoint revive. See the outcome table below.
- **#641** -- the idle-vCPU HLT storm. `APVCPU` and `PERF` print every run; boot 31 measured
  328.9M exits on one AP in 84 minutes. Another data point costs nothing.

### The outcome table for #779

| What the log says at the end | What it means |
| --- | --- |
| Input alive, `cmdring timeouts=0` | The ring never stopped. Good run, but the recovery was never exercised, so it proves nothing about the fix. |
| Input alive, `cmdring recoveries=` non-zero | **The result we want.** The ring stopped and hype brought it back. |
| Input dead, `revive_fail=` climbing, `cmdring ... DEAD` | The ring stopped and could not be restarted. The fix did not save the run but named the failure honestly, and the DEADMAN should have warm-rebooted the machine for you. |
| Input dead, `cmdring timeouts=0` | A different fault from boot 31's. Say so -- that would be new. |

## Part 2 -- three short actions that clear five On Hold tickets

Each takes well under a minute. Do them in this order, near the START of the run, so the
90 minutes of part 1 still happen afterwards.

### A. Type a known sentence at the guest login (settles #773)

Log into the guest and type this exact line, at a comfortable speed, then press Enter:

```
the quick brown fox jumps over the lazy dog 0123456789
```

Then type it a second time **as fast as you can**.

Why it settles it: #773 is about a keypress landing while the endpoint is unarmed, which
produces no completion and so shows up in no counter. The guest ECHOES what it receives, and
`RUN1A.LOG` records the echo, so the two lines can be compared against the text above
character by character. Missing or doubled characters are the defect; two clean copies are
the fix. Say roughly how fast the second one was.

### B. Hold one key down for 15 seconds (settles #774 and #777)

At the guest prompt, hold the `a` key for a slow count of fifteen, then release.

Why it settles them: #774 is "a held key never repeats" -- a run of `a`s in the echoed line
is the fix working. #777 is "typematic repeats forever when the release report is lost" --
the repeat is bounded at 10 seconds, so the run of `a`s should STOP before you let go. Note
whether it stopped on its own, and roughly when.

### C. Unplug something from a REAR socket and plug it back (settles #744 and #745)

Pick any device in a **rear root port** -- a spare stick, a webcam, anything. Pull it, wait
five seconds, plug it back into the SAME socket.

> **NOT the HYPEBOOT drive.** It is both the boot medium and the log medium; unplugging it
> ends the run and loses the evidence. And not anything on the front hub -- that is the path
> #746 already covers.

Why it settles them: #744 (root-port departure and slot teardown) and #745 (root-port arrival
and claim) have never been exercised on real hardware -- every hot-plug in boots 30 and 31 was
behind the hub. The log should show `PORT EVENT port=N` at the moment you pull it, a teardown,
then another `PORT EVENT` and an enumeration when you push it back.

## Deliberately NOT in this run

**#754** wants a USB storage device pulled mid-write. That is a good test and it needs its own
run: the only USB storage in this configuration is the drive hype is writing its log to, and
pulling it would destroy the record of everything else on this card.

## What to bring back

`HYPE.LOG` and `RUN1A.LOG`. If a warm reboot happened -- the DEADMAN fired, or you typed
`host reboot` -- also `HYPE.1.LOG`, `RUN1A.1.LOG` and `hype-log-prev.txt`.

Tell me which of parts 2A/2B/2C you actually did, and roughly when. Timestamps in the log are
byte offsets, not clock time, so knowing "the replug was about ten minutes in" is what lets
the right lines be found.
