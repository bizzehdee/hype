# Boot 40 -- Intel i5-13420H. APICv, paired A/B. Two tickets, one gated on the other.

**Different machine.** The Intel box, not the 5950X. Its only NVMe is the user's BitLocker
Windows install -- never a write target, on this run or any other.

## The pairing, and why it has to be a pairing

Two boots from the **identical tree**, the same config, differing only in
`-DHYPE_ENABLE_APICV=1`. `make clean` between them: `make` ignores an `EXTRA_CFLAGS` change on
an unchanged mtime, so without it the second build relinks nothing and you stage two copies of
the same binary under different names. Gate every run on the banner sha AND the echoed flag.

## The two

| Ticket | What to read | Passes when |
| --- | --- | --- |
| **#599** | the APICv build reaching a login prompt | it boots at all. Boot 2c hung in kernel boot against a clean Boot 2a from the identical tree (`976e71a-dirty` both), with only the flag differing. **That hang is the open work.** |
| **#605** | the same run, plus `VECSTAT` delivery counts | a PASS flips the Intel default to ON, per plan.md decision 58. #600 was the AMD half |

## Read this before scheduling it

**#605 cannot pass while #599's hang is open.** If the APICv build still hangs, this boot is a
#599 diagnostic run and nothing else -- do not record it as a failed #605 gate, because #605
has not been tested, only blocked.

That makes this the one run in the queue whose value depends on work that has not been done
yet. It is listed here so the Intel trip is planned around it, not so it is booted today.
