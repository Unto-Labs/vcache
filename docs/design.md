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
  rust/
    rust_compile.*     the Rust pipeline
  storage/
    storage.h          backend interface + blob container format
    disk_storage.*     sharded local cache with LRU eviction
    s3_storage.*       SigV4 over libcurl
    chain.*            multi-level chain with read-through backfill
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

## Compiler identity

`VCACHE_COMPILER_CHECK` selects:

- `version` (default) — hash of `<compiler> -v` output. Machine-independent, so
  entries are shareable through S3. The result is memoised in the cache
  directory keyed by the binary's path and size, so the extra process runs once
  per toolchain rather than once per compilation.
- `content` — hash the driver binary. Machine-specific in practice.
- `mtime` — path and size only. Fastest, least safe across machines.

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

Two details worth knowing if you touch this code. `curl/typecheck-gcc.h`
redefines `curl_easy_setopt` and `curl_easy_getinfo` as macros, which would
rewrite calls made through function pointers, so `curl_api.h` defines
`CURL_DISABLE_TYPECHECK` before including the header. And the loader tries
several sonames (`libcurl.so.4`, `libcurl-gnutls.so.4`, ...) because Debian and
Ubuntu ship TLS-backend-specific builds under different names.

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

A parallel build runs many independent vcache processes. There is no daemon.

- Cache writes go through a temp file plus `rename(2)`, so a reader never sees a
  partial entry.
- The statistics file is updated under `flock(2)` for a few microseconds per
  compilation.
- Two processes computing the same key simply both compile and both store the
  same bytes. Adding cross-process locking to prevent that would cost more than
  the duplicated work.

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
  linemarker-free canonical paths for the recorded include set.
- **Linking is not cached**, matching ccache and sccache.
- **Objective-C/C++** are parsed and treated as cacheable but are untested here,
  since no such toolchain was available.
- **clang** is handled by the same code path as gcc and the flag tables cover
  both, but the measurements in `preprocessor-problem.md` were taken on gcc
  13.3.0 only; clang was not installed on the development machine.
- **GCS** is not implemented. The `Storage` interface is where it would go.
