# `pgrep -f qemu` matches the shell that is running the wait loop

## The trap

The usual guard against a wait loop seeing itself is to bracket a character:

```sh
while pgrep -f '[q]emu-system-x86_64' >/dev/null; do sleep 2; done
```

The bracket stops the *pattern text on the pgrep command line* from matching,
because `pgrep`'s own argv holds `[q]emu...`, not `qemu...`.

It does **not** stop the loop matching the shell that contains it. When a whole
script is passed as one string -- `bash -c '...while pgrep -f ...'`, which is
what a tool-driven or `ssh`-driven invocation does -- that shell's `/proc/pid/cmdline`
holds the entire script text, and the script text contains the literal
`qemu-system-x86_64`. `pgrep -f` searches full command lines, so it finds the
shell. The loop waits for itself and never exits.

Nothing in the output says so. The rig simply produces no lines and is killed by
the outer timeout, which reads exactly like a slow or hung QEMU.

## The fix

Match the process **name**, not the command line:

```sh
while pgrep -x qemu-system-x86_64 >/dev/null; do sleep 2; done
```

`-x` with no `-f` compares against the executable name only, so a shell whose
script merely mentions the name cannot match. Bracketing is then unnecessary.

Use `-f` only when the name genuinely is not enough to identify the process
(picking one QEMU out of several by a flag on its command line). In that case
exclude the current shell explicitly: `pgrep -f pattern | grep -v "^$$\$"`.

## Related

- [one-qemu-at-a-time](../AGENTS.md) -- the rule this loop exists to enforce.
- `.learnings/qemu-rig-traps` -- `grep | head` masking an exit status is the
  same class of fault: a rig that reports success because the check never ran.
