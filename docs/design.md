# vcache design

Read `preprocessor-problem.md` first — it records the compiler behaviour this
design exists to work around. This document covers structure and the decisions
that were not obvious.

## Layout

```
src/
  main.cc              entry point; direct and masquerade invocation
  args/
    compiler_args.*    gcc/clang command-line parsing
    rustc_args.*       rustc command-line parsing
  core/
    roots.*            root -> canonical prefix mapping (the heart)
    preprocessed.*     linemarker normalisation while hashing
    depfile.*          Makefile-fragment parsing (Boost.Spirit X3)
    compile.*          the C/C++ pipeline
    cxx_manifest.*     the C/C++ manifest: key arguments, PCH identity, states
    manifest.*         what the manifests have in common
    config.*           TOML + environment configuration
    stats.*            persistent counters
    reason.h           why a run was not cached: log text and stats key
  rust/
    rust_compile.*     the Rust pipeline
  storage/
    storage.h          backend interface + blob container format
    disk_storage.*     sharded local cache with LRU eviction
    s3_storage.*       SigV4 over libcurl
    chain.*            multi-level chain with read-through backfill
  daemon/
    protocol.*         wire format, socket path, config fingerprint
    server.*           the optional cache daemon (see daemon.md)
    client.*           a compile's side of the connection
  hash/hasher.*        BLAKE3
  util/                strings, filesystem, subprocess, logging
```

## What goes into a cache key

Included:

- a key-format version, so a change to this list invalidates old entries
- compiler identity (see below)
- the output kind: assembly (`-S`) or object (`-c`)
- the sorted set of **canonical** root targets
- codegen-affecting flags; an argument that is itself a path under a root is
  canonicalised, a path inside a longer argument is kept verbatim
- the preprocessed text, with linemarker paths canonicalised
- explicitly configured environment variables
- for Rust, each variable the dep-info reports the crate read through `env!` or
  `option_env!`, with its raw value or an unset marker. Raw because rustc does
  not remap env values, so a path-valued one may be baked into the artifact

Deliberately excluded, and why each matters:

- **`-I`, `-D`, `-include` and friends.** Their entire effect is already visible
  in the preprocessed output. Hashing them as text would mean
  `-I/home/a/proj/inc` and `-I/work/b/proj/inc` produce different keys for
  identical preprocessed input — which is precisely the failure being fixed.
- **`-o`.** The output location does not change the object's contents. Including
  it would mean the same compilation cached under two names never shares.
- **Local root paths.** `RootMap::Fingerprint()` emits only canonical targets.
  This was an actual bug during development: including local paths made every
  cross-directory lookup miss even though everything else was correct.
- **The dependency file's path**, however it is spelled. `-MF`, `-MT`, and the
  `-Wp,-MMD,<file>` form kbuild uses all name a side output rather than change
  the object, so the path stays out of the key and the file is stored beside the
  object with its own paths canonicalised.

Where the preprocessed text stops standing in for the compilation, vcache
declines rather than guesses. `.incbin` is the case that matters in practice: it
makes the *assembler* open a file, so neither its name nor its contents are in
the text, and two compilations can hash identically while owing different
objects. Files gcc reads after preprocessing — sanitizer ignore lists, sample
profiles, plugins, the randstruct layout seed — are hashed by content instead,
since there the flag at least names the file.

A `-M`/`-MM` dependency scan has no preprocessed text, so its manifest key is
built from what is known before the scan runs:

- its own key-format version
- compiler identity and the resolved native target
- the sorted set of canonical root targets
- the command line minus `-o`/`-MF`, with `-I`, `-D` and friends kept verbatim;
  an argument that is itself a path under a root is canonicalised, a path joined
  to its flag (`-I/abs/inc`) is not
- the source's **canonical path** and its contents. The path matters because
  headers are found relative to the source: identical sources reached by
  different canonical paths read different files, and a state recorded for one
  would still verify for the other
- explicitly configured environment variables

Each state under that key lists the files the scan read with their digests, and
is served only while all of them still match.

## The C/C++ manifest

The preprocessor key needs the preprocessed text, so a lookup that computes it
runs the preprocessor, and that is most of what a hit costs. A compile first
looks up a manifest instead (`cxx_manifest`, on by default). Its key is built
from what is known before the preprocessor runs:

- its own key-format version, and the preprocessor key's, since states name
  preprocessor keys and a bump of that version must not be bypassed through an
  older manifest
- compiler identity and the resolved native target
- the sorted set of canonical root targets
- the canonical working directory, since relative `-I` paths and relative
  sources resolve against it; the source's canonical absolute path, since
  quoted includes resolve beside it; and the source as typed, canonicalised,
  since `__FILE__` and the debug info name it as the command line spelled it
- the source's contents
- the command line minus the source, `-o` and the depfile path, with `-I`,
  `-D`, `-include` and friends kept, and the paths in include-path flags
  (joined ones such as `-I/abs/dir` too) canonicalised; `-c` and `-S` are told
  apart. A `-D` value is kept as written, path or not: it reaches the object as
  written. So is `-B`: the `cc1`, `as` and specs it selects appear in no
  depfile
