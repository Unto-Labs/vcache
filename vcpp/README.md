# vcpp

A preprocessor for compilation caching. **Licensed GPL-3.0-or-later — not
Apache-2.0 like the rest of this repository.** See [Licence](#licence).

Status: **byte-identical to `gcc -E`** on every file tried so far — all 242 C
files of git 2.55.0 and the bundled corpus, with no declines and no errors. It
is not yet *faster* than `gcc -E`, because none of the reductions that motivate
it are implemented: what exists today is a faithful port of gcc's `-E` driver
onto stock libcpp, which is the correctness baseline the optimisation work
starts from.

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

Floor, from the profile: lexing, expansion and interning together are about 12%
of the current cost, so **4–6×** is the target. A hit would go from 189 ms to
60–80 ms.

## Where it stands

Measured on git 2.55.0's `http.c` (5.9 MB of preprocessed output), best of ten:

| | wall | instructions |
| --- | --- | --- |
| `gcc -E` | 0.13 s | 1.46 G |
| vcpp | 0.17 s | 1.87 G |
| vcpp, libcpp built with `-flto` | 0.16 s | 1.77 G |

vcpp is currently *slower*. Part of that is the comparison: Ubuntu builds `cc1`
with LTO and PGO, and rebuilding libcpp with plain LTO closes about a fifth of
the gap on its own. The rest has not been chased down, because it is not what
the project is about — the reductions below are worth far more than the
remainder.

A profile of vcpp on the same file says the thesis survived contact:

| | share |
| --- | --- |
| `linemap_lookup_macro_index` | 19.6% |
| other source-location machinery | 18.1% |
| `cpp_get_token_1` | 4.5% |
| `_cpp_lex_direct` | 3.9% |
| `enter_macro_context` | 2.9% |

**Source-location tracking is 37.7% of the run**, and the single hottest
function is the one that answers "which macro expansion did this token come
from" — a question a cache never asks. That is the work vcpp exists to delete,
and it is still all there, because deleting it is the next phase and not this
one.

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
