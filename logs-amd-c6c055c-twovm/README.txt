AMD laptop, build c6c055c (#242 + #237 fixes), 2026-07-28.

HYPEFULL.LOG -- IS from the successful run. Stamp "hype: build c6c055c" matches,
and the stick's previous log was deleted before this build was written, so there
is no stale-capture ambiguity. Contains: 17 "halted cleanly" (the battery), 16
"slot pool EXHAUSTED" warnings (expected: 18 sequential test guests minus 2
slots), and ZERO panics. It stops at 137 lines because the log stream stops once
the guests take over and the operator starts interacting.

hype-diag-prev.txt.FROM-AN-EARLIER-BOOT-NOT-THIS-RUN -- RT-3 stores the PREVIOUS
boot's tail, and the machine was not rebooted after the successful run, so this
file predates it (user-confirmed). Its "vmm_ok=1" is NOT evidence for this run.
Kept only so it is not mistaken for fresh evidence later.

OPERATOR-OBSERVED on this run (the primary evidence, no log line covers it):
  - Right-Ctrl+Alt+Left/Right switches between both VMs
  - a login prompt on BOTH VMs
  - successful login on BOTH VMs
  - a file created in one VM is NOT visible in the other
That last point is the one that matters most: it proves two genuinely independent
guests, not one guest rendered twice.