- the files named by keyed flags (sanitizer lists, profiles), by content
- `hash_env_vars`, and `CPATH`, `C_INCLUDE_PATH`, `CPLUS_INCLUDE_PATH`,
  `OBJC_INCLUDE_PATH` and `OBJCPLUS_INCLUDE_PATH`, which add include
  directories
- the identity of every precompiled header the command names

A state lists the files an earlier compile of that key read, with their
digests, the paths it probed and did not find, and the result key that compile
stored under. A lookup takes the first state whose files all still hash the
same, through the file-identity memo, so an unchanged header costs a `stat` and
a read of its memo, and whose absent paths are all still absent, one `lstat`
each and nothing hashed. A state that matches but whose
entry is gone, or whose entry lacks the depfile this compile asked for, falls
through to the preprocessor; the preprocessor usually computes the same key,
which is then not asked for again.

**Where the include set comes from.** The compile's own depfile, and what it
probed and did not find, from a trace or from the compiler itself. The depfile
comes first, as in ccache's depend mode. A command with `-MD` writes one. A
command with no dependency flags gets `-MD -MF <scratch> -MT <output>` added;
`-MD` writes the dependency list beside the object and changes nothing in it,
which a test checks against the compiler run directly, and the scratch file is
never stored, so the entry looks like one from an ordinary compile. `-MMD`
omits system headers, so a command with it keeps to the preprocessor. So does a
compile with `DEPENDENCIES_OUTPUT` or `SUNPRO_DEPENDENCIES` set: gcc writes
that dependency file from the preprocessor, which a manifest hit never runs,
and ignores the variable once `-MD` is on the command line.

The second is a trace of the same compile. It runs with `vcache-fstrace.so`
preloaded, as a link does (the `-E` run for the key does not), and the trace
adds every file the compiler opened that the depfile does not name, and every
path it probed and did not find. Where there is no tracer, a compiler that
lists what it did not find is asked for that list instead (below). The depfile
stays as the base: the trace cannot tell a header from the compiler's own
files, which are dropped below, and `-MD` costs the compile nothing measurable.
Of the trace, these are dropped:

- what the link cache drops: `/proc`, `/sys`, the terminal and random devices,
  directory self-references;
- the tracer, this compile's scratch directory (the object, the depfile, the
  trace), and the PCHs the key already identifies, whose bytes may differ
  between trees that share one identity;
- the compiler itself: the executables that ran, the shared objects they
  loaded (the tracer's `L` records), and the compiler's own trees,
  `lib*/gcc`, `libexec/gcc` and `lib/clang` under the prefix of each
  executable (the parent of its `bin/`, or what precedes `/libexec/`), also as
  spelled through a symlink at the top of the filesystem (`/lib64` on a merged
  `/usr`). The compiler fingerprint stands for these, and hashing a 100 MB
  `libLLVM` on every record buys nothing. Nothing under an include directory is
  dropped this way, the compiler's own (as `<compiler> -E -v` lists them)
  included: `/usr/include` and `/usr/local/include` change with the packages
  installed, not with the compiler, so the prefix itself is never dropped;
- files opened for writing only (`W`) and directories;
- absent paths directly in a `PATH` directory that are named as one of the
  compiler's executables (`as`, `cc1`, the driver's own name and so on): gcc's
  driver looks for itself and its tools along `PATH`, and which ones run is the
  compiler identity's business. A header probed in a `PATH` directory stays.

The trace is used only when every file the depfile names, the source among
them, is among its reads. A compiler process that did not load the tracer (a
static or setuid binary, another ELF class, a `-B` wrapper that drops
`LD_PRELOAD`) records nothing, so a trace without its reads says nothing about
what it probed. A traced file that is gone when the state is recorded declines
it, unless the trace wrote it as well as read it, as cc1 writes the assembly
file `as` reads; one changed at or after the compile started declines it as a
depfile's does. A traced file is not scanned for time macros or `.incbin`: the
preprocessor did not read it. Each absent path is stored as the highest of its
ancestors that does not exist either: nothing can appear below a missing
directory without it appearing too, and gcc probes every header in every `-I`
directory. For `src/daemon/server.cc` a state holds 1091 absent paths under gcc
and 902 under clang. The walk stops at the working directory and at every
root: above a root the path would name this tree's location, and a tree whose
root exists would check that location instead of its own. A path through `..`
is stored as probed, since resolving it could cross a symlink, and is compared
with what was read as spelled. Absent paths are canonicalised through the roots
like files, so a tree at another path checks its own.
Probes ending in `/..` are kept too: Clang can probe `first/sub/..` before
resolving `<sub/../a.h>`, and creating `first/sub` can change which header wins.

