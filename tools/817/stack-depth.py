#!/usr/bin/env python3
"""#817: worst-case stack depth per call chain, from the built objects.

Frame sizes come from clang's own -fstack-usage records; edges come from the direct-call
relocations in the same objects.  Indirect calls (function pointers) are invisible to this,
so a reported depth is a LOWER bound on the worst case, never an upper one.

  make clean && make all EXTRA_CFLAGS=-fstack-usage
  tools/817/stack-depth.py <root-symbol> [...]
"""
import os, re, subprocess, sys, collections

CHECK = "--check" in sys.argv
if CHECK:
    sys.argv.remove("--check")
LIMIT = 0
if "--limit" in sys.argv:
    i = sys.argv.index("--limit")
    LIMIT = int(sys.argv[i + 1], 0)
    del sys.argv[i:i + 2]

BUILD = "build"
frames, edges, defined = {}, collections.defaultdict(set), set()

for root, _, files in os.walk(BUILD):
    for f in files:
        if f.endswith(".su"):
            for line in open(os.path.join(root, f)):
                p = line.rstrip("\n").split("\t")
                if len(p) >= 2:
                    fn = p[0].rsplit(":", 1)[-1]
                    frames[fn] = max(frames.get(fn, 0), int(p[1]))

objdump = os.environ.get("OBJDUMP", "llvm-objdump")
cur = None
for root, _, files in os.walk(BUILD):
    for f in files:
        if not f.endswith(".o"):
            continue
        out = subprocess.run([objdump, "-d", "-r", os.path.join(root, f)],
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            m = re.match(r"^[0-9a-f]{16} <(.+)>:$", line)
            if m:
                cur = m.group(1); defined.add(cur); continue
            m = re.search(r"IMAGE_REL_AMD64_REL32\s+(\S+)", line)
            if m and cur:
                edges[cur].add(m.group(1))

EXCLUDE = set(os.environ.get("EXCLUDE",
    "__stack_chk_fail,hype_fatal,hype_halt_forever,hype_isr_dispatch").split(","))

memo, onstack = {}, set()
def depth(fn, path):
    if fn in onstack:
        return frames.get(fn, 0), path + [fn + " (CYCLE)"]
    if fn in memo:
        d, p = memo[fn]
        return d, path + p
    onstack.add(fn)
    best, bestp = 0, []
    for t in edges.get(fn, ()):
        if t in EXCLUDE:
            continue
        d, p = depth(t, [])
        if d > best:
            best, bestp = d, p
    onstack.discard(fn)
    own = frames.get(fn, 0)
    memo[fn] = (own + best, [fn] + bestp)
    return own + best, path + [fn] + bestp

if CHECK:
    worst = 0
    for rootfn in sys.argv[1:]:
        if rootfn not in defined and rootfn not in frames:
            print(f"check-ap-stack: {rootfn} not found in {BUILD}/ -- "
                  f"build with EXTRA_CFLAGS=-fstack-usage"); sys.exit(1)
        d, path = depth(rootfn, [])
        if d > worst:
            worst, worstp, worstr = d, path, rootfn
    if worst > LIMIT:
        print(f"check-ap-stack: FAIL -- {worstr} can reach {worst} bytes of stack, "
              f"limit {LIMIT}. Deepest chain:")
        run = 0
        for fn in worstp:
            run += frames.get(fn.split(" ")[0], 0)
            print(f"    {frames.get(fn.split(' ')[0], 0):>7}  {run:>7}  {fn}")
        sys.exit(1)
    print(f"check-ap-stack: OK -- worst AP chain {worst} of {LIMIT} bytes "
          f"(direct calls only; indirect calls are not visible)")
    sys.exit(0)

for rootfn in sys.argv[1:]:
    if rootfn not in defined and rootfn not in frames:
        print(f"{rootfn}: not found in {BUILD}"); continue
    d, p = depth(rootfn, [])
    print(f"\n=== {rootfn}: worst direct-call depth {d} bytes "
          f"(excluding {','.join(sorted(EXCLUDE))}) ===")
    run = 0
    for fn in p:
        run += frames.get(fn.split(" ")[0], 0)
        print(f"  {frames.get(fn.split(' ')[0], 0):>7}  {run:>7}  {fn}")

if os.environ.get("TOP"):
    reach, stack = set(), list(sys.argv[1:])
    while stack:
        f = stack.pop()
        if f in reach or f in EXCLUDE:
            continue
        reach.add(f)
        stack.extend(edges.get(f, ()))
    print("\n=== deepest callees reachable from the root(s) ===")
    for fn in sorted(reach, key=lambda x: -depth(x, [])[0])[:int(os.environ["TOP"])]:
        print(f"  {depth(fn, [])[0]:>7}  own {frames.get(fn, 0):>6}  {fn}")

if os.environ.get("PATHTO"):
    tgt = os.environ["PATHTO"]
    seen, q = set(), [(a, [a]) for a in sys.argv[1:]]
    while q:
        f, path = q.pop(0)
        if f == tgt:
            print(f"\n=== shortest call path to {tgt} ===")
            run = 0
            for fn in path:
                run += frames.get(fn, 0)
                print(f"  {frames.get(fn, 0):>7}  {run:>7}  {fn}")
            break
        if f in seen or f in EXCLUDE:
            continue
        seen.add(f)
        for t in edges.get(f, ()):
            q.append((t, path + [t]))
    else:
        print(f"no direct-call path to {tgt}")
