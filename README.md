# vcache

A compilation cache for C, C++ and Rust that solves the cross-directory problem
head-on.

`ccache` and `sccache` both cache well when you build the same tree, from the
same directory, twice. Move the checkout — a second worktree, a CI runner, a
colleague's machine — and the hit rate collapses, because absolute paths are
baked into preprocessed output, `__FILE__`, and debug info. Their documented
workaround requires building from the top of the source tree with only relative
include paths, which is not what CMake, Meson or Cargo generate.

vcache takes ownership of path rewriting instead. You declare your source roots;
vcache canonicalises them on the compiler command line and normalises the two
artifacts the compiler leaves un-remapped. The result:

```console
$ cd /work/checkout-one && vcache g++ -g -O2 -c -I include src/lib.cc -o lib.o
$ cd /tmp/checkout-two  && vcache g++ -g -O2 -c -I include src/lib.cc -o lib.o
   ^ cache hit, and the two lib.o files are byte-identical
```

Building vcache itself from two unrelated directories: **25 of 25 objects served
from cache, and the linked binaries are byte-identical** (4.5s → 0.77s).

## What the v stands for

Either, depending on how generous you are feeling:

- **V**lad's cache. This is the honest one.
- **V**irtuous **C**ompile **A**voidance, **C**onscientiously **H**ashing
  **E**verything. This is the one that fits on a slide.

The backronym is a joke that turns out to describe the design. Avoiding a
compile is the entire point, and vcache is conscientious to a fault about
earning it: it never trusts an mtime, it re-hashes every file a cached
dependency scan claims to have read, and it checksums every entry so a corrupt
one reads as a miss instead of a bad object. That costs a preprocessor run on
every lookup, which is why a vcache hit is slower than a ccache hit — and why it
is a hit at all when a ccache hit would not have been.

## Documentation

| | |
| --- | --- |
| [docs/quickstart.md](docs/quickstart.md) | Zero to cross-directory hits in five minutes |
| [docs/configuration.md](docs/configuration.md) | Every option, precedence, worked examples |
| [docs/preprocessor-problem.md](docs/preprocessor-problem.md) | Why this is needed, with measurements |
| [docs/design.md](docs/design.md) | How it is put together |

## Quick start

```console
$ ./third-party/fetch.sh     # builds tcmalloc (one time)
$ make -j
$ make test
```

Then either call it explicitly:

```console
$ vcache g++ -c foo.cc -o foo.o
```

or drop symlinks named after your compilers early on `$PATH`:

```console
$ ln -s /path/to/vcache ~/.local/libexec/vcache/g++
$ ln -s /path/to/vcache ~/.local/libexec/vcache/gcc
$ ln -s /path/to/vcache ~/.local/libexec/vcache/rustc
$ export PATH=~/.local/libexec/vcache:$PATH
```

## Configuring roots

A root is a directory whose location should not affect the cache key. Almost
always: your source tree.

```console
$ export VCACHE_ROOTS=/home/you/myproject
$ vcache g++ -c src/a.cc -o a.o
```

`/home/you/myproject` is rewritten to `/vcache/myproject` everywhere it appears.
On another machine, `export VCACHE_ROOTS=/build/myproject` maps to the same
canonical prefix, and the two share cache entries.

Roots may be given as `PATH`, `PATH=name` (→ `/vcache/name`), or
`PATH=/absolute/target` to choose the canonical prefix outright. They can be
repeated, come from `--vcache-root=`, `VCACHE_ROOTS` (colon-separated), or the
config file.

vcache also maps the **current directory** by default, because `DW_AT_comp_dir`
records the build directory rather than the source directory. Disable with
`VCACHE_MAP_CWD=0` if you have a reason to.

Check what a mapping resolves to:

```console
$ VCACHE_ROOTS=/home/you/myproject vcache --show-roots
/home/you/myproject -> /vcache/myproject
/home/you/myproject/build -> /vcache/cwd
```

## Prefix-map flags from your build system

If the command line already contains `-ffile-prefix-map`, `-fdebug-prefix-map`,
`-fmacro-prefix-map` or `--remap-path-prefix`, vcache **refuses to run**:

