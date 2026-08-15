# The preprocessor problem, verified

The plan behind vcache made four claims about how absolute paths leak into
compiler output, and asked for them to be double-checked. This document records
what was actually measured, on **gcc 13.3.0 (Ubuntu 24.04)** and
**rustc 1.95.0**, along with two behaviours the plan did not mention that turn
out to matter more than the original four.

Every command here is reproducible; the conclusions are what `tests/` encodes.

## The four original claims

### 1. The compilation directory is embedded — CONFIRMED, with a precise trigger

`-fworking-directory` is implied by `-g`. It makes the preprocessor emit the
current directory as a linemarker on line 2:

```console
$ cd /tmp/buildA && g++ -g -E -I ../src ../src/main.cc | head -2
# 0 "../src/main.cc"
# 1 "/tmp/buildA//"          <-- the build directory, verbatim
```

With `-fno-working-directory` the line disappears entirely. Note the trailing
double slash — that is gcc's own formatting, not a typo.

This matters even when every path on the command line is relative, which is
exactly the configuration ccache's documentation recommends. Two people building
the same relative-path tree in different directories still get different
preprocessed text.

### 2. Include paths appear in linemarkers — CONFIRMED

```console
$ g++ -E -I /a/proj/inc /a/proj/src/main.cc | grep '^#'
# 1 "/a/proj/src/main.cc"
# 1 "/a/proj/inc/hdr.h" 1
```

The path is whatever the driver resolved, so absolute `-I` flags — which is what
CMake, Meson and autotools all generate — produce absolute linemarkers.

### 3. `__FILE__` expands to the same paths — CONFIRMED

```console
$ g++ -E -I /a/proj/inc /a/proj/src/main.cc | grep hdr_where
inline const char* hdr_where() { return "/a/proj/inc/hdr.h"; }
```

### 4. Debug info records paths — CONFIRMED

```console
$ g++ -g -c /a/proj/src/main.cc -o main.o
$ readelf --debug-dump=info main.o | grep -E 'comp_dir|DW_AT_name' | head -2
DW_AT_name     : /a/proj/src/main.cc
DW_AT_comp_dir : /a/proj
```

`DW_AT_comp_dir` is the **current working directory**, not the source directory.
This is why vcache maps the build directory as well as the source tree.

## What actually fixes it

Injecting `-ffile-prefix-map=<root>=<canonical>` makes object files
**bit-identical** across directories:

```console
$ cd /a/proj && g++ -g -ffile-prefix-map=/a/proj=/vc -c src/main.cc -o /tmp/A.o
$ cd /b/proj && g++ -g -ffile-prefix-map=/b/proj=/vc -c src/main.cc -o /tmp/B.o
$ cmp /tmp/A.o /tmp/B.o && echo IDENTICAL
IDENTICAL
```

This is the whole basis of vcache. `-ffile-prefix-map` implies both
`-fdebug-prefix-map` (claim 4) and `-fmacro-prefix-map` (claim 3).

## Two behaviours the plan did not anticipate

These are the ones that actually cost the most engineering effort, and both were
found only by measuring rather than by reading documentation.

### A. `-ffile-prefix-map` does **not** rewrite `-E` linemarkers

This is the surprising one. The flag fixes `__FILE__` and debug info, but the
`# N "path"` linemarkers in preprocessed output keep their original absolute
paths:

```console
$ g++ -g -ffile-prefix-map=/a/proj=/vc -fno-working-directory \
      -E -I /a/proj/inc /a/proj/src/main.cc | head -3
# 0 "/a/proj/src/main.cc"        <-- NOT remapped
# 0 "<built-in>"
...
inline const char* hdr_where() { return "/vc/inc/hdr.h"; }   <-- remapped
```

So a cache that hashes raw preprocessed text still misses on every directory
change, even with prefix maps applied — the objects are identical but the keys
are not. **This single fact is why a straightforward "run `-E`, hash the bytes"
implementation cannot work.**

Dropping linemarkers with `-P` is not a way out: they determine the debug line
table, so two inputs differing only in linemarkers really can produce different
objects.

vcache therefore rewrites the path inside each linemarker through the root
mapping as it streams the preprocessed text into the hash
(`src/core/preprocessed.cc`).

### B. Dependency files are deliberately not remapped

Neither gcc's `-MD` output nor rustc's `--emit=dep-info` is affected by prefix
mapping, and that is correct behaviour — `make` needs paths that exist on this
machine:

```console
$ g++ -ffile-prefix-map=/a/proj=/vc -MD -MF dep.d -c /a/proj/src/main.cc
$ cat dep.d
main.o: /a/proj/src/main.cc /a/proj/inc/hdr.h    <-- still local
```

A `.d` file is therefore the one cached artifact that is inherently
directory-specific. vcache canonicalises it on store and reverse-maps it on
restore (`src/core/depfile.cc`).

There is a second, subtler issue here. vcache compiles into a temporary
directory so a failed build cannot leave a half-written object in place — which
means gcc would name that temporary as the dependency rule's target. vcache
passes an explicit `-MT` and rewrites the target on restore, since the output
path is deliberately not part of the cache key.

## Rust behaves identically

`--remap-path-prefix` is the exact analogue of `-ffile-prefix-map`:

```console
$ rustc --remap-path-prefix=/a/crate=/vc -C debuginfo=2 \
        --emit=link --out-dir outA /a/crate/src/lib.rs
$ rustc --remap-path-prefix=/b/crate=/vc -C debuginfo=2 \
        --emit=link --out-dir outB /b/crate/src/lib.rs
$ cmp outA/libdemo.rlib outB/libdemo.rlib && echo IDENTICAL
IDENTICAL
```

`file!()` is remapped, and dep-info is not — the same split as C/C++.

## Overlapping roots: last match wins

When several `-ffile-prefix-map` flags match a path, gcc applies the **last**
one given:

```console
$ g++ -E -ffile-prefix-map=/a=/GEN -ffile-prefix-map=/a/b=/SPEC ... 
/SPEC/inc/hdr.h        # the later, more specific flag won

$ g++ -E -ffile-prefix-map=/a/b=/SPEC -ffile-prefix-map=/a=/GEN ...
/GEN/b/inc/hdr.h       # the later, more general flag won
```

For "most specific root wins" — the intuitive semantics — the flags must be
emitted **most general first**. `RootMap::PrefixMapArgs` sorts by ascending path
length to guarantee this, and `RootMap::Canonicalize` applies the mirror-image
rule in-process so the two never disagree.

## Why the ccache/sccache workaround is restrictive

The documented way to get cross-directory hits from ccache requires *all* of:

- building from the top of the source tree,
- all include paths relative to that top,
- passing `-ffile-prefix-map` yourself and telling ccache to ignore it,
- configuring `gdb` with a substitute path to debug the result.

The first two are the binding constraints: out-of-tree builds and absolute `-I`
flags are what every mainstream build generator produces. vcache instead takes
ownership of the mapping — it strips incoming prefix-map flags, derives its own
from configured roots, covers the build directory as well as the source tree,
and normalises the two artifacts the compiler leaves un-remapped.

## Debugging cached output

Because paths are rewritten, a debugger needs to be told where the sources
really are:

```
(gdb) set substitute-path /vcache/proj /home/you/proj
```

or for lldb:

```
(lldb) settings set target.source-map /vcache/proj /home/you/proj
```

Choosing a canonical target that is a plausible absolute path
(`--vcache-root=/home/you/proj=/usr/src/proj`) works too, if you prefer to point
a single shared location at the sources instead.