Without a tracer the state comes from the compiler's list when it writes one
(below), and otherwise from the depfile alone. The log says which and why: when
tracing is off (`cxx_manifest_trace`), when there is no tracer (no
`vcache-fstrace.so` beside the binary, `VCACHE_TRACER` naming nothing or
something that is not an ELF shared object of vcache's own class and machine, a
host other than Linux), when `LD_PRELOAD` is already set. A trace that turns
out unusable after the compile gives the depfile alone, since the list was not
asked for: when no process loaded the tracer, when the trace is marked
incomplete, does not parse or names no process, when the compiler's include
directories cannot be listed, and when the trace does not show a file the
depfile names being read, and when ld.so refused a preload in some process of
the compile. ld.so's "cannot be preloaded" lines that name the tracer are then
taken out of the compile's stderr: they come from preloading it, and an entry
would replay them on every hit. A refusal of any other object is the build's
own and stays. The entry is stored as usual.

**The compiler's own list.** The devtools gcc and clang carry a patch that ends
the version line of `-v` with `absent-deps-1`. Given `-fabsent-deps=FILE` such
a compiler writes there every path the preprocessor looked for and did not
find: each search directory tried before the one that supplied a header, each
`__has_include` that found nothing, each `.gch` or `.pch` probed beside a
header, each include directory that does not exist. Its depfile names a header
`__has_include` found and every PCH it read. The trace still comes first: it
also sees what the driver reads and probes on its own, which the list does not
name, and recording from the list is not measurably cheaper. So when there is
no tracer for the compile (any reason above, `cxx_manifest_trace` off among
them), the banner carries version 1 (`CompilerId::absent_deps_version`) and
`cxx_manifest_absent_deps` is on, the recording compile gets
`-fabsent-deps=<scratch>/absent` beside the `-MD -MF` vcache adds; with a
tracer it gets no such flag, and the `-E` run never does. The flag is vcache's
own, like `-MD`, and enters neither key. A version vcache does not know, the
switch off, or clang's `-working-directory` (the list's relative paths are
relative to that directory, not to where the compiler runs) records from the
depfile alone, and the log says why. `cxx_manifest_trace = false` with a
patched compiler is how to record from the list on purpose, to compare it with
the trace.

The state's files come from the depfile, less a PCH the manifest key already
identifies, whose bytes may differ between trees that share one identity. Its
absent paths come from the list: relative ones joined to the working directory,
canonicalised through the roots, each stored as its highest missing ancestor (a
path spelled through `..`, as clang spells its C++ include directories, is
stored as spelled and costs one `lstat` on every lookup: 313 of clang's 1202
for `src/daemon/server.cc`), with the same noise dropped as from a trace's
absent paths: what the link cache drops, the scratch directory, the keyed PCHs,
and the compiler's own trees outside every include directory, found from the
driver's prefix. The tracer's other rules have nothing to act on: the list
names no executable, no shared object, no file the compiler read and no probe
the driver made along `PATH`. The gcc `__has_include` decline and the search
for an unkeyed `.gch` do not run for a state from the list, which holds the
probes; from the depfile alone they do, for a patched compiler too: its depfile
names a header `__has_include` found, but not a probe that found nothing. What
only the driver reads or probes is in neither the list nor the depfile: clang's
configuration files, the GCC installation it detects (`crtbegin.o`,
`/etc/os-release`), gcc's `specs` and `as`. The trace records those; with the
list they are left to the compiler fingerprint, as ccache leaves them. A listed
path that exists when the state is recorded is a fault in the compiler: a state
holding it would never be served, and the list cannot be trusted to be whole,
so no state is recorded, and the log says `the compiler listed <path> as absent
but it exists`. So does a path whose absence `lstat` cannot show (an error
other than `ENOENT` or `ENOTDIR`), which a lookup would never take as absent
(`... but it cannot be shown absent`). A trailing carriage return on a line is
dropped. A compile that writes no list records no state either. A state holds
the same kinds of inputs whichever way it was recorded, so the manifest format
and its key version are unchanged.

**Result keys.** When `-E` succeeds the result is stored under the ordinary
preprocessor key, and the state names that key. A tree with the manifest off
hits what a tree with it on stored, and the reverse; the two modes only differ
in how they find the key. When `-E` fails with a compiler error but the compile
succeeds (a PCH consumer whose command lacks an include path the PCH's header
text needs: `-E` expands that text, the compile loads the PCH) the result is
stored under a key derived from the manifest key and every recorded file's
digest, as ccache's depend mode keys every result. Only a manifest lookup can
name that key. With single-flight, such a compile leases the manifest key, and
a peer it wakes looks in the manifest. Before the manifest existed, the same
compile was a *preprocess failed* passthrough; if the compile fails too, it is
still counted that way.