```console
$ vcache g++ -ffile-prefix-map=/build=/usr/src -c a.cc -o a.o
vcache: refusing to run: the command line contains -ffile-prefix-map=/build=/usr/src
vcache manages path prefix mapping itself, and combining the two would silently change
which paths end up in your output. Choose one:
  --vcache-allow-prefix-maps        drop them; vcache's mapping wins
  --vcache-incoming-prefix-maps=keep pass them through (disables caching)
```

Silently overriding a mapping the build deliberately asked for would change its
output without saying so, and gcc's last-match-wins resolution makes the
combination genuinely ambiguous. Override deliberately, by any of:

```console
$ vcache --vcache-allow-prefix-maps g++ ...          # command line
$ export VCACHE_INCOMING_PREFIX_MAPS=strip           # environment
```
```toml
[vcache]
incoming_prefix_maps = "strip"   # error (default) | strip | keep
```

Precedence is command line > environment > config file. `keep` passes the flags
through untouched and marks the compilation uncacheable.

## Configuration file

`$VCACHE_CONFIG`, else `~/.config/vcache/config.toml`, else
`/etc/vcache/config.toml`. The layout follows sccache's.

```toml
[vcache]
roots = ["/home/you/myproject", "/opt/toolchain=toolchain"]
map_cwd = true
incoming_prefix_maps = "error"   # error (default) | strip | keep
hash_env_vars = ["SOURCE_DATE_EPOCH"]

[cache.disk]
dir = "~/.cache/vcache"
size = "20G"

[cache.s3]
bucket = "my-build-cache"
region = "us-west-2"
prefix = "vcache/"
# endpoint = "http://minio.internal:9000"   # for MinIO/Ceph
# path_style = true
```

Environment variables override the file, and command-line flags override those.
[docs/configuration.md](docs/configuration.md) lists every key, its environment
and command-line equivalents, and its default.

## Cache layers

Reads walk local disk first, then S3, and stop at the first hit. A remote hit is
promoted into the local layer so the rest of the build serves it locally. Writes
go to every writable layer. If S3 is unreachable, lookups degrade to misses and
the build proceeds — a shared cache outage never breaks a build.

## Commands

| Command | Effect |
| --- | --- |
| `vcache --show-stats` | hit/miss counters and cache size |
| `vcache --zero-stats` | reset counters |
| `vcache --clear` | delete all entries |
| `vcache --trim` | evict until under the size limit |
| `vcache --show-config` | effective configuration |
| `vcache --show-roots` | resolved root mapping for this directory |

## Debugging cached builds

Paths in debug info are canonical, so point your debugger at the real sources:

```
(gdb) set substitute-path /vcache/myproject /home/you/myproject
(lldb) settings set target.source-map /vcache/myproject /home/you/myproject
```

## Performance

Measured on one translation unit from this repository (gcc 13.3, `-O2 -g`,
warm page cache, median of three):

| | per file |
| --- | --- |
| plain `g++` | 1.50 s |
| vcache hit | 0.073 s |
| ccache hit (direct mode) | 0.009 s |

A vcache hit is ~19× faster than compiling, and ~8× slower than a ccache hit,
because vcache always runs the preprocessor while ccache's direct mode skips it
by hashing the source plus a stored manifest of includes.

The trade is deliberate: vcache competes on **hit rate**, not per-hit latency. A
ccache hit is faster, but only when ccache hits at all — move the checkout and it
does not. 73 ms against a 1.5 s compile is comfortably on the right side of the
line.

## Runtime dependencies

```console
$ ldd bin/vcache
	linux-vdso.so.1
	libm.so.6        (tcmalloc uses log2)
	libc.so.6
	/lib64/ld-linux-x86-64.so.2
```

That is the whole list. vcache is spawned once per compilation, so every
`DT_NEEDED` entry is mapped and relocated on every invocation — linking libcurl
directly pulled in about thirty shared objects (TLS, HTTP/2, Kerberos, LDAP,
SSH, IDN, compression) and cost ~2.4 ms of startup on builds that never make a
request. Two changes removed them:

- **libcurl is `dlopen`d**, only when an S3 layer is actually constructed. A
  disk-only build never maps it; if it is missing when S3 *is* configured,
  vcache says so and continues with the local cache.
- **SHA-256 and HMAC are vendored** (`src/hash/sha256.cc`). SigV4 was the only
  use of OpenSSL — three functions — which is a thin reason to link it.

Process startup went from 6.1 ms to 3.0 ms as a result.

