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
2. Do part 2 below (about three minutes), then leave the machine alone.
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

## Part 2 -- short actions that clear five On Hold tickets

**Everything you type in this part must be typed on the KEYCHRON** (`3434:0da4`), not the
Pico. The Pico sends fixed scripted bursts and never holds a key down, so it cannot test
typematic at all and cannot test fast human typing. Leave it armed anyway -- see the note on
witnesses below.

### The topology, verified on the host before this run

| Where | Device | Notes |
| --- | --- | --- |
| `1-6` root port 6, 12M | **Keychron K10** | On controller[1] (02:00.0), directly on a root port, no hub. This is what part A needed -- it is **already done**. |
| `1-5` root port 5, 480M | SanDisk Cruzer Blade (`sdc`, 7.5 GB) | The only USB drive that is safe to unplug. |
| `2-4` root port 4, 5000M | **SABRENT bridge = HYPEBOOT (`sdb`)** | Boot medium AND log medium. **Never unplug this one.** |
| `3-4.3` behind the UGREEN hub | Pico `cafe:4b44` | On controller[2] (2f:00.3). Still armed. |
| `3-4.4` behind the same hub | Logitech receiver | Also controller[2]. |

### Read this before judging the run: the witnesses have changed

Boot 31's command-ring death was on **controller[2]**, and back then all three keyboards sat
behind the hub there, so "input died" was a reliable alarm for it.

The Keychron has now moved to **controller[1]**. If controller[2] stops again, the Keychron
will keep working and the machine will feel fine. **The Pico is now the witness for
controller[2]** -- so arm it, leave it armed, and judge that controller by its `reports=`
count in `HIDTICK`, not by whether you can type.

The flip side: the Keychron now shares a controller with the log stick. If controller[1] is
the one that dies, the log dies with it -- which is exactly the case the DEADMAN warm reboot
and `\hype-log-prev.txt` exist for, so let it fire rather than powering off.

### A. Cycle the Keychron in its rear socket (settles #744 and #745)

The Keychron is already in the right place (`1-6`, a rear root port). What is still needed is
the EVENT: hype has to watch it leave and come back.

1. With hype at the dashboard, unplug the Keychron. Wait five seconds.
2. Plug it back into the **same rear socket**.
3. Type a few characters to confirm it still reaches the guest.

Why it settles them: #744 (root-port departure and slot teardown) and #745 (root-port arrival
and CLAIM) have never run on real hardware -- every hot-plug in boots 30 and 31 was behind the
hub. #745's bar is specifically a keyboard or mouse claimed after boot on a root port, so a
USB drive cannot settle it; it has to be a HID, which is why the Keychron move matters.

### B. Type a known sentence twice (settles #773)

At the guest prompt, type this exact line, at a comfortable speed, then press Enter:

```
the quick brown fox jumps over the lazy dog 0123456789
```

Then type the same line again **as fast as you can**, and press Enter.

Why it settles it: #773 is about a keypress landing while the endpoint is unarmed, which
produces no completion and so appears in no counter -- which is why the earlier attempt to
close it on `lost=0` was measuring the wrong thing. The guest ECHOES what it receives and
`RUN1A.LOG` records the echo, so the two lines get compared against the text above character
by character. Missing or doubled characters are the defect; two clean copies are the fix.
Say roughly how fast the second one was.

### C. Hold one key down for 15 seconds (settles #774 and #777)

At the guest prompt, hold the `a` key for a slow count of fifteen, then release.

Why it settles them: #774 is "a held key never repeats" -- a run of `a`s in the echoed line is
the fix working. #777 is "typematic repeats forever when the release report is lost" -- the
repeat is bounded at 10 seconds, so the run of `a`s should STOP before you let go. Note
whether it stopped on its own, and roughly when.

### D. Optional: cycle the SanDisk (extra evidence for #744)

Pull the **SanDisk Cruzer Blade** from its rear socket, wait five seconds, put it back in the
same socket. It is a mass-storage arrival rather than a HID one, so it exercises a different
branch of #744's enumeration than part A does.

> **Only the SanDisk.** The other USB drive on this machine is the SABRENT bridge at `2-4`,
> which is HYPEBOOT -- the boot medium and the log medium both. It is the one on the
> SuperSpeed port, and unplugging it ends the run and destroys the record of everything else
> on this card. If you are unsure which is which by feel, do not do part D; it is optional and
> part A already covers #744.

## Deliberately NOT in this run

**#754** wants a USB storage device pulled MID-WRITE. Part 2D does not settle it: hype only
inventories those two drives, it never writes to them, so pulling one exercises enumeration
rather than the USB-SATA bridge's behaviour under a yank -- which is the whole point of the
ticket. Settling it needs a config that makes hype write to a sanctioned scratch drive
identified by the serial hype itself reports from INQUIRY VPD 0x80, and that deserves its own
short run rather than being bolted onto this one.

## What to bring back

`HYPE.LOG` and `RUN1A.LOG`. If a warm reboot happened -- the DEADMAN fired, or you typed
`host reboot` -- also `HYPE.1.LOG`, `RUN1A.1.LOG` and `hype-log-prev.txt`.

Tell me which of parts 2A-2D you actually did, and roughly when. Timestamps in the log are
byte offsets, not clock time, so knowing "the replug was about ten minutes in" is what lets
the right lines be found.
