# Link caching in vcache — implementation plan

Branch `vlad/link-cache`. Design rationale and measurements:
`~/agent-backup/vcache-link-caching-20260830/DESIGN.md`.

## Why this is tractable

- A link is deterministic: two LTO links of identical inputs produced a
  byte-identical binary (measured).
- The expensive case is worth it: 2.27 s wall / 8.8 s CPU for a 25-object LTO
  link, versus a file copy on a hit.
- The inputs are files with content; no preprocessing step is needed.

## The correctness condition

A hit is sound only when BOTH hold:

1. every input recorded as read still hashes the same, and
2. every path recorded as probed-and-absent is still absent.

(2) is not optional. `-lfoo` resolves to `/usr/lib/libfoo.so` only because
`/opt/lib/libfoo.so` was absent; install that later and a positive-only
manifest hits and links the wrong library.

Validating (2) is cheap and needs no cleverness: re-statting all 243 negative
paths from a real LTO link takes **0.454 ms** against a 2270 ms link. A
directory-mtime token would save 0.31 ms and trade away soundness, so it is
deliberately not done.

## Checklist

- [x] 1. Link command-line parsing (`src/args/link_args.{h,cc}`)
- [x] 2. LD_PRELOAD tracer (`src/trace/fstrace.c`), built by the Makefile
- [x] 3. Trace parsing and classification (`src/core/link_trace.{h,cc}`)
- [x] 4. Manifest: serialise/validate -- folded into `core/link.cc` rather than
      its own file; it is 60 lines and only one caller will ever have it
- [x] 5. Key computation and the run path (`src/core/link.{h,cc}`)
- [x] 6. Dispatch from `main.cc`; reuses the existing counters
- [x] 7. Unit tests (parse + trace classification)
- [x] 8. Integration tests, including a real cross-directory link
- [x] 9. Security audit -- two issues found and fixed
- [x] 10. Docs
- [ ] 11. PR + critical loop

## Result

The link that motivated this, `g++ -flto=auto` over 25 objects:

```
cold (miss)   2.43 s
warm (hit)    0.06 s      byte-identical output
```

Suites: unit 343, integration 202, 0 failed.

## Reuse rather than reinvent

- `RootMap` for canonicalising every path that enters a key.
- The dep-scan manifest pattern: several remembered states per key, validated by
  re-hashing, nothing trusted on mtime.
- `storage::CacheChain` and `Blob` for storage; `LinkOrCopy` for materialising
  outputs (reflink-cheap on this host's ZFS).
- `CompilerId` for toolchain identity, already canonicalised through roots.

## Deliberately out of scope for this branch

Remote tier for link outputs, macOS support, non-ELF targets.
