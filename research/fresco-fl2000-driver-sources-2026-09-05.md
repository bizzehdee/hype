# Fresco Logic FL2000 / FL2000DX -- what source material exists (2026-09-05)

Research for **#793** (USB host console phase 1). Recorded here so it is never re-fetched.

## There is no datasheet

No public FL2000/FL2000DX datasheet or programming guide could be found. The usable material is
driver source, not a spec.

## The source found

**https://github.com/F5OEO/fl2k** -- fetched 2026-09-05. A **display** driver for the FL2000 USB-VGA
chip (not the SDR repurposing that `osmo-fl2k` does with the same silicon -- that was the first
thing checked, because a driver that abuses the RGB DACs as arbitrary sample output would document
the transfer path and nothing about display operation).

- Forked from `ykaukab/fl2k`.
- Its own README describes it as **"an official driver release from Fresco Logic"**, with no
  mention of clean-room reverse engineering. So it is vendor-published source, which is a better
  situation than reverse-engineered material -- and a worse one licensally, see below.
- Scope, in its own words: **"This driver only covers the USB part of the display logic."**
- **Licence: GPL-2.0.**

### What it documents

- The **USB transfer path** and the bandwidth requirement.
- Kernel integration, building, loading.

### What it does NOT document

- **EDID / DDC / I2C** -- so mode discovery is unaddressed.
- **Sync generation.**
- **RGB DAC output configuration** in detail.

These are exactly the parts a from-scratch driver needs beyond "push bytes at a bulk endpoint",
so the gap matters.

## The two facts that should shape #793

**1. 373 MB/s, continuous, with no buffer.** The README's own arithmetic:
`1920 * 1080 * 24bpp * 60 = 373,248,000 bytes/sec`. And **the chip has no onboard frame
buffering** -- it depends on sustained USB 3.0 bandwidth and needs the stream *on time*, not on
average.

That is the hard constraint for hype, not the byte count. `core/blk_usb.c`'s ticket lock
(#346/#362) serialises every host USB transfer across cores, and boot AMD-L0 run 9 measured a
**318,810 us** flush slice against a 10 ms budget (#809). A 300 ms stall against a device with no
frame buffer is a blanked or corrupted display, and a console refresh far below 60 Hz does not
remove the requirement that each frame's bytes arrive without a gap.

**2. Licence incompatibility is a real possibility, and it is a blocker.** hype is **GPL-3.0**
(`LICENSE`). The driver is **GPL-2.0**. GPL-2.0-**only** is incompatible with GPL-3.0; GPL-2.0
**or-later** is not. Linux kernel drivers are conventionally GPL-2.0-only, which would mean hype
may not incorporate or closely derive from this source at all -- it could still be read for
factual register knowledge, but that distinction needs deciding deliberately, not assumed.

**The actual licence header in the source files has not been read.** The GitHub label does not
distinguish only from or-later. That is the first thing to check, before any code.

## Deliberately not vendored

The source is not cloned into this repo. Vendoring GPL-2.0 code into a GPL-3.0 tree is the very
question above, so it stays out until the licence is settled.
