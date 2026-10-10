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

- its own key-format version
- compiler identity and the resolved native target
- the sorted set of canonical root targets
- the canonical working directory, since relative `-I` paths and relative
  sources resolve against it, and the source's canonical path, since quoted
  includes resolve beside it
- the source's contents
- the command line minus the source, `-o` and the depfile path, with `-I`,
  `-D`, `-include` and friends kept, and the paths in them (joined ones such as
  `-I/abs/dir` too) canonicalised; `-c` and `-S` are told apart. A `-D` value
  is kept as written, path or not: it reaches the object as written
- the files named by keyed flags (sanitizer lists, profiles), by content
- `hash_env_vars`, and `CPATH`, `C_INCLUDE_PATH`, `CPLUS_INCLUDE_PATH`,
  `OBJC_INCLUDE_PATH` and `OBJCPLUS_INCLUDE_PATH`, which add include
  directories
- the identity of every precompiled header the command names

A state lists every file the depfile of an earlier compile of that key named,
with its digest, and the result key that compile stored under. A lookup takes
the first state whose files all still hash the same, through the file-identity
memo, so an unchanged header costs a `stat`. A state that matches but whose
entry is gone, or whose entry lacks the depfile this compile asked for, falls
through to the preprocessor; the preprocessor usually computes the same key,
which is then not asked for again.

**Where the include set comes from.** The compile's own depfile, as in ccache's
depend mode. A command with `-MD` writes one. A command with no dependency
flags gets `-MD -MF <scratch> -MT <output>` added; `-MD` writes the dependency
list beside the object and changes nothing in it, which a test checks against
the compiler run directly, and the scratch file is never stored, so the entry
looks like one from an ordinary compile. `-MMD` omits system headers, so a
command with it keeps to the preprocessor.

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
directory or any directory on the include path. It is identified, in order of
preference, by `<pch>.sum` (what a build writes for ccache's
`pch_external_checksum`: a digest of the PCH's inputs, stable where clang's
PCH bytes are not), by `<pch>.vcache-key` (written by vcache beside a PCH it
caches), or by the PCH's bytes. A side file older than the PCH is ignored: it
describes an earlier build. A directory of `.gch` files is declined. gcc also
uses `H.gch` for a plain `#include "H"`, again listing neither, so under gcc a
state is not recorded while any directory an `#include` could search holds a
`.gch` the key does not cover.

**What declines a state.** A file the depfile names that cannot be read, that
was modified or had its inode changed at or after the moment the compile
started (it may not be the version the compile read), or whose bytes contain
`__DATE__`, `__TIME__` or `__TIMESTAMP__`: the digest stays the same while the
expansion changes, which only the preprocessor key would see. A file that later
gains such a token changes its digest, so the state is checked again when it is
next written. For a derived key, `.incbin` declines it as well, since no
preprocessed text was checked for it. A declined state leaves the entry stored
under the preprocessor key as usual.

**The shadowing hole.** A depfile names the files a compile read, not the paths
the preprocessor probed and did not find. A new header that would be found
earlier on the include path than a recorded one is invisible to the manifest,
so a lookup can still hit with the old answer until something recorded
changes. ccache's direct and depend modes have the same hole and document it,
as does `dep_scan` here. `vcache-fstrace.so` already records absent paths (`M`
lines) for the link cache; recording them for compiles would close it.

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
  the depfile of a compile that ran. A lookup whose manifest key has no matching
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
  drops them. Its key also holds the working directory and the command line
  exactly as typed, not canonicalised. A PCH records the absolute path of
  every input, even one named relatively, and clang 22 refuses to load it once
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
  beside it: one line, the PCH's cache key in hex, so a later key can name the
  PCH without hashing tens of megabytes. It is written after the PCH, so no
  reader pairs a new key with an old PCH, and every PCH build removes the old
  key first, so a failed or uncached build leaves none.
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
