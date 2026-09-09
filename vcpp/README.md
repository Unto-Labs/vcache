# vcpp

A preprocessor for compilation caching. **Licensed GPL-3.0-or-later — not
Apache-2.0 like the rest of this repository.** See [Licence](#licence).

Status: **byte-identical to `gcc -E`** on every file tried — all 242 C files of
git 2.55.0 and the bundled corpus, with no declines and no errors — and
**1.11× faster** overall, 1.18× on the largest translation units.

That is far short of the 4–6× this file used to project, and the shortfall is
not a matter of unfinished work. Both reductions the project was betting on have
now been measured and neither is available while the output has to match gcc
byte for byte. See [What the speedup ceiling actually is](#what-the-speedup-ceiling-actually-is).

## Why it exists

vcache decides whether two compilations are the same one by preprocessing and
hashing the result. That is what makes a hit trustworthy — the preprocessed text
is a complete description of what the compiler is about to see — and it is also
the dominant cost of a hit.

Measured on a Linux kernel translation unit (4.8 MB of preprocessed output,
gcc 13.3, best of five):

| | ms |
| --- | --- |
| `gcc -E` | 166 |
| a vcache hit | 189 |
| a full compile | 558 |

**Preprocessing is 87.6% of a cache hit.** Across a whole kernel build it is at
least 45% of all CPU, and `cc1` is busy for 92% of the wall clock even though
99.91% of compilations hit the cache. vcache's own work — hashing, lookup,
writing the object — is 6.4%.

## Why a new one rather than a faster gcc

gcc's preprocessor is not badly written. On identical work producing identical
output, clang is *slower*: 212 ms against gcc's 186, executing more
instructions. Two mature implementations landing within 15% of each other is
what an inherent cost looks like.

What is not inherent is *how much of that work a cache needs*. A profile of
19,202 samples over `gcc -E`:

| | share |
| --- | --- |
| kernel — page faults, I/O | 24.3% |
| source-location tracking (`linemap*`) | 20.5% |
| allocation | 10.6% |
| lexing | 9.2% |
| formatting the output text | 6.4% |
| identifier interning | 2.3% |
| macro expansion | 0.02% |

The hottest single function is `linemap_lookup_macro_index`, at 8.7%. gcc
tracks, for every token a macro produces, both where it sits in the macro's
definition and where the macro was invoked, so a diagnostic can point inside a
macro and add "in expansion of macro". `-ftrack-macro-expansion=0` turns most of
that off and takes the same file from 194 ms to 134 — **31% for one flag**.

vcache cannot simply pass that flag. It changes the output: on 3 of 150 kernel
translation units, `__LINE__` reached through the argument of a *multi-line*
macro invocation reports a different line, and that number is compiled into the
object. Hashing text the compiler will not see breaks the reasoning that makes a
hit sound.

But a preprocessor written for this job needs only one integer per macro-body
token to keep `__LINE__` conformant — not a queryable expansion history. That,
plus an arena allocator, hashing tokens instead of formatting 4.8 MB of text,
and indexing the include path once rather than issuing 1,454 failing `openat`
calls, is what vcpp is.

Whether any of that is reachable is answered above, and the answer is mostly no:
the provenance a cache "never asks for" turns out to be what gcc's own
linemarkers are built on.

## Where it stands

`test/bench.sh --tree` on git 2.55.0's six largest translation units, best of
fifteen. It refuses to report a time for any file whose output does not match,
because a speedup that changes the text is not a speedup:

| | wall | instructions |
| --- | --- | --- |
| `gcc -E` | 0.42 s | 4.44 G |
| vcpp, first working version | 0.61 s | 5.78 G |
| vcpp now | **0.38 s** | 4.46 G |

Three changes got it there, and the largest was not a clever one. Caching
location resolution — one memo covering a whole macro map, since every token an
expansion produces resolves to the same expansion point — was worth about 12%.
Bulk-writing identifiers and turning off stdio locking, a few percent more. But
the big one was that `line_table->default_range_bits` was left at zero where
gcc sets it to 5, so every single token was allocating an ad-hoc location in a
side hash table. That one line was worth more than everything else combined.

## What the speedup ceiling actually is

This file used to argue for 4–6×, on the grounds that source-location tracking
was 37.7% of the run and a cache never needs to ask which macro expansion a
token came from. The first half was right. The second half was wrong, and it is
worth being precise about why, because it is the project's go/no-go question.

**Turning off macro-expansion tracking does not merely perturb `__LINE__`.**
`-ftrack-macro-expansion=0` is fast — 0.88 G instructions against 1.28 G on
`http.c`, which would be 1.86× against gcc — and it produces the wrong text on
**every one of git's 242 files**, by 1060 lines on one 28k-line file alone. The
reason is not `__LINE__`. It is that gcc emits a linemarker whenever a token's
system-header-ness changes, and that includes crossing into and out of a macro
argument:

```c
 gettimeofday(&tv,
# 204 "compat/posix.h" 3 4
                  ((void *)0)
# 204 "compat/posix.h"
                      );
```

`NULL` comes from a system header, the call around it does not, and recording
that needs a per-token spelling location — exactly the expansion history that
`=0` throws away. `=1` keeps enough for `__LINE__` (it gets the multi-line
macro-argument case right) but still loses the per-token definition location,
and so still gets 171 of 242 files wrong.

`__LINE__` alone would have been survivable, and it is a real difference —
`ID(
 __LINE__)` gives 3 under gcc and 2 under `=0`. The linemarkers are not
survivable, and there is no cheap guard for them, because they are pervasive
rather than rare.

**Hashing instead of formatting is worth 8.6%, not a multiple.** vcache needs a
key, not the text, so the obvious move is to skip writing 5.9 MB. Measured by
keeping every decision — spacing, linemarkers, every location query — and
discarding only the bytes: 1.28 G instructions becomes 1.17 G. The formatting
was never the expense. Deciding *what* to format is, and a key has to encode the
same decisions the text does, so it cannot skip them.

**A faster allocator does not help either, and a linear one is worse.** This was
the third hypothesis and the most tempting: a preprocessor is short-lived, so
give it an arena on huge pages and make `free` a no-op. Measured, preloaded over
glibc on `http.c`:

| | instructions | faults | peak RSS |
| --- | --- | --- | --- |
| glibc | 1.28 G | 5463 | 23 MB |
| arena, no-op `free` | 1.88 G | 1397 | 573 MB |

Slower, and the RSS says why: libcpp's peak live set is 23 MB but it allocates
573 MB over the run. It churns — macro expansion buffers come and go constantly
— so glibc spends its time recycling 23 MB while the arena spends its time
zeroing 573 MB of fresh pages. Kernel time went from about 4% to 18.7%. Huge
pages cut the fault count fourfold and did not save it, because the cost is
zeroing, not faulting.

The experiment is kept in `test/bump-alloc.c`, along with the two traps that
have to be avoided before it is even that close — a bump allocator that always
copies on `realloc` makes libcpp's buffer growth O(n²) and is *4×* slower, and
a `calloc` that memsets arena memory re-zeroes pages the kernel already zeroed.

Underneath all of it: allocation is only about 6% of the profile to begin with,
because libcpp already pools through its own `_cpp_buff` instead of calling
malloc per token. There was never 4× hiding there.

**Mapping the input files instead of reading them would be slower too.** A run
on `http.c` does 430 `read()`s totalling 3.77 MB and 1417 `openat()`s, 988 of
which fail as the include search tries each directory in turn. Measured
directly, against a ~100 ms run, with warm caches — which is what a repeat build
sees:

| | | |
| --- | --- | --- |
| 988 failed `open()` | 1.45 ms | 1.4% |
| 3.77 MB via `read()` | 0.55 ms | 0.5% |
| 3.77 MB via `mmap()` | 0.85 ms | 0.8% |

Faulting in 920 pages costs more than one memcpy out of a warm page cache, so
the map loses by about 0.3 ms. Same direction as the allocator: page faults beat
copying only when the copy is large and cold.

libcpp could not take a mapping as it stands, either. `read_file_guts` allocates
`st_size + 16` because the SSE4 lexer reads aligned 16-byte chunks past the end
of the content and stops on a `'\n'` sentinel libcpp writes into the buffer, and
`_cpp_clean_line` then rewrites the buffer in place for backslash-newline
splicing. That needs a writable private mapping with a guaranteed writable tail
page, and would fault on a file whose size is an exact multiple of the page size.
gcc removed mmap from libcpp years ago and there is no trace of it left.

The failed opens are the larger of the two and still only 1.4%, and removing them
means indexing the include directories instead of probing them — trading a
snapshot for a syscall, which is a semantic change for about a percent.
`test/io-cost.c` has the measurement.

What remains is genuine work that byte-identity requires. `in_system_header_at`
alone is 22.5% of the run, measured by stubbing it out, and it resists memoising:
the obvious cache on the unwound spelling location hits 8.3% of the time,
because most macro tokens turn out to be argument tokens whose spelling location
is unique to that use rather than body tokens sharing a definition site.

So the honest number is 1.1–1.2×, not 4–6×. Preprocessing is 87.6% of a cache
hit, so that is roughly an 8% faster hit. **Whether that justifies maintaining a
GPLv3 fork of libcpp is a judgement call, and it should be made on this number
rather than on the projection this file used to carry.**

The larger win, if one exists, is not inside vcpp: it is for vcache to stop
needing byte-identical text at all — to key on something coarser that is still
sound. That is a change to vcache's soundness argument, not to its preprocessor,
and it is not one to make casually.

## Approach

vcpp is a front end for GCC's `libcpp`, not a reimplementation. The reason is
conformance: matching gcc's `-E` output means matching its exact choices, and
those are not obvious. clang's output differs from gcc's **from the third byte**
and in 1,347 lines on a single kernel file. Reimplementing that from scratch is
a conformance project measured in years, where every undiscovered divergence is
a wrong object.

Reducing libcpp is a much smaller problem than rewriting it: every item in the
profile above is a deletion or a substitution, not a new lexer. `_cpp_lex_direct`
is 5.6% and already good — it is what surrounds the lexer that costs.

libcpp is fetched and built by `./fetch-libcpp.sh`, not vendored. It needs
`libiberty`, one x86 header, the top-level `depcomp` and friends its configure
probes for, and four things from its client:

- `fancy_abort` — and it must not call `abort()`, which re-enters libcpp's
  diagnostic path and recurses.
- `linemap_client_expand_location_to_spelling_point`.
- a `diagnostic` callback — `cpp_diagnostic_at` calls `abort()` without one.
- `line_maps::reallocator` and `round_alloc_size` (`m_`-prefixed since gcc 14),
  set **after** `linemap_init`, which placement-news the struct and would wipe
  them.

The `-E` output driver is *not* in libcpp: it lives in
`gcc/c-family/c-ppoutput.cc` and pulls in gcc front-end headers, so it has to be
ported rather than lifted. `src/vcpp.cc` is that port, together with the
preamble sequence from `c_finish_options` in `c-opts.cc` — the `# 0 "<built-in>"`
and `# 0 "<command-line>"` markers, and the `stdc-predef.h` push, are not
incidental output but a specific order of `linemap_add` and `cpp_change_file`
calls that has to be reproduced exactly.

### Which GCC to build on

**The libcpp version has to match the compiler being cached.** An earlier draft
of this file said the opposite — that libcpp's behaviour is stable across
releases and only the predefined macro set differs. That was measured on
predefined macros, where it holds: telling gcc 13 to claim `__GNUC__=14` made
one kernel translation unit byte-identical to gcc 14's output, and only 9 of
401 predefined macros differ. It does not hold in general.

The counterexample was found by building against gcc 16.2.0's libcpp and
watching every system-header linemarker come out wrong:

```
gcc 13:  # 1 "/usr/include/stdc-predef.h" 1 3 4
gcc 16:  # 1 "/usr/include/stdc-predef.h" 1 3
```

`_cpp_stack_file` computes `sysp` as an `int` — `1 + !cxx_aware`, so 2 for a C
system directory — and gcc 16 passes it to `_cpp_post_stack_file`, whose
parameter is a `bool`. The 2 becomes 1 and the `4` flag is lost. It is a real
upstream regression, not a deliberate change: gcc 16's own `c-ppoutput.cc`
still has the `sysp == 2` branch that would print it. Confirmed here by
instrumenting vcpp: the `cpp_dir` carries `sysp=2` and the resulting line map
reports 1.

So `fetch-libcpp.sh` picks the release matching the local `gcc` by default, and
`VCPP_GCC_VERSION` overrides it. `Makefile` reads the version the script
actually built and compiles against that API — libcpp's C++ API is not stable
either (`reallocator` became `m_reallocator`, `cpp_set_include_chains` grew a
chain for `#embed`, `line_maps` gained `cmdline_location`).

### What vcpp has to learn from the target compiler

Three things belong to the compiler rather than to libcpp:

- **Predefined macros.** gcc defines them in `c_cpp_builtins`, in the front end.
  vcpp reads them from `gcc -dM -E`, or from a file given with `--predef=`.
- **The system include chain**, from `gcc -E -v`, or from `-isystem` arguments.
- **`__has_attribute` / `__has_builtin`.** These are libcpp callbacks answered
  out of front-end tables, and cannot be dumped as `-D`. vcpp answers from a
  table built by `tools/gen-has-table.sh` and **fails closed** — exit 3, "no
  answer for `__has_attribute(x)`" — on anything it has not been taught, so
  vcache preprocesses with the real compiler rather than vcpp guessing.

  Harvesting the names literally is not enough. glibc routes every query through
  `#define __glibc_has_attribute(attr) __has_attribute (attr)`, so the name
  libcpp finally sees arrives by macro expansion and appears nowhere near the
  operator. `gen-has-table.sh --learn` closes the gap the way vcache would in
  production: run, take the name it declined on, ask the compiler, repeat. On
  git 2.55.0 that converges in one round.

Registering pragmas matters for the same reason. gcc registers `GCC diagnostic`
with libcpp even when only preprocessing, so it arrives as a `CPP_PRAGMA` token;
an unregistered pragma goes through the `def_pragma` callback instead and leaves
a `CPP_PADDING` behind, which the next token turns into a spurious linemarker.
That one difference accounted for 61 of git's 242 files.

## Building

```console
$ ./fetch-libcpp.sh     # downloads GCC (~107 MB, verified), builds libcpp
$ make
$ make check            # compares against the installed gcc -E
$ ./test/compare.sh --tree /path/to/linux -n 200
```

`make check` is green: 4 of 4 byte-identical. It reports "token stream matches,
formatting differs" and "declined" separately from a pass, because neither is
one.

To check against a real tree:

```console
$ ./tools/gen-has-table.sh -o /tmp/has.tbl /path/to/tree
$ ./bin/vcpp --has-table=/tmp/has.tbl -I/path/to/tree file.c -o out.i
```

## Licence

vcpp is **GPL-3.0-or-later**. The rest of this repository is Apache-2.0.

GCC's `libcpp` is GPLv3 and carries **no** runtime-library exception — that
exception covers GCC's runtime libraries, `libgcc` and `libstdc++`, so the
familiar "GCC components can be linked into anything" intuition does not apply
here. Anything built on libcpp is GPLv3.

That does not affect vcache, because **vcpp is a separate executable**. vcache
runs it the way it already runs gcc, and the GPL does not propagate across
`exec`. Nothing under `vcpp/` is linked into `bin/vcache`.

Two rules follow, and they are the reason this directory is separate:

- Nothing in `vcpp/` may be linked into vcache, and nothing above `vcpp/` may
  include a header from it.
- Shipping a vcpp binary carries the GPL's obligation to offer complete
  corresponding source, including modifications. The release workflow has to
  account for that.

`LICENSE` here is GPLv3. The repository's top-level `LICENSE` and `NOTICE`
remain Apache-2.0 and describe this boundary.