**Precompiled headers.** clang's depfile lists the PCH's headers but not the
`.pch`, and gcc's lists neither the `.gch` nor its header, so a PCH joins the
manifest key from the command line: `-include-pch F`, `-Xclang -include-pch
-Xclang F`, and `-include H` with `H.pch` or `H.gch` beside `H` in the working
directory or any directory on the include path (the `CPATH` family's
included). It is identified, in order of
preference, by `<pch>.sum` (what a build writes for ccache's
`pch_external_checksum`: a digest of the PCH's inputs, stable where clang's
PCH bytes are not), by `<pch>.vcache-key` (written by vcache beside a PCH it
builds or restores: a digest of the PCH's inputs with paths canonicalised, the
same in every worktree), or by the PCH's bytes. A side file older than the PCH is ignored: it
describes an earlier build. A directory of `.gch` files is declined. gcc also
uses `H.gch` for a plain `#include "H"`, again listing neither. The trace sees
gcc open the `.gch`, and probe for one that is not there yet; a patched gcc
names the one it read in its depfile and the one it probed in its list. From
the depfile alone, under gcc a state is not recorded while
any directory an `#include` could search holds a `.gch` the key does not cover.

**What declines a state.** A file the depfile names that cannot be read, that
was modified or had its inode changed at or after the moment the compile
started (it may not be the version the compile read; each file is read before
it is stat'ed, so an edit between the two still shows), or whose bytes contain
`__DATE__`, `__TIME__` or `__TIMESTAMP__`: the digest stays the same while the
expansion changes, which only the preprocessor key would see. That includes a
`.gch` or `.pch` the depfile names (a patched compiler names the PCH it read)
and the key does not identify, so such a state is declined: every PCH holds the
name `__DATE__`, and gcc's depfile does not name the header the PCH was built
from, whose time macros nothing else would see. A file that later gains such a
token changes its digest, so the state is checked again when it is next
written. The same tokens on the command line (`-DSTAMP=__TIME__`) keep the
compile off the manifest altogether. A depfile that does not name the source
declines the state, since a state with nothing to check would always match.
From the depfile alone under gcc, a file outside the compiler's own include
directories (as `<compiler> -E -v` lists them, memoised under
`<cache>/sysinc/`) whose bytes use `__has_include` declines it too: clang's
depfile names a header `__has_include` found and no `#include` read, gcc's does
not (a patched gcc's does, but no depfile names a probe that found nothing),
and the trace sees both. For a derived key, `.incbin` declines it as well,
since no preprocessed text was checked for it. A declined state leaves the
entry stored under the preprocessor key as usual.

**Absent paths.** A depfile names the files a compile read, not the paths the
preprocessor probed and did not find. A header created earlier on the include
path than a recorded one, or one a `__has_include` would now find, changes
nothing a depfile recorded; a recorded absent path that now exists rejects the
state. The other side, a header `__has_include` (or `__has_include_next`) found
but no `#include` read, is among the trace's files, as it is in clang's
depfile, so deleting it rejects the state too. This holds in the system's
include directories as well: a `-devel` package installed or removed changes
`/usr/include`. Only the compiler's own trees outside every include directory
are left out, on the assumption that they change only with the compiler, which
its fingerprint follows.

**From the depfile alone.** Without the trace the shadowing hole is back: a new
header that would be found earlier on the include path than a recorded one is
invisible, so a lookup can still hit with the old answer until something
recorded changes. ccache's direct and depend modes have the same hole and
document it, as does `dep_scan` here. The deleted-file case stays covered: clang
lists a header `__has_include` found in its depfile, and under gcc the
`__has_include` rule above declines such states. A system header that probes is
exempt, on the same assumption about the compiler's own include directories.

## Compiler identity

`VCACHE_COMPILER_CHECK` selects:

- `version` (default) — hash of `<compiler> -v` output. Machine-independent, so
  entries are shareable through S3. The result is memoised in the cache
  directory keyed by the binary's path, size and mtime, so the extra process
  runs once per toolchain version rather than once per compilation. Paths under
  a configured root are canonicalised out of the banner before it is hashed. An
  `absent-deps-<n>` token stays in: a compiler built with that local patch (it
  reports the paths it looked for and did not find) produces the stock
  compiler's objects but names the PCH it read in its depfile, so the two do
  not share entries.
- `content` — hash the driver binary. Machine-specific in practice.
- `mtime` — canonicalised path, size and mtime only. Fastest, least safe across
  machines.

`content` and `mtime` still read the memoised banner for the `absent-deps-<n>`
version, without hashing it.

`version` is the default specifically because a shared remote cache is a first-
class use case, and mtime differs across machines for identical toolchains.

## Storage

One opaque blob per key, so backends stay trivial. The blob is a sequence of
tagged sections (object, dependency file, diagnostics, metadata, and repeated
named files for Rust's multi-artifact output), prefixed by a BLAKE3 digest of
the body. Unknown section kinds are skipped, so a newer vcache's entries stay
readable.

The checksum is not paranoia: a silently corrupt object linked into a binary is
far worse than a cache miss, and object storage plus local disk gives two
independent opportunities for truncation.

**Sidecar files.** Some outputs are too large to carry inside an entry. A link
output, and a Rust output of 4 MiB or more other than its `.d`, is kept in the
disk cache as a content-addressed file of its own, `<dir>/<digest[0:2]>/<digest>.<suffix>`
(`linkout`, `rustout`), and the entry names it by digest (a `kFile` section for
link, a `kSidecarFile` section for Rust). Both go through `DiskStorage::PutFile`
and `GetFile`. A sidecar is created by `CloneFile`, a reflink where the
filesystem has one, and restored the same way into an exclusive temporary
beside the output. The hit hashes that clone, not the stored file, and renames
it into place only if it matches; a Rust hit verifies every sidecar before it
places anything or forwards diagnostics. The checksum covers only the digest,
so the hash is what proves the bytes. A Rust sidecar that fails it is removed,
so failed hits cannot keep a torn file the freshest in the store.

Sidecars sit in the shards, so `--trim`, `--clear` and the size budget count
and evict them like entries, by their own mtime; `GetFile` refreshes it on
every hit. Eviction is per file: an entry whose sidecar has gone is a failed
hit and a recompile, never a wrong output. Storing a new sidecar runs the
shard's eviction check, without the scratch sweep, since a sidecar is stored
before its compile has placed the outputs. They are not fsynced; a lost one
fails its hash. Rust sidecars are used only with a disk cache and no S3, so
every entry that can reach another host is whole; the entry key records which
of the two forms it has. A Rust miss stores each large output as a sidecar
straight from the stage dir, before placing it by rename, so it is never read
into memory.

The metadata section is not part of any cache key. A stored compile or link
appends `max_rss_kb` and `wall_ms`, taken from `wait4` on that process. For a
gcc or clang driver the figure is the peak of the largest process the driver
waited for: cc1 or cc1plus, and for an `-flto` link the linker together with
the LTO jobs it waited for. The value is recorded as reported. macOS counts
`ru_maxrss` in bytes and Linux in kibibytes; the stored number is kibibytes
on both. It also includes vcache's own resident set at the fork, which on a
heavy translation unit was 368,068 KiB through vcache against 368,992 KiB for
the compiler run directly.

The same two numbers are kept, for misses only, under `<cache>/costs/`. Each
file holds the last eight observations of one codegen class (operation,
canonical source or link output, language, and the optimisation and debug
flags — not the source text and not include paths). `--clear` removes cache
entries and leaves `costs/` alone, as it leaves the compiler-version memos.
A failure to write one is logged (`cost: could not record`) and does not fail
the build. `vcache --show-costs` summarises the records.

Each compile, dependency scan and link works in a scratch directory under
`<cache>/tmp/` (the system temp dir only when that cannot be created). It is
removed when vcache exits; a vcache that is killed mid-compile cannot, so
`--trim`, and a store that triggers an eviction check, sweep them. The name
carries the owner's pid and a tag for its pid namespace and boot,
`<prefix><pid>.<tag>-XXXXXX`. A directory whose owner is still running is kept
however old its mtime, since a long compile rewriting existing files does not
change it; one whose owner has exited goes after ten minutes. A directory the
sweep cannot check (another container, an earlier boot, an older vcache) goes
once it has not been modified for six hours.

**Disk.** Entries live at `<dir>/<first-2-hex>/<rest>`, sharded 256 ways. Each
store checks only its own shard against `max_size/256` and evicts LRU within it,
which bounds eviction work to 1/256th of the cache — cheap enough to run inline
during a build. `mtime` is refreshed on read and used as the LRU timestamp,
because `relatime` mounts do not reliably update `atime`.

**S3.** GET and PUT of one object, signed with SigV4 over libcurl. Avoiding the
AWS SDK keeps the dependency footprint small; the signing code is ~60 lines and
is covered by known-answer tests. `403` is treated the same as `404`, since
buckets without `ListBucket` permission return it for a missing key.

**Chain.** First hit wins; the hit is written back into every faster layer it
passed through. Any backend error is reported as a miss.

## Restoring outputs

A hit writes each output to a temporary file beside it and renames it into
place, so a reader never sees a partial file. Outputs are not fsynced; cache
entries are. A torn entry would be served to every later build. A torn output
is what a crash during an uncached build leaves too, because rustc and gcc do
not fsync what they write either. If the machine crashes after vcache returns
and before the output reaches disk, the file can be empty or short while
cargo's fingerprint, which checks mtimes and not contents, still counts it
fresh. `cargo clean -p <crate>` recovers.

A Rust miss moves rustc's outputs from the stage dir into the output directory
with `rename(2)`, so the file in place is the one rustc wrote, with rustc's own
mode bits. When the rename fails with `EXDEV`, because the scratch dir fell
back to a `$TMPDIR` on another filesystem or the output directory is on one,
the file is copied with `CloneFile` and given its execute bits under the
umask. The `.d` is the exception: it names the stage dir, so it is written from
the canonicalised copy captured for the entry and localised as a hit's is.
Capture still reads every file once to build the entry.

`VCACHE_LOG` gives each placement one line, so a build's log can be summed.
"restored" and "placed" time the writes alone; "hit total" runs from asking the
cache for the entry to the last output in place, so it adds reading the entry,
checking its checksum and unpacking it. Without the fsync that is most of a
Rust hit (below).

```
rust: restored 3 files, 41630234 bytes in 28.512 ms    a Rust hit
rust: hit total 61.077 ms
rust: placed 3 files, 41630234 bytes in 9.204 ms       a Rust miss
hit: restored 2 files, 182311 bytes in 0.207 ms        a C or C++ hit
hit total 0.391 ms
```

### Measurements

A clean `cargo build` (dev profile) of sunbird's root workspace on a 128-core
host, through `build-limited` (30 CPUs): 344 rustc runs, of which a warm build
serves 341 from the cache, restoring 4,801 MiB. Store and target directory on
one XFS filesystem, entries in the page cache, daemon off, `sync` before each
build. Hit builds are four interleaved samples per binary, median and range;
the fill is one sample. A summed time adds up every process's line, so with
builds running in parallel it exceeds the wall time.

| | origin/fbg | fsync kept | no fsync |
|---|---|---|---|
| warm build, wall | 6.6 s (6.3–7.5) | 6.4 s (5.6–8.7) | 3.6 s (3.3–3.9) |
| warm build, summed "hit total" | not logged | 42.6 s (36.0–73.0) | 5.7 s (5.1–6.0) |
| warm build, summed "restored" | not logged | 37.2 s (30.5–68.2) | 1.2 s (0.7–1.6) |
| fill, wall | 129.8 s | 131.7 s | 118.1 s |
| fill, summed "placed" | not logged | 21.6 s | 2.3 s |

"fsync kept" is this change with only its `durable = false` arguments
reverted. Without the fsync a warm build's restore cost is mostly reading and
checking entries: 4.5 s of the 5.7 s. The `fastdev` profile (no debug info,
1,234 MiB restored) showed the same shape in two unsynced samples: summed
"restored" 33.0 s and 37.8 s with the fsync, 1.4 s and 1.1 s without.

Placing a miss by rename, two synced fills each, interleaved: summed "placed"
1,458 ms and 892 ms when rustc's outputs were read back and written again,
48.6 ms and 47.4 ms by rename; fill wall 109.1 s and 103.5 s, then 102.8 s and
100.4 s. Warm builds are unchanged, at 3.3 s and 4.2 s against 3.6 s and 4.2 s.

Sidecars for outputs of 8 MiB or more, four interleaved warm builds against the
same build with every output inline (median, range; the host was busier than in
the runs above, so compare within the table only):

| | inline | sidecars |
|---|---|---|
| warm build, wall | 4.8 s (3.4–5.6) | 3.0 s (2.3–3.1) |
| warm build, summed "hit total" | 12.6 s (5.5–17.3) | 6.4 s (3.5–8.9) |
| warm build, summed "restored" | 6.2 s (1.1–8.3) | 5.1 s (3.0–6.7) |
| fill, wall (two runs) | 106.0 s, 102.6 s | 98.3 s, 100.7 s |
| store after one fill | 682 entries, 5,035 MB | 682 entries, 728 MB, and 53 sidecars, 4,307 MB |

With sidecars, "restored" includes cloning 4.3 GB and hashing every clone; the
entries a warm build reads shrink from 5.0 GB to 0.7 GB, which is where "hit
total" falls. The sidecars ran from 8.1 MiB to 1,056 MiB, median 13.4 MiB.
`HashFile` on the largest took 0.13–0.15 s from the page cache and 0.36–0.38 s
with its pages dropped first (NVMe), three runs each.

The threshold was then varied from 1 MiB to 32 MiB on the same build, one fill
and three interleaved warm builds per variant, 343 hits each (median, range):

| threshold | sidecars after one fill | warm build, wall | summed "hit total" | `--trim` walk |
|---|---|---|---|---|
| 1 MiB | 249 | 4.3 s (3.9–5.5) | 10.0 s | 8 ms |
| 2 MiB | 170 | 4.0 s (3.8–5.4) | 11.7 s | 27 ms |
| 4 MiB | 135 | 4.5 s (3.8–4.7) | 11.8 s | 33 ms |
| 8 MiB | 55 | 7.6 s (5.5–8.1) | 14.7 s | 27 ms |
| 16 MiB | 24 | 7.5 s (5.4–8.8) | 18.7 s | 29 ms |
| 32 MiB | 12 | 7.0 s (6.1–8.2) | 31.5 s | 29 ms |

1, 2 and 4 MiB are one group and 8 MiB and above another; the trim walk, the
reason 8 MiB was first chosen, is noise beside a 40 GB store that already holds
about 17,000 files. 4 MiB takes the gain with the fewest sidecars, each of which
is evicted on its own mtime (below). The medians overlap at the edges and the
host was busy, so the choice rests on the grouping, not on the 1–4 MiB order.

## Incoming prefix-map flags

A caller-supplied `-ffile-prefix-map` and vcache's own would both be on the
command line, and gcc resolves overlaps last-match-wins — so whichever vcache
appended would silently win. Since vcache's whole job is deciding what paths end
up in the output, quietly overriding a mapping the build system asked for would
change that build's artifacts without telling anyone.

The default is therefore to refuse and name the three ways to decide
(`--vcache-allow-prefix-maps`, `VCACHE_INCOMING_PREFIX_MAPS`,
`vcache.incoming_prefix_maps`), with command line beating environment beating
config file. `keep` passes the flags through and marks the compilation
uncacheable, because the resulting mapping is no longer one vcache can reason
about.

## Runtime dependencies

vcache is exec'd once per compilation, which makes the dynamic-loader cost part
of its steady-state overhead rather than a one-off. Linking libcurl directly
brought the load set to 34 shared objects and process startup to 6.1 ms;
measured in isolation, the libcurl chain accounted for ~2.4 ms of that and
libcrypto ~0.5 ms.

Both are now avoided. libcurl is opened with `dlopen` in the `S3Storage`
constructor and reached through a resolved function-pointer table
(`storage/curl_api.h`), so a disk-only build never maps it; SHA-256 and HMAC are
vendored in `hash/sha256.cc`, since SigV4 needed exactly three OpenSSL
functions. The result is four `ldd` entries and 3.0 ms of startup.

Three details worth knowing if you touch this code. `curl/typecheck-gcc.h`
redefines `curl_easy_setopt` and `curl_easy_getinfo` as macros, which would
rewrite calls made through function pointers, so `curl_api.h` defines
`CURL_DISABLE_TYPECHECK` before including the header. The loader tries several
sonames (`libcurl.so.4`, `libcurl-gnutls.so.4`, ...) because Debian and Ubuntu
ship TLS-backend-specific builds under different names. And on a host without
curl's headers the build uses `third-party/curl` instead, which declares only
the types and constants vcache uses, so a new `CURLOPT_` has to be added there
as well.

The vendored SHA-256 is not a general-purpose crypto primitive and makes no
constant-time claims: it signs outbound requests with a key the process already
holds. It is covered by the FIPS 180-4 examples, thirteen lengths clustered
around the 55/56 and 63/64/65 padding boundaries where a hand-written `Final()`
typically breaks, streaming-versus-one-shot equivalence, and the RFC 4231 HMAC
vectors including the key-longer-than-block case.

## Failure policy

vcache never breaks a build it could not cache. Every failure path — unparseable
arguments, preprocessing failure, temp-directory failure, unreachable S3, a
corrupt entry, an unwritable object — falls back to running the compiler exactly
as invoked. Statistics record which path was taken, and `VCACHE_LOG` explains
each decision, including the compiler's stderr when preprocessing fails. That
last detail was added after a systematic bug (dependency flags leaking into the
`-E` command) silently degraded every compile to a passthrough; the counter was
visible but the reason was not.

## Concurrency

A parallel build runs many independent vcache processes. By default there is
no daemon. With one ([daemon.md](daemon.md)), the compiles still run in
parallel and only the cache layers move into the shared process, so everything
below holds in both cases: the daemon's threads write the disk layer exactly as
separate processes do.

- Cache writes go through a temp file plus `rename(2)`, so a reader never sees a
  partial entry.
- The statistics file is updated under `flock(2)` for a few microseconds per
  compilation.
- Two processes computing the same key simply both compile and both store the
  same bytes by default. With `daemon.single_flight` enabled, a compile session
  holds a memory-only key lease and peers wait within a bounded interval before
  restoring the stored result or compiling themselves. This can avoid repeated
  work across concurrent worktrees; it remains off by default.
- A manifest is re-read just before it is written and merged, so two worktrees
  that each add a state keep both. The gap that remains is the one Get and the
  one Put. `VCACHE_TEST_PAUSE_BEFORE_MANIFEST_PUT` names a fifo; a test write
  to it releases the process that is waiting to re-read. A path that is not a
  fifo is ignored, and a fifo with no writer is given up after 60 seconds.
  `VCACHE_TEST_PAUSE_BOUND_MS` shrinks that wait when a test sets it to a
  positive number of milliseconds no larger than 60000. Unset, the pause
  does nothing.
  `vcache --test-put KEY` reads one blob from stdin and stores it through a
  running daemon, so a test can put two values of one key while an upload is
  still in flight. `VCACHE_TEST_MAX_HELD_BYTES` shrinks the daemon's held-blob
  cap for that same test. Unset, the cap stays 1 GiB.
  `VCACHE_TEST_REFUSAL_WAIT_MS` shrinks how long a refused re-put waits for
  the in-flight upload, when set to a positive number of milliseconds no
  larger than half the client reply timeout. Unset, that wait stays 150
  seconds. `MOCK_S3_DENY_PUT_MIN_BYTES` makes the test bucket answer 403 for
  a PUT whose body is at least that many bytes.

## Parsing choices

Boost.Spirit X3 handles dependency files, which are a genuine small grammar:
line continuations, backslash-escaped spaces, `$$` for a literal dollar, and
`-MP`'s prerequisite-free phony rules. The same grammar serves gcc's `.d` output
and rustc's `--emit=dep-info`.

Boost is vendored as a subset rather than in full: 932 of 15,828 headers, 4.2 MB
instead of 104 MB. `third-party/regen-boost-subset.sh` derives the list with
`-H` and then verifies it by recompiling everything against the subset alone.

`-H` rather than `-MM` for a specific reason. Boost marks several headers with
`#pragma GCC system_header` (`boost/config/detail/suffix.hpp` among them), and
`-MM` omits system headers *and everything they include* — so it silently drops
genuine dependencies such as `boost/config/helper_macros.hpp`, which
`suffix.hpp` includes unconditionally. That produces a subset which looks
complete, passes a `-O1` syntax check, and then fails the real build. `-H`
reports every header actually opened and is exact.

Compiler command lines are not grammar-shaped — they are a flag list where the
hard part is a table of which options consume a following argument — so those
use a table-driven scanner. Preprocessed output uses a streaming scanner because
it is hot: tens of megabytes per compilation, of which only lines starting with
`#` need any work.

## Known limitations

- **A preprocessor hit records no manifest state.** States are recorded from
  the depfile and the trace of a compile that ran. A lookup whose manifest key has no matching
  state but whose preprocessor key hits (a header edited only in a comment, a
  PCH rebuilt with different bytes and no `.sum`, an entry another tree stored
  in preprocessor mode) is served by the preprocessor, and keeps being served
  that way until the next miss records a state.
- **Linking is cached only on request** (`VCACHE_LINK_CACHE=1`, Linux only, traced
  through `vcache-fstrace.so`); ccache and sccache do not cache it.
- **Objective-C/C++** are parsed and treated as cacheable but are untested here,
  since no such toolchain was available.
- **Precompiled headers** are covered by the integration tests for clang
  (`-include-pch`) and for gcc (`-include` with a `.gch` beside the header).
  Both hit across directories because `-E` expands the header text, so the key
  covers the header although the PCH bytes never enter it. The manifest key
  names the PCH too (see above). gcc's `-fpch-preprocess` swaps that text for a
  pragma naming the `.gch`, so it is declined.
- **A PCH build is cached only at the same paths.** A build with `-x c-header`,
  `-x c++-header` or clang's `-Xclang -emit-pch` (CMake's form included) is
  keyed like any compile, from the header preprocessed with `-dD`: the PCH
  hands its macro definitions to every compile that loads it, and plain `-E`
  drops them. Its key, and its manifest key, hold the working directory and
  the command line exactly as typed, not canonicalised. A PCH records the
  absolute path of every input, even one named relatively, and clang 22 refuses
  to load it once
  one is missing ("could not find file ... referenced by AST file"); with
  `-fno-pch-timestamp` it checks only their sizes. So a rebuild of the same
  tree hits, and so do containers that mount every worktree at one path, but
  two trees at different host paths never share a PCH entry.
  A clang PCH build needs `-Xclang -fno-pch-timestamp`. Without it the PCH
  holds its headers' mtimes and clang rejects a restored copy once any of them
  is touched, so the build is declined as `pch timestamps`. gcc does not check
  header mtimes, and its `.gch` bytes differ between identical builds, which a
  stored copy does not care about. A PCH build without `-o` is declined
  (`no -o`), because gcc and clang name the default output differently.
  After it places a PCH, on a miss or a hit, vcache writes `<output>.vcache-key`
  beside it: one line, a digest of the PCH's inputs with paths canonicalised
  (compiler, flags, `-dD` header text), so a consumer's manifest can name the
  PCH without hashing tens of megabytes. Unlike the entry's key it carries no
  paths, so consumers in two worktrees name the PCH the same way and share
  manifest states although each worktree builds its own PCH. The identity
  travels in the entry's meta, so a hit writes it too. It is written after the
  PCH, so no reader pairs a new identity with an old PCH, and every PCH build
  removes the old file first, so a failed or uncached build leaves none.
- **How a PCH was built is not in the key of a compile that uses it.** clang
  accepts a PCH that defines a macro the compile does not, and gcc uses a
  `.gch` it never checks against an edited header; in both cases the expanded text describes a different
  compilation from the one that ran. Build PCHs with the flags and headers of
  the compiles that use them, as build systems do. A stale `.gch` stores an
  object under the edited header's key. With an S3 tier that entry is shared
  with every other host and stays until it expires, so clearing the local cache
  does not remove it. clang's `-fno-validate-pch`, which removes the one check
  that catches a stale PCH, is declined.
- **GCS** is not implemented. The `Storage` interface is where it would go.
