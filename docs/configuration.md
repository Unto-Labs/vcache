# vcache configuration reference

Every setting vcache reads, where it can be set, and what it does.

New to vcache? Start with [quickstart.md](quickstart.md) — this document is a
reference, not an introduction.

## Contents

- [Precedence](#precedence)
- [The config file](#the-config-file)
- [Option summary](#option-summary)
- [Root mapping](#root-mapping)
- [Incoming prefix-map flags](#incoming-prefix-map-flags)
- [Disk cache](#disk-cache)
- [S3 cache](#s3-cache)
- [Cache behaviour switches](#cache-behaviour-switches)
- [Cache key inputs](#cache-key-inputs)
- [Logging and diagnostics](#logging-and-diagnostics)
- [Value formats](#value-formats)
- [What is never cached](#what-is-never-cached)
- [Worked examples](#worked-examples)

## Precedence

Later wins:

1. Built-in defaults
2. Config file
3. Environment variables
4. Command-line flags

Everything is optional. With no configuration at all, vcache caches to
`~/.cache/vcache` with a 10 GiB budget, maps only the current directory, and
refuses command lines that already carry prefix-map flags.

`vcache --show-config` prints the result of resolving all four layers, which is
the fastest way to find out why a setting is not taking effect.

## The config file

Searched in this order; the first that exists is used, and only that one:

1. `$VCACHE_CONFIG`
2. `$XDG_CONFIG_HOME/vcache/config.toml`
3. `~/.config/vcache/config.toml`
4. `/etc/vcache/config.toml`

The layout follows sccache's. A malformed file produces a warning and is
otherwise ignored — vcache falls back to defaults rather than failing a build
over a config typo. Unknown keys are ignored silently; unknown *values* for
enumerated keys produce a warning.

A complete file, with every key shown at its default:

```toml
[vcache]
roots                = []          # no default; see "Root mapping"
map_cwd              = true
cwd_name             = "cwd"
incoming_prefix_maps = "error"     # error | strip | keep
read_only            = false
hash_env_vars        = []

[cache.disk]
enabled = true
dir     = "~/.cache/vcache"
size    = "10G"

[cache.s3]
enabled        = false             # implied true when `bucket` is set
bucket         = ""
region         = "us-east-1"
prefix         = ""
endpoint       = ""                # derived from region when empty
path_style     = false
no_credentials = false
timeout        = 30                # seconds
```

## Option summary

| TOML key | Environment | Command line | Default |
| --- | --- | --- | --- |
| `vcache.roots` | `VCACHE_ROOTS` | `--vcache-root=SPEC` | none |
| `vcache.map_cwd` | `VCACHE_MAP_CWD` | — | `true` |
| `vcache.cwd_name` | `VCACHE_CWD_NAME` | — | `cwd` |
| `vcache.incoming_prefix_maps` | `VCACHE_INCOMING_PREFIX_MAPS` | `--vcache-incoming-prefix-maps=`, `--vcache-allow-prefix-maps` | `error` |
| `vcache.native_target` | `VCACHE_NATIVE_TARGET` | — | `resolve` |
| `vcache.dep_scan` | `VCACHE_DEP_SCAN` | — | `manifest` |
| `vcache.read_only` | `VCACHE_READONLY` | — | `false` |
| `vcache.error_on_cache_media_failure` | `VCACHE_ERROR_ON_CACHE_MEDIA_FAILURE` | `--error-on-cache-media-failure` | `false` |
| `vcache.hash_env_vars` | `VCACHE_HASH_ENV_VARS` | — | none |
| `cache.disk.enabled` | `VCACHE_DISK` | — | `true` |
| `cache.disk.dir` | `VCACHE_DIR` | — | `$XDG_CACHE_HOME/vcache`, else `~/.cache/vcache` |
| `cache.disk.size` | `VCACHE_CACHE_SIZE` | — | `10G` |
| `cache.s3.enabled` | — | — | `false` |
| `cache.s3.bucket` | `VCACHE_S3_BUCKET` | — | none |
| `cache.s3.region` | `VCACHE_S3_REGION`, `AWS_REGION`, `AWS_DEFAULT_REGION` | — | `us-east-1` |
| `cache.s3.prefix` | `VCACHE_S3_PREFIX` | — | none |
| `cache.s3.endpoint` | `VCACHE_S3_ENDPOINT` | — | derived from region |
| `cache.s3.path_style` | `VCACHE_S3_PATH_STYLE` | — | `false` |
| `cache.s3.no_credentials` | `VCACHE_S3_NO_CREDENTIALS` | — | `false` |
| `cache.s3.timeout` | *(none)* | — | `30` |
| `cache.s3.assume_no_list_bucket` | *(none)* | — | `false` |
| `cache.s3.ttl_days` | `VCACHE_S3_TTL_DAYS` | — | `30` |
| `cache.s3.size` | `VCACHE_S3_CACHE_SIZE` | — | `0` (uncapped) |
| — | `AWS_ACCESS_KEY_ID` | — | — |
| — | `AWS_SECRET_ACCESS_KEY` | — | — |
| — | `AWS_SESSION_TOKEN` | — | — |
| — | `VCACHE_CONFIG` | — | see search order |
| — | `VCACHE_DISABLE` | — | `false` |
| — | `VCACHE_LINK_CACHE` | — | `false` |
| — | `VCACHE_TRACER` | — | next to the binary |
| — | `VCACHE_RECACHE` | — | `false` |
| — | `VCACHE_COMPILER_CHECK` | — | `version` |
| — | `VCACHE_LOG` | — | off |

`cache.s3.timeout` is the only setting with no environment override.

## When a cache layer breaks

A broken cache and a cold cache both make a build recompile, and by default
vcache treats them the same way as far as the build is concerned: it warns and
carries on. An unreachable shared cache should slow a build down, never break
one.

What changed is that it now *says so*. Previously a failed S3 upload was
invisible — `CacheChain::Put` reported success as long as the local layer took
the entry, so a bucket that rejected every write looked like a healthy cache
with disappointing hit rates. Both directions are now reported:

```console
vcache: warning: cache layer failed: s3: PUT /vcache/ab/cdef: HTTP 403: ...
```

and counted, so `--show-stats` shows the aggregate across a whole build rather
than one line per compile buried in the log:

```
cache media errors  1843
```

Set `--error-on-cache-media-failure` (or the equivalent config/environment
setting) to make that fatal instead. vcache then exits **90** — chosen to sit
outside the range compilers use, so a build log can tell "the cache is broken"
apart from "the code does not compile". This is for CI that is *supposed* to
have a working shared cache, where a silent fallback to local-only is a
regression that would otherwise go unnoticed for weeks.

Three things it deliberately does not do:

- **A miss is never a media failure.** An absent object, an empty cache and a
  first-ever build all report nothing, with or without the flag.
- **It does not override a compiler failure.** If the compile itself failed,
  that exit status is propagated; the cache fault is still warned about. The
  compiler's diagnosis is the more useful one.
- **The output is still produced.** The object is in place before the exit
  status is decided, so a build re-run without the flag needs no cleanup.

### Buckets should grant ListBucket

S3 answers a missing key with **404** when the caller holds `s3:ListBucket` and
**403** when it does not. So on an under-permitted bucket an ordinary miss and a
real permission failure look identical, and the two want opposite handling.

vcache resolves that rather than guessing. The first time a lookup comes back
403, it issues one `max-keys=0` listing — a request that fetches no objects and
asks only whether the caller is allowed to ask:

- **The listing succeeds.** `ListBucket` is granted, so a missing key would have
  been a 404. This 403 is a genuine permission failure on the object — no
  `s3:GetObject`, or a KMS key the caller cannot use — and is reported as a
  media error rather than filed as a miss.
- **The listing is denied too.** The 403 was most likely an ordinary miss, and
  vcache warns once that this bucket cannot distinguish the two cases.

The probe costs nothing on a correctly permitted bucket, because such a bucket
answers misses with 404 and never reaches it. It runs at most once per
invocation, and only on the configuration it is asking you to fix.

Grant `s3:ListBucket` where you can — it is also what `--trim` needs. Where you
cannot, accept it explicitly:

```toml
[cache.s3]
assume_no_list_bucket = true
```

which silences the warning *and* skips the probe. That key is **config-file
only**, with no environment variable or command-line form, on purpose: it
silences a correctness diagnostic, so it should be a recorded decision about a
particular bucket rather than something a shell alias can set on the way past.

With it set, a wrong-credentials bucket looks like a cache that never hits
rather than an error — check `--show-config` and the bucket policy before
concluding the cache is merely cold.

## Caching the link step

Off by default, and **Linux only**. `VCACHE_LINK_CACHE=1` turns it on.

Discovering a link's real input set depends on `LD_PRELOAD` interposition,
`/proc/self/exe`, `/proc/self/fd` and `dl_iterate_phdr`. macOS blocks
`DYLD_INSERT_LIBRARIES` for system binaries and has no `/proc`, so there is no
trace, no absent set, and no way to show a hit is sound. Elsewhere vcache leaves
links to the compile path, which declines them exactly as it did before link
caching existed; setting `VCACHE_LINK_CACHE` there changes nothing.

A link is a pure function of its inputs -- two LTO links of identical inputs
produce a byte-identical binary -- so caching one is sound in principle. What
makes it harder than caching a compile is that there is no `-E` equivalent: the
input set is *discovered*, not declared. ccache and sccache decline to cache
links for exactly this reason.

vcache discovers the set by watching the link. `vcache-fstrace.so` is
`LD_PRELOAD`ed into the linker's whole process tree and records what was read,
what was probed and found absent, and which executables ran. A hit then requires
both halves:

- every recorded input still hashes the same, **and**
- every recorded absent path is still absent.

The second is not a nicety. `-lfoo` resolves to `/usr/lib/libfoo.so` only
because `/opt/lib/libfoo.so` was missing; an entry recording only what it read
would hit after someone installs the latter and hand back a binary linked
against the wrong library. Re-checking costs 0.45 ms against a 2.3 s link, so
there is no reason to be clever about it.

### What is not cached

Declined, each because caching it would give a wrong or incomplete answer:

| Case | Why |
| --- | --- |
| `--build-id=uuid` | the output is not a function of the inputs |
| compiling and linking in one command | the compile half is already cached |
| output to `/dev/null` or a stream | nothing to store |
| no `-o` | the implied `a.out` is not worth an entry |
| the tracer is missing or was not loaded | no absent set, so a hit cannot be shown to be sound |
| the disk cache is disabled | large outputs are local content-addressed sidecars; the remote tier does not carry them |
| an inherited `LD_PRELOAD` is active | a preloaded library can change the link before the tracer can observe it |

The last one matters most. A statically linked linker, or a loader that ignores
`LD_PRELOAD`, produces no process records; rather than cache on the assumption
that the input set is complete, vcache declines.

`-Map`, the `-Wl,-Map=...` and `-Wl,-Map,...` forms, `-Xlinker -Map`, and the
corresponding `--dependency-file` forms are captured as additional outputs and
replayed, so a hit does not hand back the binary while the companion file
silently fails to appear.

Link outputs live content-addressed in the local disk cache and are verified
against their recorded digest before every hit. S3 may carry the small manifest
and result metadata, but link-output sidecars are deliberately local in this
version; remote-only link caching is therefore declined. `VCACHE_READONLY=1`
serves existing local link hits but writes neither metadata nor sidecars.

### Why the linker's shared libraries are inputs

A link entry records every shared library loaded by the toolchain processes, not
just the executables. That looks over-cautious until you check what the linkers
actually load:

```
ld.bfd  ->  libbfd-2.42-system.so, libctf, libsframe, libz, libzstd, libc
lld     ->  libLLVM.so.22.1, libstdc++, libz, libzstd, libc
```

For the two most common linkers **the implementation is a shared library**.
`/usr/bin/ld.bfd` is a thin driver; object parsing, relocation and layout live
in `libbfd`. `lld` is thinner still -- the whole of LLD is inside `libLLVM.so`.
Hash only the executable and a binutils or LLVM update changes the linker
completely while the file you hashed is byte-identical.

`libz` and `libzstd` are more direct again: `--compress-debug-sections=zstd`
runs debug sections through libzstd, so its version decides output bytes.

The cost is real -- a libc or zlib update invalidates every link entry -- and
there is no clean rule that keeps libbfd and libLLVM while dropping libc, since
"system library" and "part of the linker" are the same set here.

The tracer itself is deliberately excluded. It observes the link and cannot
affect the output, so recording it would make every rebuild of vcache invalidate
every link entry.

### Using it from CMake

Three things to know, because getting any of them wrong makes the feature look
like it does nothing rather than fail.

**`CMAKE_<LANG>_COMPILER_LAUNCHER` does not apply to link rules.** It wraps
compiles only. Links need `CMAKE_<LANG>_LINKER_LAUNCHER`:

```
-DCMAKE_C_COMPILER_LAUNCHER=vcache  -DCMAKE_C_LINKER_LAUNCHER=vcache
-DCMAKE_CXX_COMPILER_LAUNCHER=vcache -DCMAKE_CXX_LINKER_LAUNCHER=vcache
```

Without the second pair the generated rule calls the driver directly. A full
LLVM build configured that way cached 7,877 compiles and zero links, with no
error anywhere to say so. Check the generated rule rather than assuming:

```console
$ grep -m1 -A1 '^rule CXX_EXECUTABLE_LINKER' CMakeFiles/rules.ninja
```

**Sub-builds need it passed again.** Each entry in `LLVM_ENABLE_RUNTIMES`, and
compiler-rt's builtins, is a separate CMake project that does not inherit
either launcher; they take `RUNTIMES_CMAKE_ARGS` and `BUILTINS_CMAKE_ARGS`.
Projects with nested CMake builds generally have some equivalent.

**CMake has no launcher for assembly.** `CMAKE_<LANG>_COMPILER_LAUNCHER`
supports C, CXX, Fortran, ISPC, OBJC, OBJCXX, CUDA and HIP -- not `ASM`. So
`.S` sources compile outside the cache, and with `-g` they record the absolute
source path in `.debug_line`, which makes the object differ per checkout. That
is invisible until it propagates: in LLVM, four BLAKE3 `.S` objects made
`libLLVMSupport.a` differ between two checkouts, and every link that pulls in
that archive then missed.

Nothing vcache can fix from its side. `-Wa,--debug-prefix-map` does not remap
it either; building the assembly without debug info does, and the objects then
match byte for byte. Worth knowing before concluding that link caching does not
work on a project.

### Debugging a link miss

`VCACHE_LOG` names the input that changed or the path that stopped being
absent, which is usually the whole answer.

## Flags that write a second output file

A cache entry holds the object and the dependency file, and nothing else. Some
flags make the compiler write a *companion* file next to the object — coverage
notes, split debug info, a compilation-database fragment — and replaying such
an entry would return a correct object while that companion silently never
appeared. The build looks successful; the breakage shows up later and somewhere
else, when gcov finds no data or a debugger cannot resolve any DWARF.

vcache therefore declines to cache them, reporting `flag writes a second output
file` as the uncacheable reason:

| Flag | Companion file |
| --- | --- |
| `--coverage`, `-ftest-coverage`, `-fprofile-arcs` | `.gcno` |
| `-gsplit-dwarf`, `-gsplit-dwarf=split` | `.dwo` |
| `-fstack-usage` | `.su` |
| `-fcallgraph-info` | `.ci` |
| `-fdump-*` | gcc dump files |
| `-ftime-trace` | `.json` |
| `-fsave-optimization-record`, `-foptimization-record-file=` | `.opt.yaml` |
| `-MJ FILE` | compilation-database fragment |
| `-serialize-diagnostics FILE` | `.dia` |
| `-gen-cdb-fragment-path DIR` | cdb fragments |

`-gsplit-dwarf=single` is deliberately **not** in that list: it keeps the debug
sections inside the object, so there is no companion file and nothing to lose.
That is checked against clang rather than inferred from the flag's name.

## Root mapping

This is the setting that makes vcache worth using. Everything else is
housekeeping.

A **root** is a directory whose location must not affect the cache key. vcache
rewrites it to a stable **canonical prefix** on the compiler command line, via
`-ffile-prefix-map` for gcc/clang and `--remap-path-prefix` for rustc. Two
checkouts in different places then produce byte-identical objects and share one
cache entry.

### Spec syntax

A root spec takes three forms:

| Spec | Canonical prefix | Use when |
| --- | --- | --- |
| `/home/you/proj` | `/vcache/proj` (basename) | the directory name is the same everywhere |
| `/home/you/proj=myapp` | `/vcache/myapp` | directory names differ between machines |
| `/home/you/proj=/usr/src/myapp` | `/usr/src/myapp` | you want to choose the path that lands in debug info |

Relative paths are resolved against the current directory. Symlinks are
resolved, so two spellings of the same tree map identically. A root that does
not exist yet is accepted (useful for generated-source directories) and handled
lexically.

The middle form is the one to reach for in practice: if one machine has
`/home/alice/checkout` and another has `/build/ci-123`, the basename default
gives them different prefixes and they will never share.

### Setting roots

```console
# Command line, repeatable, highest precedence
$ vcache --vcache-root=/home/you/proj=myapp --vcache-root=/opt/toolchain g++ ...

# Environment, colon-separated like PATH
$ export VCACHE_ROOTS=/home/you/proj=myapp:/opt/toolchain

# Config file
```
```toml
[vcache]
roots = ["/home/you/proj=myapp", "/opt/toolchain"]
```

Specs from all three sources are combined, not overridden.

### Ordering and overlap

Roots may nest. The most specific one wins:

```toml
roots = ["/work=tree", "/work/vendor/thirdparty=vendor"]
```

`/work/vendor/thirdparty/x.h` maps to `/vcache/vendor/x.h`, and `/work/src/a.cc`
to `/vcache/tree/src/a.cc`. vcache emits the underlying `-ffile-prefix-map`
flags most-general-first, because gcc resolves overlapping maps last-match-wins;
you do not need to order the specs yourself.

Two roots resolving to the same canonical prefix would alias distinct trees onto
one cache key. vcache detects this, warns, and disambiguates by appending an
index (`/vcache/name-2`).

### The build directory

`DW_AT_comp_dir` in an object file records the **working directory**, not the
source directory, so an unmapped build directory defeats sharing even when the
source tree is mapped correctly.

vcache therefore adds the current directory as a root automatically, named
`cwd`, unless it is already inside a configured root. Set `map_cwd = false` /
`VCACHE_MAP_CWD=0` to disable, or `cwd_name` / `VCACHE_CWD_NAME` to rename the
canonical prefix.

### Checking what you got

```console
$ vcache --show-roots
/home/you/proj -> /vcache/myapp
/home/you/proj/build -> /vcache/cwd
```

If two machines' right-hand sides differ, they will not share entries. That is
the single most common cause of an unexpectedly cold cache.

### Debugger setup

Because paths in debug info are canonical, tell your debugger where the sources
really are:

```
(gdb)  set substitute-path /vcache/myapp /home/you/proj
(lldb) settings set target.source-map /vcache/myapp /home/you/proj
```

Or choose a canonical target that is a plausible absolute path
(`--vcache-root=/home/you/proj=/usr/src/myapp`) and place the sources there once
on the machine that does the debugging.

## Incoming prefix-map flags

If the compiler command line already contains `-ffile-prefix-map`,
`-fdebug-prefix-map`, `-fmacro-prefix-map`, `-fprofile-prefix-map` or
`--remap-path-prefix`, vcache must decide what to do about it.

| Policy | Behaviour |
| --- | --- |
| `error` *(default)* | Refuse to run and explain the options. Nothing is compiled. |
| `strip` | Remove the caller's flags; vcache's mapping applies. Compilation is cached. |
| `keep` | Pass the caller's flags through untouched. The compilation is **not** cached and counts as *uncacheable*. |

The default is `error` because vcache's job is deciding which paths end up in
your output. Silently overriding a mapping the build system deliberately asked
for would change its artifacts without telling anyone, and gcc's
last-match-wins resolution makes the combination genuinely ambiguous.

```console
$ vcache --vcache-allow-prefix-maps g++ ...              # shorthand for =strip
$ vcache --vcache-incoming-prefix-maps=keep g++ ...
$ export VCACHE_INCOMING_PREFIX_MAPS=strip
```
```toml
[vcache]
incoming_prefix_maps = "strip"
```

An unrecognised policy name is rejected outright rather than silently falling
back to something permissive.

## Disk cache

The local layer, and the first one consulted.

```toml
[cache.disk]
enabled = true
dir     = "~/.cache/vcache"    # ~ is expanded
size    = "20G"
```

Entries are stored at `<dir>/<first-2-hex-of-key>/<rest>`, sharded 256 ways.
Each store checks only its own shard against `size / 256` and evicts
least-recently-used entries within it, which bounds eviction work to 1/256th of
the cache and keeps it cheap enough to run inline during a build. `size` is a
budget, not a hard limit: a shard is trimmed to 80% of its share when it exceeds
it.

`vcache --trim` sweeps every shard on demand; `vcache --clear` empties the cache
and resets counters.

Statistics live in `<dir>/stats` and are updated under `flock(2)`. Memoised
compiler fingerprints live in `<dir>/compilers/`.

## S3 cache

The optional shared layer, consulted after disk. Setting `bucket` enables it.

```toml
[cache.s3]
bucket   = "my-build-cache"
region   = "us-west-2"
prefix   = "vcache/"
timeout  = 30
```

```console
$ export VCACHE_S3_BUCKET=my-build-cache
$ export VCACHE_S3_REGION=us-west-2
$ export AWS_ACCESS_KEY_ID=... AWS_SECRET_ACCESS_KEY=...
```

Credentials come from the standard `AWS_ACCESS_KEY_ID`,
`AWS_SECRET_ACCESS_KEY` and `AWS_SESSION_TOKEN` variables. There is no support
for profile files or IMDS; export the variables, or use `no_credentials` for a
public read-only bucket.

Requests are signed with AWS Signature Version 4, with no SDK. A compile only
ever issues `GET` and `PUT` of a single object. `vcache --trim` additionally
uses `ListObjectsV2` and `DELETE`, so a role that only ever compiles needs
`s3:GetObject` and `s3:PutObject`, while a role that also trims needs
`s3:ListBucket` and `s3:DeleteObject`.

### Non-AWS endpoints

MinIO, Ceph and similar gateways need an explicit endpoint, and usually
path-style addressing:

```toml
[cache.s3]
bucket     = "builds"
endpoint   = "http://minio.internal:9000"
path_style = true
region     = "us-east-1"      # still required; part of the signature scope
```

### Behaviour

Reads walk disk then S3 and stop at the first hit. A remote hit is written back
into the local layer, so it serves the rest of the build locally. Writes go to
every writable layer.

Any transport, authentication or timeout failure is reported as a **miss**: an
unreachable shared cache slows a build down, it never breaks one. HTTP 403 is
treated like 404, since buckets without `ListBucket` permission return it for a
missing key.

libcurl is loaded with `dlopen` the first time an S3 layer is constructed, so a
disk-only build never maps it. If S3 is configured and libcurl is not
installed, vcache prints one warning and continues with the local cache.

### Expiry and size

```toml
[cache.s3]
ttl_days = 30      # 0 disables expiry
size     = "500G"  # 0, the default, means no cap
```

`ttl_days` is enforced **on read**, not only by `--trim`: an object older than
the window is treated as a miss even if nothing has trimmed the bucket and no
lifecycle rule exists. That deliberately does not depend on anyone remembering
to trim, or on a bucket rule being present and correctly scoped. Age comes from
the object's `Last-Modified`; if that header is missing or unparseable the entry
is served rather than refused, because losing a good cache to a header quirk is
worse than serving something slightly stale.

`size` is a byte budget for this layer, and unlike the disk budget it is
enforced **only** by `vcache --trim`, never during a compile. Two reasons:
enforcing it inline would mean a bucket listing per compilation, and the bucket
is usually shared, where evicting other machines' entries is not a decision an
ordinary build should be making. For the same reason it defaults to *uncapped* —
a cap that every client enforced would let one machine's small setting evict
work the rest of the fleet is still using. The disk layer can default to a
budget because that cache belongs to one machine.

`vcache --trim` lists every object under the prefix, deletes those past the
TTL, and then — if a cap is set and the remainder is still over it — deletes
oldest-first down to 80% of the budget, the same overshoot the disk layer uses
so the next trim is not triggered by a single store. It reports what it did and
exits non-zero if the listing or any delete failed, so a scheduled trim that is
quietly doing nothing is visible.

A bucket lifecycle rule remains the better primary mechanism where you control
the bucket: it costs nothing, runs server-side, and cannot be skipped. vcache's
TTL is the belt to that braces — it bounds what a build can be handed
regardless of what the bucket is configured to do.

### Auto-disable rules

S3 turns itself off, with a warning, if:

- it is enabled but no `bucket` is set; or
- credentials are absent and `no_credentials` is not set.

## Cache behaviour switches

| Variable | Effect |
| --- | --- |
| `VCACHE_DISABLE=1` | Run the compiler directly. No lookup, no store, no statistics. |
| `VCACHE_READONLY=1` | Look up and serve hits, never store. Also `vcache.read_only`. |
| `VCACHE_RECACHE=1` | Ignore hits, always compile, and overwrite the entry. Useful for repairing a cache you suspect is wrong. |

`read_only` applies to every layer, including S3.

## Cache key inputs

Understanding what is *not* in the key explains most surprising hits and misses.

**In the key:** a key-format version; compiler identity; the resolved native
target, if any; the sorted set of canonical root targets; codegen-affecting
flags with paths canonicalised; the preprocessed source text with linemarker
paths canonicalised; and any variables named in `hash_env_vars`.

**Deliberately not in the key:**

- `-I`, `-D`, `-U`, `-include`, `-isystem` and friends. Their entire effect is
  already visible in the preprocessed text. Hashing them would make
  `-I/home/a/proj/inc` and `-I/work/b/proj/inc` derive different keys for
  identical input — exactly the failure vcache exists to fix. A consequence
  worth knowing: adding an unused `-DFOO` still hits.
- `-o`. The output location does not change the object's contents.
- Local root paths. Only the canonical targets participate.

### Data pulled in behind the preprocessor's back

vcache hashes what the preprocessor produces, so anything the *assembler* reads
is invisible to it. The case that matters in practice is `.incbin` in inline
assembly — the pattern behind Firedancer's `FD_IMPORT_BINARY` and similar
"embed this blob in the object" macros. Change the blob without changing a
byte of source, and the preprocessed text is identical, so vcache serves the
object built from the old blob. (ccache handles this by refusing to cache any
translation unit containing `.incbin` at all.)

Two things usually save you, and it is worth knowing which one you are relying
on:

- Build systems that do this generally already pass a digest of the embedded
  files as a `-D`, precisely to bust ccache and sccache. That reaches vcache's
  key **only under `-g3`/`-ggdb3`**, where gcc writes command-line `#define`s
  into its `-E` output. At `-g` or lower it does not, and the protection is
  silently gone.
- Failing that, name the blob in `hash_env_vars` or regenerate a header from
  it, so the dependency is visible in the text.

### `hash_env_vars`

For builds where an environment variable reaches the output without appearing on
the command line — `SOURCE_DATE_EPOCH` being the usual case:

```toml
[vcache]
hash_env_vars = ["SOURCE_DATE_EPOCH"]
```
```console
$ export VCACHE_HASH_ENV_VARS=SOURCE_DATE_EPOCH,BUILD_FLAVOUR   # comma-separated
```

### `native_target`

`-march=native`, `-mtune=native` and `-mcpu=native` are the one case where the
command line does not say what the compiler will do: the same six characters
mean AVX-512 on one machine and SSE4.2 on the next. Hashing the flag text alone
would let those two machines share an entry, and the object one of them gets
back would use instructions its CPU does not have.

By default vcache asks the compiler instead. Before the first such compilation
it runs `<compiler> -march=native … -E -dM -x <lang> /dev/null` and hashes the
result into the key. The macro dump names every enabled ISA extension and the
selected tuning model (`__AVX512F__`, `__tune_znver4__`, …), so two hosts that
would generate different code derive different keys, and two identical hosts
share entries as usual — including through S3.

| Mode | Meaning |
| --- | --- |
| `resolve` *(default)* | Probe the compiler and put the answer in the key. |
| `uncacheable` | Do not cache such compilations at all. |

```toml
[vcache]
native_target = "resolve"
```
```console
$ export VCACHE_NATIVE_TARGET=uncacheable
```

The probe costs about 7 ms, so its result is memoised in the cache directory
against the compiler's identity, the flags, and the machine's hostname and
architecture. That last part matters if — unusually — you point `VCACHE_DIR` at
a directory shared between machines: hosts are told apart by hostname there, so
if several machines with different CPUs share both a cache directory *and* a
hostname, choose `uncacheable`.

If the probe fails for any reason, the compilation is treated as uncacheable
rather than being cached under a guess.

### `dep_scan`

A `-M`/`-MM` run with no `-c` produces a dependency list and nothing else. Some
build systems — Firedancer's, for one — run one per translation unit alongside
the compile, so on a large tree they are half of all compiler invocations.

They cannot be keyed the way a compile is. That key comes from preprocessed
text, and for a `-M` run the preprocessor *is* the work. Measured on one
115-header unit:

| | |
| --- | --- |
| the `-M -MP` run itself | 16 ms |
| `-E`, to derive an ordinary key | 20 ms |
| hashing all 115 headers (753 KiB) | ~1 ms |

Preprocessing to save preprocessing is a losing trade, so these are cached
against a **manifest** instead. The key covers the command line — including
`-I`, `-D` and friends verbatim, since there is no preprocessed text to stand in
for them — plus the compiler, the resolved native target, and the source's
contents. The entry stores every file the scan read with its digest, and a hit
is served only once all of them still hash the same. Nothing is trusted on
mtime.

One manifest holds up to eight remembered header states, so alternating between
two branches keeps hitting rather than overwriting one state with the other.

| Mode | Meaning |
| --- | --- |
| `manifest` *(default)* | Cache scans; verify by hashing the recorded files. |
| `uncacheable` | Run the compiler. Same as not caching them. |

```console
$ export VCACHE_DEP_SCAN=uncacheable
```

**The limitation worth knowing.** A manifest records the files a scan *did*
read, so it cannot notice a file that was not there at the time. Create a header
that would have been found earlier on the include path than the one recorded,
change nothing else, and a scan can still hit and return the old answer. Adding
or removing an `#include` in a source file does not have this problem — the
source's contents are in the key. Changing `-I` does not either — the flags are
in the key. It is specifically *creating a file that shadows an existing one*
that this mode cannot see, which is the same trade ccache's direct mode makes.
`-MG`, which lets a dependency name a file that does not exist yet, is not
cached at all for the same reason.

### `VCACHE_COMPILER_CHECK`

How the toolchain is identified:

| Mode | Meaning |
| --- | --- |
| `version` *(default)* | Hash of `<compiler> -v` output — version, target triple, configure flags. Machine-independent, so entries are shareable through S3. Memoised in the cache directory, so the extra process runs once per toolchain, not once per compilation. |
| `content` | Hash the compiler driver binary. Differs between machines even for identical toolchains. |
| `mtime` | Path, size and mtime only -- no probe, no banner. Fastest, least safe. |

`version` is the default precisely because a shared remote cache is a first-class
use case and mtimes never agree across machines.

## Logging and diagnostics

`VCACHE_LOG` takes a file path, or the literal `stderr`:

```console
$ VCACHE_LOG=stderr vcache g++ -c foo.cc -o foo.o
$ VCACHE_LOG=/tmp/vcache.log make -j16
```

Each line carries a timestamp and pid, so a parallel build stays readable when
many processes append to one file. Every cache decision is logged — the
preprocessing command, the computed key, hit or miss and from which layer, the
compile command, and the compiler's own stderr when preprocessing fails.

Inspection commands:

| Command | Shows |
| --- | --- |
| `vcache --show-config` | The fully resolved configuration, plus any warnings |
| `vcache --show-roots` | The root mapping for the current directory |
| `vcache --show-stats` | Counters, hit rate, cache size |
| `vcache --zero-stats` | Reset counters |
| `vcache --clear` | Delete all entries |
| `vcache --trim` | Evict until under the size limit |

## Value formats

**Sizes** accept a decimal number and an optional suffix, case-insensitive:
`b`, `k`/`kb`/`ki`, `m`/`mb`/`mi`, `g`/`gb`/`gi`, `t`/`tb`/`ti`. All are powers
of 1024; `20G` means 20 GiB. A bare number is bytes. Unparseable values produce
a warning and leave the default in place.

**Booleans** in the environment accept `1`, `true`, `yes`, `on` and `0`,
`false`, `no`, `off`, case-insensitive. Anything else leaves the current value
alone. In TOML, use real booleans.

**Lists** differ by source, following each one's convention:

| Setting | Environment separator | TOML |
| --- | --- | --- |
| `VCACHE_ROOTS` | `:` (like `PATH`) | array of strings |
| `VCACHE_HASH_ENV_VARS` | `,` | array of strings |

**Paths** expand a leading `~/` using `$HOME`.

## What is never cached

These fall through to the compiler unchanged and are counted as *uncacheable*.
A build always makes progress.

**C/C++:** linking (no `-c`); `-E` only; `-MG`; more than one input file;
input from stdin; no recognised source language; plain `.s` assembly (`.S` is
cacheable, since it is preprocessed); output to `/dev/null` or `-`;
`-save-temps`, `-fsyntax-only`,
`-specs=`, `-frepo`, `-fmodules`, PGO flags (`-fprofile-generate`,
`-fprofile-use`, `-fprofile-instr-use`, `-fauto-profile`),
`-fsanitize-blacklist=`; malformed or deeply nested `@response-files`.

**Rust:** no `--out-dir`; no `--emit`; `--emit` with an explicit output path;
an explicit `-o`; more than one input file; input from stdin.

Additionally, `incoming_prefix_maps = "keep"` makes any affected compilation
uncacheable, and any internal failure (preprocessing error, temp-directory
failure, unreadable output) falls back to a plain compiler run.

## Worked examples

### A single developer machine

Nothing but the root needs setting. Put it in the shell profile:

```console
$ export VCACHE_ROOTS=$HOME/src/myproject
$ export PATH=$HOME/.local/libexec/vcache:$PATH
```

### Several checkouts of one project

Name the canonical prefix explicitly so the differing directory names do not
produce differing prefixes. A per-checkout `.envrc`, or:

```console
$ cd ~/work/feature-branch
$ export VCACHE_ROOTS=$PWD=myproject
```

### CI sharing a cache with developers

Developers write, CI reads. On the developer machines:

```toml
[vcache]
roots = ["/home/you/src/myproject=myproject"]

[cache.disk]
dir  = "~/.cache/vcache"
size = "20G"

[cache.s3]
bucket = "org-build-cache"
region = "us-west-2"
prefix = "vcache/v1/"
```

On the CI runner, the same bucket plus a root pointing at the checkout, and
read-only so a misconfigured job cannot poison the shared cache:

```console
$ export VCACHE_ROOTS=$CI_WORKSPACE=myproject
$ export VCACHE_S3_BUCKET=org-build-cache VCACHE_S3_REGION=us-west-2
$ export VCACHE_S3_PREFIX=vcache/v1/
$ export VCACHE_READONLY=1
```

Bumping the `prefix` is the cheapest way to invalidate a shared cache wholesale.

### A self-hosted MinIO cache

```console
$ export VCACHE_S3_BUCKET=builds
$ export VCACHE_S3_ENDPOINT=http://minio.internal:9000
$ export VCACHE_S3_PATH_STYLE=1
$ export AWS_ACCESS_KEY_ID=... AWS_SECRET_ACCESS_KEY=...
```

### A build system that already remaps paths

Debian packaging and reproducible-build setups often pass `-ffile-prefix-map`
themselves. Decide once, in the config file:

```toml
[vcache]
roots                = ["/build/myproject=/usr/src/myproject"]
incoming_prefix_maps = "strip"
```

Choosing an absolute canonical target reproduces what the build was aiming for
while letting vcache own the mapping.

### Diagnosing a cold cache

```console
$ vcache --show-roots                  # do both machines agree?
$ vcache --show-stats                  # is "uncacheable" high?
$ VCACHE_LOG=stderr vcache g++ ...     # what did it decide, and why?
```
