# Invariant: guest RAM is zeroed before first execution

**Hard invariant. Do not weaken without updating `plan.md` §10 first.**

Guest RAM is zeroed before first execution, on every (re)start, including after
Force power off — never reused as-is.

Corollary for hype's own test payloads: never pad a guest payload with zeros.
`0x00 0x00` decodes as `add byte [rax], al`, so zeroed RAM is a NOP slide and a
guest entered at the wrong address slides into its payload and reports a false
PASS. See [nop-slide.md](nop-slide.md).
