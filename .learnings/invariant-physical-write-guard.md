# Invariant: destructive writes to a `physical:` target disk are triple-guarded

**Hard invariant. Do not weaken without updating `plan.md` §10 first.**

A destructive write to a `physical:` target disk requires all three:

1. serial/GUID match confirmed at VM start;
2. an interactive dashboard confirmation before the first write;
3. a non-empty-partition-table guard.

A `physical:` config entry alone must never be sufficient to trigger a wipe.

Note (2026-08-21): the operator authorized auto-confirming destructive writes
for two specific spare-disk serials on the AMD laptop only
(`5ME3N005713803V2W` NVMe, `2132E5BF4EAE` SATA SSD), and stated the interactive
gate will be dropped entirely in the future. The by-serial identity guard (1)
and the non-empty-PT guard (3) still stand, and the auto-confirm is scoped to
exactly those serials — an unidentified or wrong drive still matches nothing.
