# Boot 34 -- the guest gets its disk back, and the Pico's typing test finally has somewhere to land

Boot 33 settled the hot-plug question and exposed a different one.

**#744 and #745 are closed.** You pulled the Keychron from its rear socket twice and hype
handled both cycles end to end -- port event, teardown, arrival, enumeration, claim, and it
typed afterwards. The slot id recycled 5 -> 7 -> 8, so teardown really is returning slots.
That is the port-power fix working.

**The guest had no disk, and it was not a coincidence.** You saw "No bootable option or
device" twice in a row. hype refused to use its own boot drive:

```
media: registered host device 1 = usb serial='DB9876543214E'
host-media: vm[0] 'run1a': media_disk = 'DB9876543214E' is not present
            -- refusing to stream media from a different drive
```

Device 1 IS that drive. The registration stored a POINTER to a single shared buffer that gets
rewritten for every mass-storage device hype probes, so the stick on the second controller
renamed the first one after the fact. It started when the spare drives went in for the
hot-plug tests, which is why it appeared out of nowhere. Fixed, with a QEMU rig (`tools/780`)
that reproduces the exact line against the old binary and passes against this one.

## Why this run matters for #773, #774 and #777

They got nothing from boot 33. The Pico ran and hype counted its keystrokes -- 1,134 scancodes
and 30 chords -- but the guest never booted, so there was nothing to echo them into and
`RUN1A.LOG` is five lines of firmware complaining. The typing test needs a live guest at a
prompt. This run should give it one.

## The sequence

1. Boot. Stay on the dashboard. Press BOOTSEL once to arm the Pico, confirm `a0001`.
2. **Check the guest reaches a login prompt** before you walk away. If it says "No bootable
   option or device" again, stop and say so -- that would mean the fix is incomplete and the
   rest of the run is wasted.
3. Leave the machine alone for **at least 90 minutes**.
4. Power off normally. Bring back `HYPE.LOG` and `RUN1A.LOG`, plus `HYPE.1.LOG`,
   `RUN1A.1.LOG` and `hype-log-prev.txt` if a warm reboot happened.

Nothing else to do by hand. Leave the spare USB drives plugged in -- they are the condition
that triggered #780, so this run is only a fair test of it with them there.

## What it settles

| Ticket | How | Needs you? |
| --- | --- | --- |
| **#780** media identity | Step 2. The guest booting at all is the answer. | Step 2 only |
| **#773** keypress lost in the unarmed window | The Pico types `abcdefghijklmnopqrstuvwxyz0123456789` -- four passes at 8 ms per character, four at 30 ms -- and the guest echoes it into `RUN1A.LOG`. Strictly increasing, so a gap or a repeat is visible at a glance. | No |
| **#774** held key never repeats | The Pico holds `h` for 12 seconds every sixth tag. | No |
| **#777** typematic repeats forever | The same hold crosses the 10-second bound. | No |
| **#779 / #775** command ring and revive | The 90 minutes. | No |
| **#641** idle-vCPU HLT storm | `APVCPU` and `PERF` print every run. | No |

## Reading the result

**#773** turns on the two speeds together. If the fast passes drop characters and the medium
passes do not, the loss is rate-dependent and the unarmed window is the cause. Both clean
means the window is closed. Both dropping means the window is not the explanation.

**#779** is not settled by input still working. If `cmdring timeouts=0`, the ring never
stopped and the recovery was never exercised. The result worth having is input alive with
`cmdring recoveries` non-zero.

**Which controller is which.** The Keychron is on controller[1] with the log stick; the Pico
and the Logitech receiver are on controller[2], where boot 31 died. If controller[2] stops
again the Keychron keeps working and the machine feels fine, so the Pico's `reports=` count in
`HIDTICK` is the witness there, not your hands.
