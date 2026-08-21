# hype's logs are invalid UTF-8 — grep needs LC_ALL=C grep -a

**Backs:** the log-reading note in the `microtests` skill.

## What happened

hype's on-stick logs (`hype.log` and the per-VM guest logs) contain invalid
UTF-8 bytes. A plain `grep` treats the file as text in the current locale and
silently matches **nothing** — it does not error. This produced a wrong "the
probe never ran" conclusion.

## The lesson

- Always read hype's logs with `LC_ALL=C grep -a`. `LC_ALL=C` stops locale-based
  decoding; `-a` forces grep to treat the file as text despite the invalid
  bytes.
- A `grep` that matches nothing on a binary-ish log is not evidence of absence
  until you have re-run it with `LC_ALL=C grep -a`.
