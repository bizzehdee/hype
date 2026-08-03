# §6k input scripts

Drop these on the ESP as `\input\vm0.txt` / `\input\vm1.txt`. See plan.md §6k for the
language; `core/input_script.c` is the parser and `core/input_runner.c` the runner.

## isolation-vm0.txt / isolation-vm1.txt — INPUT-10 (#283), closes #274's gap

Each guest writes a distinctly-named file, waits, lists `/tmp`, and asserts it can see
**its own** file and **not** the other guest's.

Three details are load-bearing:

* **`fail-if` is armed FIRST**, before the login expect, so it is live for the entire
  run. Armed late it would only cover the tail, and the other guest's marker can show
  up at any point.
* **The `delay 15000` is not padding.** It gives the other guest time to write its own
  marker, so a shared filesystem would definitely expose it by the time `ls` runs.
  Without the delay a pass could just mean "the other guest hadn't got there yet".
* **`ls /tmp` rather than `cat /tmp/marker`.** With a shared filesystem, `cat` of a
  single shared path shows whichever guest wrote last, so it is a race; listing the
  directory shows BOTH names deterministically.

Verified on Intel (nested VMX, two guests, one boot):

    fw-1 SCRIPT vm0: PASS pass (11 directive(s), 56277ms)  at line 15: isolation-vm0
    fw-1 SCRIPT vm1: PASS pass (11 directive(s), 56270ms)  at line 12: isolation-vm1
    fw-1 ISOLATION: isolated -- vm0 ram@0x37e00000 root=0x14005a000
                              | vm1 ram@0x17fe00000 root=0x140091000 (flags=0x0)

and the discrimination confirmed rather than assumed -- vm0's console mentions
`vm1-only-file` zero times, vm1's mentions `vm0-only-file` zero times.
