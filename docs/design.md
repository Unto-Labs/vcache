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
- the sorted set of **canonical** root targets
- codegen-affecting flags, with any embedded paths canonicalised
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
- the command line minus `-o`/`-MF`, with `-I`, `-D` and friends kept verbatim
  and every path canonicalised
- the source's **canonical path** and its contents. The path matters because
  headers are found relative to the source: identical sources reached by
  different canonical paths read different files, and a state recorded for one
  would still verify for the other
- explicitly configured environment variables

Each state under that key lists the files the scan read with their digests, and
is served only while all of them still match.

## Compiler identity

`VCACHE_COMPILER_CHECK` selects:

- `version` (default) — hash of `<compiler> -v` output. Machine-independent, so
  entries are shareable through S3. The result is memoised in the cache
  directory keyed by the binary's path, size and mtime, so the extra process
  runs once per toolchain version rather than once per compilation. Paths under
  a configured root are canonicalised out of the banner before it is hashed.
- `content` — hash the driver binary. Machine-specific in practice.
- `mtime` — canonicalised path, size and mtime only. Fastest, least safe across
  machines.

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
output, and a Rust output of 8 MiB or more other than its `.d`, is kept in the
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

- **Preprocessor mode only.** Every compilation runs the preprocessor, even on a
  hit. Measured on one translation unit from this repository: a vcache hit takes
  ~77 ms against ~9 ms for a ccache hit and ~1.50 s to actually compile. The gap
  is preprocessing. ccache's "direct mode" avoids it by hashing the source plus
  a manifest of includes recorded from a previous run; the manifest has to be
  validated against the current file contents, which is why it is a real piece
  of work rather than a switch. This is the main remaining performance win, and
  the key derivation is already factored to accommodate it: the manifest path
  would replace step 4 of the pipeline and reuse everything else, including
  linemarker-free canonical paths for the recorded include set. The Rust side
  already has its equivalent: a manifest of earlier `--emit=dep-info` runs,
  verified by re-hashing every recorded file, lets a hit skip that run (see
  `rust_dep_info` in `configuration.md`).
- **Linking is cached only on Linux**, where `vcache-fstrace.so` can discover
  the input set (`configuration.md`, "Caching the link step"). ccache and
  sccache do not cache links at all.
- **Objective-C/C++** are parsed and treated as cacheable but are untested here,
  since no such toolchain was available.
- **Precompiled headers** are covered by the integration tests for clang
  (`-include-pch`) and for gcc (`-include` with a `.gch` beside the header).
  Both hit across directories because `-E` expands the header text, so the key
  covers the header although the PCH bytes never enter it. gcc's
  `-fpch-preprocess` swaps that text for a pragma naming the `.gch`, so it is
  declined.
- **How a PCH was built is not in the key.** clang accepts a PCH that defines a
  macro the compile does not, and gcc uses a `.gch` it never checks against an
  edited header; in both cases the expanded text describes a different
  compilation from the one that ran. Build PCHs with the flags and headers of
  the compiles that use them, as build systems do. A stale `.gch` stores an
  object under the edited header's key. With an S3 tier that entry is shared
  with every other host and stays until it expires, so clearing the local cache
  does not remove it. clang's `-fno-validate-pch`, which removes the one check
  that catches a stale PCH, is declined.
- **GCS** is not implemented. The `Storage` interface is where it would go.