## What is not cached

Linking, `-E`-only runs, `-MG`, multiple inputs in one invocation,
`-save-temps`, PGO flags, and `rustc` without `--out-dir`/`--emit`. All of these
fall through to the compiler unchanged, so a build always makes progress.
`vcache --show-stats` counts them as *uncacheable*.

Two things that look like they belong on that list but are cached:

- **`-march=native`.** The flag means something different on every machine, so
  vcache asks the compiler which target it resolved to and puts *that* in the
  key — see [`native_target`](docs/configuration.md#native_target).
- **`-M`/`-MM` dependency scans.** Keying those on preprocessed text would cost
  more than the run being cached, so they are keyed on the command line and
  verified against a manifest of the files they read — see
  [`dep_scan`](docs/configuration.md#dep_scan).

## How it works

1. Parse the command line; decide whether it is cacheable at all.
2. Refuse if the caller supplied `-ffile-prefix-map` / `--remap-path-prefix`,
   unless overridden; vcache owns path rewriting.
3. Derive `-ffile-prefix-map` flags from the configured roots, ordered so the
   most specific root wins under gcc's last-match-wins rule.
4. Preprocess with those flags plus `-fno-working-directory`, then hash the
   output **with linemarker paths rewritten** — gcc does not remap those itself.
5. Hash alongside it: compiler identity, codegen flags (never `-I`/`-D`, whose
   effect is already in the preprocessed text, and never `-o`), and the
   canonical root targets.
6. Look up; on a hit, write the object, replay diagnostics, and reverse-map the
   dependency file into local paths.
7. On a miss, compile into a temporary directory, place the artifacts, and store.

Rust follows the same shape, with `--emit=dep-info` standing in for
preprocessing and `--extern` dependencies hashed by content rather than path.

`docs/preprocessor-problem.md` records the measurements this design rests on,
including the two compiler behaviours that make the naive approach fail.

## Build configuration

Release builds use `-O3 -ggdb3`, LTO, and zstd-compressed debug info. Both LTO
and `-gz=zstd` are probed for at configure time, so a toolchain without them
still builds — just larger. `make BUILD=debug` gives `-O0 -ggdb3` with no LTO.

Effect on the shipped binary, which keeps full `-ggdb3` debug info throughout:

| configuration | size |
| --- | --- |
| `-O2 -g`, no LTO, no compression | 12.9 MB |
| `-O3 -ggdb3` + LTO + `--gc-sections` | 9.3 MB |
| the above + `-gz=zstd` | **4.0 MB** |

zstd compression of DWARF is the larger win (−5.3 MB); LTO with
`-ffunction-sections -fdata-sections -Wl,--gc-sections` accounts for −1.9 MB.
LTO on its own tends to *grow* a binary through inlining — the dead-code
elimination is what pays for it. Debug info survives intact: 32 compilation
units, full line tables, and 27,909 macro definitions from `-ggdb3`.

## Implementation notes

- C++20, built with plain `make`.
- Boost.Spirit X3 parses Makefile-syntax dependency files, which have real
  structure (line continuations, escaped spaces, `$$`). The compiler command
  line is a flag list rather than a grammar, so it uses a table-driven scanner;
  preprocessed output is normalised by a streaming scanner because it is a hot
  path over tens of megabytes per compile.
- BLAKE3 for cache keys, tcmalloc for allocation, toml++ for config. S3 uses
  hand-rolled SigV4 over a lazily loaded libcurl, with SHA-256/HMAC vendored —
  no AWS SDK and no OpenSSL.
- Boost is vendored as a 932-header subset (4.2 MB of Boost 1.86.0's 104 MB) —
  exactly what Spirit X3 opens. `make boost-subset` regenerates it against a
  full Boost tree if an include ever reaches further. Only gperftools is
  downloaded at setup time; everything else is committed.
- Statically linked apart from libc and libcurl.
- Cache entries carry a BLAKE3 checksum; a corrupt entry reads as a miss rather
  than yielding a bad object.

## Tests

```console
$ make test
```

129 unit tests and 48 integration tests, covering cross-directory hits,
out-of-tree builds, dependency-file replay, diagnostics replay, uncacheable
fallback, masquerade mode, Rust, cache management, and the S3 layer against a
mock object store. SigV4 is checked against AWS's documented signing-key vector
and an independent reference implementation.
