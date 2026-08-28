# Waiting for QEMU to exit: use `pidof`, not `pgrep`

Both `pgrep` spellings are wrong here, in opposite directions. One matches too
much and hangs; the other matches too little and waves the check through.

## `pgrep -f` matches the shell running the loop

The usual guard against a wait loop seeing itself is to bracket a character:

```sh
while pgrep -f '[q]emu-system-x86_64' >/dev/null; do sleep 2; done   # WRONG
```

The bracket stops the *pattern text on pgrep's own command line* matching,
because that argv holds `[q]emu...`, not `qemu...`.

It does not stop the loop matching the shell that contains it. When a whole
script is passed as one string -- `bash -c '...'`, which is what a tool-driven
or `ssh`-driven invocation does -- that shell's `/proc/<pid>/cmdline` holds the
entire script text, and the script text contains the literal
`qemu-system-x86_64`. `pgrep -f` searches full command lines, so it finds the
shell. The loop waits for itself and never exits.

Nothing says so. The rig produces no lines and is killed by the outer timeout,
which reads exactly like a slow or hung guest. A one-shot guard of the same
shape fails the other way and reports `FAIL: a qemu is still running` when
none is.

## `pgrep -x` silently matches nothing

The obvious repair is to match the executable name instead of the command line:

```sh
while pgrep -x qemu-system-x86_64 >/dev/null; do sleep 2; done       # ALSO WRONG
```

`pgrep` compares against `comm`, which the kernel truncates to 15 characters.
`qemu-system-x86_64` is 18. It can never match a running QEMU. On this box
`pgrep` prints a warning to stderr and exits 1 -- so a guard written this way
passes unconditionally, and a rig that should have refused to start runs anyway
against a QEMU that is still up.

`pgrep -x qemu-system-x86` (the truncated name) does work, but only by knowing
where the kernel's cut falls.

## Use `pidof`

```sh
while pidof qemu-system-x86_64 >/dev/null; do sleep 2; done          # RIGHT
```

`pidof` resolves the executable, so it has no 15-character limit and cannot
match a shell that merely mentions the name. Verified both ways: it finds a
running QEMU, and `bash -c` containing the literal string does not self-match.

Use `pgrep -f` only when the name genuinely is not enough -- picking one QEMU
out of several by a flag on its command line. Exclude the current shell
explicitly there: `pgrep -f "$pat" | grep -vx "$$"`.

Same rule for killing: `killall`, never `pkill`, for the same truncation reason
(already noted in `tools/371/ab-transport.sh`).

## Related

- `AGENTS.md` -- one QEMU at a time, the rule these guards enforce.
- `.learnings/qemu-rig-traps` -- `grep | head` masking an exit status is the
  same class of fault: a rig that reports success because the check never ran.
