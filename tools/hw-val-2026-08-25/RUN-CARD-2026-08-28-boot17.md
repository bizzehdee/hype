# Boot 17 -- missed keys, and key repeat

Boot 16's real finding was not the capture I staged for; it was the two things you noticed
while typing. Both are fixed here, and both are things you can judge directly.

## DO NOT UNPLUG THE HYPE DRIVE

`HYPEBOOT` is the boot medium **and** the log medium.

## What changed, and one correction

**Missed keys when typing fast (#773).** I told you the 125 Hz input tick was running at
50 Hz. **That was wrong** -- my arithmetic paired timestamps and poll counts that did not
span the same interval. Measured directly with a counter on the tick itself, it runs at
122-136 Hz, exactly as intended.

The real cause is worse and had nothing to do with the rate. An interrupt endpoint carries
one transfer at a time, and hype re-armed it only on the NEXT tick -- so after every report
the keyboard was **unarmed for up to 8 ms**. A USB HID report is a snapshot of which keys are
down, not a list of what happened, so the device has nowhere to record a key that was pressed
and released while nothing was armed. It simply never existed. Raising the tick rate would
have narrowed that window without closing it. hype now re-arms the instant it takes a report.

**Key repeat (#774).** Nothing implemented it, and nothing could. A PS/2 keyboard repeats in
hardware and your guest expects that; a USB keyboard sends one report when a key goes down,
one when it comes up, and nothing at all while you hold it. hype presents PS/2 scancodes fed
from USB, so the repeat has to be invented -- and now is. Only the last key pressed repeats
and a new key takes over, which is what a PS/2 keyboard does rather than an approximation of
it. Defaults are the PS/2 power-on values: 500 ms, then about 11 a second.

Your guest can also change it now. The `0xF3` command was being swallowed by a generic ACK,
so a guest asking for a fast repeat silently got the default; hype honours it and applies it
to every keyboard, since the guest sees one keyboard however many are merged behind it.

**Also in this build:** #753 (a deferral metric that had been reporting the age of a stale
timestamp), and #758 twice over -- a port read mid-transition on a root port, and then the
same thing on the hub path, which I had missed the first time and which broke rig 746 until
I found it.

## What to do

No hot-plugging this time. Just type.

1. Boot, wait for the guest login prompt.
2. **Type fast.** Faster than is comfortable. Compare it with last time -- do keys still go
   missing?
3. **Hold a key down.** A letter, then backspace, then an arrow key. Each should repeat after
   about half a second and then run at a steady rate.
4. Try a chord (**Right-Ctrl + Right-Alt + D**) and hold a key inside a VM as well as at the
   dashboard.
5. Let it run a few minutes, then power off.

Both questions are things only you can answer -- the log can show me repeats being injected,
but not whether typing FEELS right.

## What I will read afterwards

| line | means |
|---|---|
| `host-hid: guest set typematic 0x..` | your guest asked for its own rate and got it |
| `DIAG: host-kbd scancodes=` | should be markedly higher for the same amount of typing, because repeats now count |
| `#764 DIVERGED` | should be absent; boot 16's two were benign ring-wrap and the detector now requires a divergence to persist |
| `BSPCOST` | where the BSP loop's time goes -- new, and the reason I could tell the tick was healthy |

## Afterwards

    cp \HYPE.LOG \RUN1A.LOG  tools/hw-val-2026-08-25/logs/boot-17/
