# Boot 39 -- systematic VM-exit coverage. One ticket, and a short boot.

Queued behind the storage runs. This one is quick and needs nothing rebuilt.

## Config

`suite-603.cfg`. It needs **four physical cores after BSP reservation** -- vmexit=2,
vmexitstorm=1, hello=1. The earlier failure to admit `hello` was never a hype bug: that
sandbox and the AMD laptop exposed only three, and admission control was correctly refusing an
over-subscription. The 5950X has the cores.

## The one

| Ticket | What to read | Passes when |
| --- | --- | --- |
| **#603** | the microtest's coverage table against its own counters | every intercepted exit reason is reached and handled correctly on the SVM leg, with the table in the test's header comment matching what the run produced |

The deliverable is systematic reachability plus correctness of the dispatch table, not
adversarial decode fuzzing -- that belongs to the fuzz-harness ticket and is explicitly a
non-goal here.

## Why it is worth its own boot

Every intercept hype adds needs an obvious place to claim, and today there is no run that says
which exit reasons are actually exercised. A gap found here is a gap that has been invisible;
a gap found later is one that shipped.
