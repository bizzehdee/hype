# Boot 10 -- hot-plug, with the log finally able to answer

Boot 9 fixed the boot-time keyboard. This boot is about hot-plug, and about the hitching.

## DO NOT UNPLUG THE HYPE DRIVE

`HYPEBOOT` is the boot medium **and** the log medium. Only the keyboard and mouse move.

## What boot 9 settled

| | |
|---|---|
| Keyboard at boot | **Works.** #755 (a second claim on one slot lowered Context Entries and invalidated the first endpoint) and #757 (stale enumeration port bits re-judged as departures) were both real |
| #757 confirmed | `STEP: dropped 8 port-change bit(s)` -- eight stale bits the old build would have swept as departures. One false departure kills keyboard, guest media and log together, stickily |
| Hitching | **#759.** Each idle interrupt-IN poll burned 19,531 event-ring reads. #746 arms one endpoint per hub and you have five, so hype was doing ~156,000 reads a tick to learn nothing. Fixed: a poll now takes one look |
| Why the counters were useless | **#760.** The periodic DIAG is behind a 30-second gate and printed ONCE, before you touched anything. `reports=0` described the idle period before the test. Fixed: port events are logged where they happen |

The throttled tick was also why hot-plug felt dead: the sweep rides the same 125 Hz tick,
and it was running at a few hertz.

## The run

Boot, wait for the login prompt and a heartbeat, then:

| step | do | wait | reads |
|------|----|------|-------|
| 1 | Press **Right-Ctrl + Right-Alt + D** | -- | the keyboard works at boot (boot 9 says it should) |
| 2 | Unplug the **mouse** from the hub | 15 s | behind-hub departure |
| 3 | Re-plug it in the same port | 15 s | behind-hub arrival; move the mouse |
| 4 | Unplug the **keyboard** from the hub | 15 s | the mouse must keep working |
| 5 | Re-plug it, then chord again | 15 s | behind-hub re-claim |
| 6 | Move the keyboard to a **direct port on the machine**, chord again | 15 s | root-port arrival -- a different path from the hub one |
| 7 | Move it back to the hub, chord again | 15 s | -- |
| 8 | Stop touching it | rest of run | the #750 soak |

**Please also say whether the hitching is gone.** That is a real result either way, and it
is not something the log reports as well as you can.

## Reading it afterwards

Every hot-plug should leave a three-line ladder in `\HYPE.LOG`:

```
host-xhci: PORT EVENT port=N (event #K on this controller)   <- the controller told hype
host-usb: port N on this controller changed -- now empty      <- the sweep judged it
host-hid: keyboard ... DEPARTED                               <- hype acted
```

Which line is **missing** is the diagnosis:

| missing | means |
|---|---|
| no `PORT EVENT` at all | the controller never raised one, or hype is not dequeuing. For a device behind a hub this is expected -- a hub reports through its own status-change endpoint, so look for `hub slot N status bitmap` instead |
| `PORT EVENT` but no `changed` | the sweep is not running |
| `changed -- something is attached` on an unplug | #758: PORTSC read before CCS cleared |
| all three present | the path works |

`hub slot N status bitmap 0x..` is the one to look for on steps 2-5. Boot 9 showed
`HUBPOLL reports=0`, but that count was taken before the test, so it proved nothing --
this boot will actually say.

## Afterwards

    cp \HYPE.LOG \RUN1A.LOG  tools/hw-val-2026-08-25/logs/boot-10/

`\HYPE.LOG` needs `LC_ALL=C grep -a`.
