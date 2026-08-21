# SENDKEYS key loss was hype's PS/2 bug, not a harness timing problem

**Ticket:** #582. **Backs:** the `SENDKEYS` note in the `microtests` skill.

## What happened

Typed guest commands lost most of their keystrokes, so a command missing 14 of
its 18 events never formed a word. This read as "typed commands do not work" and
cost two runs on #177. The apparent fix — spacing keys further apart — was
treated as a harness timing rule for a while.

The real cause was in hype: the PS/2 poll disabled itself after a single taken
IRQ1, and the i8042 controller holds only ONE byte. Every key that arrived
before the BSP took the next interrupt was dropped by the controller.

## The lesson

- Measured at 1-second spacing: **4 of 18** events arrived before the fix,
  **18 of 18** after. The fix removed the spacing rule entirely.
- A signal that looks like a flaky harness can be a deterministic device bug.
  Count what actually arrives (18 in, N out) before blaming timing.
