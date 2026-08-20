#!/usr/bin/env python3
"""
#563: fail the build's checks on a new un-annotated `static` inside run_fw_1_test().

That function runs once per VM and, since SMP, concurrently on several cores. A function-level
static is one object for the whole program, so every VM shares it and every write races. The
default reading of a bare `static` there -- "this is my VM's variable" -- is wrong, and the cost is
not theoretical: #557's shared one-shot silenced every VM but the first and sent a bare-metal
investigation to the wrong guest.

A `static` there is allowed only if it is either:
  - `const` (read-only, so it cannot race), or
  - annotated with a comment on the declaration line or the line above saying why one-per-host is
    intended (look for "#563", "one-per-host", or "per-host").

Anything else is per-VM state pretending to be local, and belongs on hype_fw_vm_t (vm->diag).
"""
import re
import sys

PATH = "boot/main.c"
START = re.compile(
    r"^static void run_fw_1_test\(hype_fw_vm_t \*vm, const hype_vmm_ops_t \*ops, "
    r"hype_vmm_kind_t kind\) \{"
)
OK = ("#563", "one-per-host", "per-host")


def main() -> int:
    lines = open(PATH, encoding="utf-8", errors="surrogateescape").read().split("\n")

    start = next((i for i, l in enumerate(lines) if START.match(l)), None)
    if start is None:
        print("check-fw1-statics: run_fw_1_test() not found -- the guard has gone blind, which is "
              "worse than a failure. Fix the pattern in tools/check-fw1-statics.py.")
        return 2
    end = next((i for i in range(start + 1, len(lines)) if lines[i] == "}"), None)
    if end is None:
        print("check-fw1-statics: could not find the end of run_fw_1_test()")
        return 2

    bad = []
    for i in range(start, end + 1):
        line = lines[i]
        stripped = line.strip()
        if not stripped.startswith("static "):
            continue
        if stripped.startswith("static const ") or " const " in stripped.split("=")[0]:
            continue
        context = line + " " + (lines[i - 1] if i > 0 else "")
        if any(tok in context for tok in OK):
            continue
        bad.append((i + 1, stripped))

    if bad:
        print("check-fw1-statics: un-annotated mutable `static` inside run_fw_1_test() (#563).")
        print("That function runs CONCURRENTLY once per VM, so a static is shared by every VM and")
        print("every write races. Put it on hype_fw_vm_t (vm->diag), or say at the declaration why")
        print("one-per-host is intended.")
        for ln, text in bad:
            print(f"  {PATH}:{ln}: {text}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
