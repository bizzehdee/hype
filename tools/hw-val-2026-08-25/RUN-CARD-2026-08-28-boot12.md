# Boot 12 -- the retry storm, and whether hot-plug finally works

Boot 11 brought the hitching back and killed the keyboard. Your log said why, and it was
hype's doing again, not your hardware.

## DO NOT UNPLUG THE HYPE DRIVE

`HYPEBOOT` is the boot medium **and** the log medium. Only the keyboard and mouse move.

## What boot 11 found

Making hub reports work (#761) switched on a path that then ran away:

```
hub slot 6 status bitmap 0x02
  Address Device slot 8 try 1/2/3: completion code 4   <- Transaction Error
hub slot 6 port 1 Address Device FAILED
hub slot 1 status bitmap 0x02      ... and again, 28 times
```

There is a device hype cannot address behind one of the SuperSpeed hubs. Each retry resets
the port, the reset sets a change bit, the hub reports it, hype retries -- 38 Address Device
attempts, each a command timeout, all from the 125 Hz input tick. **That device is on the
same hub as your keyboard**, so the storm starved the tick the keyboard's own polling rides
on: `HID[0] polls=9136 reports=8`.

Fixed: three attempts, then that port is left alone and said so once. Cleared only when the
port reads empty. Your webcam and Bluetooth adapter are things hype has no use for, so
giving up on them costs nothing.

Also fixed: hype tracked at most 6 hub devices and your machine has 5 -- a USB-3 hub is two
USB devices, a 2.0 half and a 3.0 half, each polled separately. Your two hubs plus the one
on the motherboard fill it. Now 16, with 24 interrupt-IN blocks.

**One correction to the last run card.** It said you had five hubs. You have two, plus one
inside the machine. The log was counting USB devices, not hubs, and now says
`hub-devices=` and explains itself.

## The run

Boot, wait for the login prompt and the heartbeat, then:

| step | do | wait |
|------|----|------|
| 1 | Press **Right-Ctrl + Right-Alt + D** | -- |
| 2 | Unplug the **mouse** from the hub | 15 s |
| 3 | Re-plug it, move it | 15 s |
| 4 | Unplug the **keyboard** from the hub | 15 s |
| 5 | Re-plug it, chord again | 15 s |
| 6 | Move the keyboard to a **direct port on the machine**, chord again | 15 s |
| 7 | Move it back to the hub, chord again | 15 s |
| 8 | Stop touching it | rest of run |

If the keyboard dies, note which step and stop -- the log is the result.

## What to read afterwards

| line | means |
|---|---|
| `giving up on hub slot N port M after 3 failed arrivals` | #763 working -- it should appear once or twice, early, and then never again |
| `Address Device ... completion code 4` repeating dozens of times | #763 NOT working; the storm is back |
| `HUBPOLL hub-devices=5 ... reports=` small and non-zero | the hub path is alive |
| `dequeue ... is past our TRB ... NOT re-armed` | #764 diagnostics. **Expected to appear.** It is counting only, deliberately -- I do not yet understand what it is measuring and will not act on it until I do |
| a keyboard that stops with `errors=0` while `polls` climbs | the deaf-endpoint failure again -- note the `reports=` value it stopped at |

Whether the **hitching** is gone is again something only you can report.

## Afterwards

    cp \HYPE.LOG \RUN1A.LOG  tools/hw-val-2026-08-25/logs/boot-12/

`\HYPE.LOG` needs `LC_ALL=C grep -a`.
