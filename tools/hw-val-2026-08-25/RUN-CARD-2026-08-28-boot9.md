# Boot 9 -- the boot-8 fixes, and a log that can say where it stopped

Boot 8 came back with a dead keyboard, no guest, and a log that stops mid-sentence. Three
defects came out of its log. This boot tests the fixes and, more importantly, **can report
its own failure** if they are not enough.

## DO NOT UNPLUG THE HYPE DRIVE

The stick labelled `HYPEBOOT` is the boot medium **and** the log medium. Only the keyboard
and mouse move. Nothing else.

## What boot 8 told us

| # | finding |
|---|---|
| 755 | **Confirmed in the log.** The Logitech receiver is claimed twice -- keyboard interface and mouse interface -- and the second claim lowered the slot's Context Entries from 5 to 3, invalidating the endpoint just configured. Fixed |
| 757 | **Likely the cause of everything.** Port-change bits banked during enumeration were never cleared, so the first hot-plug sweep re-judged them and could call a settled port a DEPARTURE. A departure releases the slot and marks the log sink gone *stickily* -- killing the keyboard, the guest's media and the log at once, surviving a re-plug, and erasing its own evidence. Fixed by dropping the backlog once, after enumeration |
| 756 | **Why it cost a whole boot.** Nothing drained the log between the GOP handoff and the dispatch loop, so four candidate failure points were indistinguishable. Fixed: a flush and a `STEP:` line at each |
| 758 | Rig 744 loses a departure about 1 run in 4 (PORTSC read while CCS is still set). Open, not in this boot |

The keyboard **was** enumerated and claimed on boot 8 -- `3434:0da4 ... owner=hype`, and the
device inventory was byte-for-byte identical to boot 6. Nothing is wrong with your cabling.

## Before you start

- Keyboard and mouse on the **2.0 hub**, where they are now. Do not re-cable.
- Boot the drive. `\EFI\BOOT\BOOTX64.EFI` is the default build, `\hype.cfg` is `hype1a.cfg`.

## First: does it get further than boot 8?

Boot 8's log ended at `host-stream: vm0 CD001 verified streaming`. This one should continue
straight into new lines:

```
fw-1 STEP: dashboard ready (...)
fw-1 STEP: dropped N port-change bit(s) banked during enumeration -- hot-plug starts from here
fw-1 STEP: phys-confirm gate passed
fw-1 STEP: USB log owner latched to the BSP
fw-1 AP[vm0 vCPU 0]-SMOKETEST ... (one per vCPU)
```

**`dropped N` is the number that matters.** If N is greater than 0, boot 8's first sweep had
that many stale bits to misread, and #757 was real.

**If it stops again, the last `STEP:` line names where.** That is the whole point of this
boot -- boot 8 could not tell us, and this one can.

## Then: the guest and the keyboard

If it reaches the guest, you get a login and a heartbeat every 10 s. Then:

1. Press **Right-Ctrl + Right-Alt + D**. The chord should reach the dashboard.
2. If the keyboard works, run the boot-8 hot-plug sequence: unplug the mouse, wait 15 s,
   re-plug, wait 15 s; then the same for the keyboard; then move the keyboard to the front
   USB-C port and back. Chord after each re-plug.
3. Then leave it idle for the rest of the run -- that is the #750 soak.

If the keyboard does **not** work, do not spend time on it. Let it sit for a minute so a
`DIAG` line lands, then power off. The log is the result.

## What to read afterwards

| what | means |
|---|---|
| `STEP: dropped 0 port-change bit(s)` | #757 was NOT boot 8's cause -- look at the last `STEP:` line instead |
| `STEP: dropped N` with N>0, and the boot continues | #757 was real and is fixed |
| the log stops after a `STEP:` line | that step is the wedge; the next boot targets it directly |
| `DIAG: HID[i/n] ... reports=` non-zero | the keyboard is alive |
| `CTXDUMP int-in slot=... entries=` | must never DECREASE across two claims on one slot (#755) |
| `soft lockup` in `\RUN1A.LOG` | #750 is not fixed on hardware |

## Afterwards

Archive both logs before re-staging -- `stage.sh` refuses while they are non-empty:

    cp \HYPE.LOG \RUN1A.LOG  tools/hw-val-2026-08-25/logs/boot-9/

`\HYPE.LOG` needs `LC_ALL=C grep -a`; it contains invalid UTF-8.
