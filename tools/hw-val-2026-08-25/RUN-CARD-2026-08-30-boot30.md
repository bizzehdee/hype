# Boot 30 -- the post-mortem boot: warm reboot instead of power-off

Boot 29 produced four seconds of log for a three-minute run. Every earlier boot did a
version of the same thing: the log file stops growing early, silently, while the run
continues (boot 26: log dead at ~14 s, input alive for 45 minutes). Every line explaining
WHY sits in the in-RAM capture buffer, which a power-off destroys.

A WARM reboot preserves RAM, and on the next boot hype scans it, finds the previous boot's
entire capture buffer -- including everything written AFTER the file died -- and writes it
to `\hype-log-prev.txt` on the boot volume (RT-1b, #452). This machine has no reset
button, so this build gives hype two ways to warm-reboot itself:

- **`host reboot`** typed in the hype terminal (#175). The reset now tries WARM first,
  exactly so RAM survives for the salvage. Use this when the log has died but the
  keyboard still works -- which is the usual shape.
- **DEADMAN**: if the Pico had been delivering tags, then goes silent for 5 minutes with a
  failed revive behind it, hype declares the input path dead and warm-reboots itself. No
  action needed from you. It announces itself on the live display first.

## The sequence

1. Boot, stay on the dashboard, press BOOTSEL on the Pico, confirm `a0001`.
2. Type on the Keychron ~30 s, then leave the machine alone.
3. When input dies, wait: the revive experiment (boot 29, unanswered) needs the five
   minutes after death, and the DEADMAN fires itself at the end of them.
4. If input still works but the dashboard shows the new `** LOG N KB BEHIND **` alert,
   let it run a few more minutes, then type `host reboot` in the hype terminal.
5. After the reboot, let hype reach the dashboard again (~60 s), then power off normally.
6. Bring back **`HYPE.1.LOG`** (the death run's log -- the salvage boot renames it),
   `RUN1A.1.LOG`, **`hype-log-prev.txt`**, and the salvage boot's own `HYPE.LOG` (it says
   whether RT-1b found the buffer).

## What changed in this build

- The dashboard alerts on the gap between the capture buffer and the file
  (`** LOG N KB BEHIND **`), computed on every idle-loop pass -- so it fires whichever way
  the flush dies: failing writes, a silent stall, or the drain never being scheduled.
  Boot 29's alert only watched write failures, and the flush was not failing.
- `host reboot` tries EfiResetWarm before cold/CF9.
- The DEADMAN auto-reboot described above. It triggers ONLY on the Pico (cafe:4b44), only
  after it had worked, and only after a revive was attempted -- a person walking away from
  an ordinary keyboard cannot fire it.

## What the salvage file should settle

- Whether the flush was failing (`FLUSH FAILED` / streak lines) -- pointing at the stick's
  USB bridge, which has form for dropping under sustained write -- or silently draining
  nothing (USBFLUSH lines with `stalled` climbing), which points at hype's own drain logic.
- Whether the revive fired after input death and whether tags resumed (HIDTICK lines
  continuing past the point the file froze).

## Honest status

The log-death root cause is unknown; this build adds no fix, only the machinery to finally
see it. The input-death root cause is unknown; the revive experiment is unanswered. One
run with a warm reboot at the end should answer both. If `hype-log-prev.txt` comes back
absent or empty, RAM did not survive this board's warm reset, and that is worth knowing
too -- say so and we fall back to photographing the live display.
