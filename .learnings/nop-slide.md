# Zeroed guest RAM is a NOP slide — never pad a payload with zeros

**Ticket:** #535. **Backs:** the "never pad with zeros" rule in the `microtests`
skill and the "guest RAM is zeroed before first execution" invariant.

## What happened

A microtest reported a correct PASS while the guest was entered at the **wrong**
address. The bytes `0x00 0x00` decode as `add byte [rax], al` — a NOP-equivalent
on the paths that mattered. Zeroed guest RAM is therefore a NOP slide: a guest
entered anywhere before its payload slides forward through the zeros and
executes the payload anyway, producing a perfectly correct verdict that proves
nothing about the entry point.

## The lesson

- Never pad a guest payload with zeros.
- `tests/micro/crt0.S` fills its pre-entry region with `0xCC` (int3) so a wrong
  entry faults instead of sliding.
- hype's launch log prints the *intended* rip, not a readback — a correct log
  line is not proof the guest started where you think. Fill padding with a
  trapping byte and prove the guard fires.
