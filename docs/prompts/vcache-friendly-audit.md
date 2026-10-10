# Prompt: audit a codebase for vcache friendliness

Copy everything below the line into a coding agent (Claude Code, Codex, Cursor, ...) started at
the root of the repository you want audited. The agent reads the repository and its build and
reports what to change. It edits nothing unless you ask it to afterwards.

---

vcache is a compiler cache for C, C++ and Rust. For C and C++ it keys each compile on the
preprocessed text (with the paths in its linemarkers canonicalised), the compiler's identity and
the codegen flags. For Rust it keys on the files and `env!` variables that
`rustc --emit=dep-info` reports, the codegen flags, and the content of every `--extern`
dependency. You declare source **roots** (`VCACHE_ROOTS`), and vcache rewrites them to fixed
canonical prefixes on the command line, so a compile in one checkout or worktree hits the entry
that another checkout stored. It never trusts an mtime. When it cannot cache a compile safely,
it runs the compiler uncached and records the reason. Reference: the vcache
[README](../../README.md), [quickstart](../quickstart.md), [configuration](../configuration.md),
[design](../design.md) and [daemon](../daemon.md).

You are auditing **this** repository and its build system for how well they suit vcache. Find
every concrete thing that makes a compile uncacheable, or that makes the same compile get a
different key in a second checkout, and report the fix. Do not edit files. Base every finding
on a file and line or on a captured command line. If you cannot, say so.

## 1. What to collect

1. The compile commands: `compile_commands.json` (CMake: `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`),
   or a verbose build log (`ninja -v`, `make V=1 VERBOSE=1`, `cargo build -v`). For Bazel, the
   `bazel aquery` output for the C, C++ and Rust actions.
2. The build configuration: `CMakeLists.txt` and `cmake/` modules, toolchain files and presets,
   Makefiles, `meson.build`, `Cargo.toml`, `.cargo/config.toml`, `build.rs` scripts, Bazel
   `BUILD` files and `.bazelrc`, and any script that wraps the build.
3. How the build reaches the compiler: `CMAKE_<LANG>_COMPILER_LAUNCHER` and
   `CMAKE_<LANG>_LINKER_LAUNCHER`, `RUSTC_WRAPPER`, `CC`/`CXX`, and directories of compiler
   symlinks on `PATH` (vcache's own masquerade directory, ccache's `/usr/lib64/ccache` or
   `/usr/lib/ccache`).
4. The environment the build exports: every `VCACHE_*` variable, `SOURCE_DATE_EPOCH`,
   `RUSTFLAGS`/`CARGO_ENCODED_RUSTFLAGS`, `CFLAGS`/`CXXFLAGS`, `CARGO_TARGET_DIR`, and any vcache
   config file (`$VCACHE_CONFIG`, `~/.config/vcache/config.toml`, `/etc/vcache/config.toml`).
   `vcache --show-config` prints the resolved result.
5. The layout: where checkouts and worktrees live, where each build directory and cargo target
   directory is relative to its checkout, and where generated sources are written.
6. If vcache is already in use: `vcache --show-stats` and `vcache --show-roots`, run with the
   same `VCACHE_DIR` and `VCACHE_CACHE_SIZE` as the build, and a build run with
   `VCACHE_LOG=<file>`.

## 2. What to check

Each item says why it matters to vcache, what to look for, and the fix. A quoted name is the
reason that `--show-stats` and `VCACHE_LOG` print.

### Wiring

- **Every checkout must map to the same canonical prefixes.** vcache rewrites a root to
  `/vcache/<basename>` unless you name it, so `/home/a/proj` and `/ci/build-123` never share.
  Use the `PATH=name` form (or `PATH=/absolute/target`) with the same name in every checkout,
  and run `vcache --show-roots` in two checkouts: the right-hand sides must match. Cover every
  tree whose path differs between checkouts or machines: the source tree, an out-of-tree build
  directory, a generated-source directory, a cargo target directory outside the workspace, and
  a toolchain installed at different paths. vcache canonicalises paths under a root in the
  compiler's `-v` banner, which is the default compiler identity.
- **vcache maps the working directory by default** (`VCACHE_MAP_CWD`, as `/vcache/cwd`),
  because `DW_AT_comp_dir` records it. Keep it on for C and C++ unless you have a reason.
- **Paths spelled with `//` or `/./`** (often a directory variable that already ends in `/`) do
  not match any root, so they get no mapping and no sharing. Look for them in the commands.
- **Wrappers.** A CMake compiler launcher wraps compiles only. Links need
  `CMAKE_<LANG>_LINKER_LAUNCHER`, and CMake has no launcher for `ASM`. Nested CMake projects
  (LLVM runtimes, ExternalProject) need the launchers passed again. With compiler symlinks on
  `PATH`, keep other wrappers' symlink directories off `PATH`: each wrapper runs the next
  same-named compiler, and vcache stops after four such hops. ccache's `base_dir`, `sloppiness`
  and `pch_external_checksum` have no vcache equivalent. Do not translate them.

### Command lines that vcache declines

vcache runs the real compiler uncached for each of these. Report how many commands carry each
one, and whether the build can drop it.

- "unsupported flag": `-fpch-preprocess` (gcc's `-E` then names the `.gch` instead of expanding
  the header), `-fno-validate-pch` (also as `-Xclang -fno-validate-pch`), `-fmodules`,
  `-fmodule-file=`, `-fmodule-map-file=`, `-fprebuilt-module-path=`, `-save-temps`,
  `-fsyntax-only`, `-specs=`, `-frepo`, the PGO flags (`-fprofile-generate`, `-fprofile-use`,
  `-fprofile-instr-use`, `-fauto-profile`), and `-MG`. `-MG` lets a dependency name a file that
  does not exist yet, which a manifest of file hashes cannot verify.
- "second output file": flags that write a companion file, which a cache entry would not
  restore: `--coverage`, `-ftest-coverage`, `-fprofile-arcs`, `-gsplit-dwarf`
  (`-gsplit-dwarf=single` is cached), `-fstack-usage`, `-fcallgraph-info`, `-fdump-*`,
  `-ftime-trace`, `-fsave-optimization-record`, `-fopt-info...=FILE`, `-MJ`,
  `-serialize-diagnostics`. Use them only in the configurations that need them.
- ".incbin": the string is somewhere in the preprocessed text. The assembler reads the file, so
  its contents never reach the key. Move the embed into one small TU, or generate a header that
  holds the bytes, so that they are in the text.
- "multiple inputs" (`cc -c a.c b.c`), "link" (no `-c`, which includes compile and link in one
  step; "compile and link" when link caching is on), "source from stdin" (`-` as the input),
  "preprocess only" (`-E`), "plain assembly" (`.s`; vcache caches `.S`), "unknown language".
  Give each `-c` one source. Compiler probes from CMake, configure scripts and cargo also count
  here. They are expected and are not findings.

### Paths that give one compile different keys in two checkouts

vcache canonicalises the paths in linemarkers, an argument that is a path under a root, and the
dependency file. Its own `-ffile-prefix-map` rewrites `__FILE__` and debug info. vcache does
**not** rewrite other text, or a path inside a longer argument.

- **The build already passes prefix maps.** By default, vcache refuses to run when it sees
  `-ffile-prefix-map`, `-fdebug-prefix-map`, `-fmacro-prefix-map`, `-fprofile-prefix-map` or
  `--remap-path-prefix`, and the compile fails. Remove them and let roots do the job (the
  `=/absolute/target` form of a root gives a chosen debug path), or set
  `incoming_prefix_maps = "strip"`. `keep` passes them through uncached ("kept prefix maps").
- **Paths inside joined flags** are keyed verbatim: `-fsanitize-ignorelist=/abs/...`,
  `-fprofile-sample-use=`, `-fplugin=`, `-frandomize-layout-seed-file=` (vcache also keys the
  file's content), and for rustc `-C link-arg=/abs/...`. With clang, vcache also keys
  linker-only flags on a compile line (`-L/abs`, `-Wl,-rpath,/abs`); with gcc it does not. Use
  paths relative to the build directory, or remove link flags from compile lines.
- **Absolute paths in `-D` values or generated headers.** `-I` and `-D` are not in the key, but
  the expansion of a macro is in the preprocessed text verbatim. A TU that uses
  `-DDATA_DIR="/home/a/proj/data"` misses in every other checkout. Under `-g3`/`-ggdb3`, gcc
  writes every command-line `#define` into its `-E` output, so an unused one also splits the
  key. Pass a relative path, or find the path at run time.
- **Dependency scans** (`-M`/`-MM` without `-c`) are cached against a manifest whose key is the
  command line *with* `-I` and `-D`. As for compile flags, only an argument that is a path is
  canonicalised: the joined form `-I/abs/inc` stays verbatim and splits the key per checkout,
  while `-I /abs/inc` under a root or a relative `-Iinc` does not. `VCACHE_DEP_SCAN=uncacheable`
  turns scan caching off ("dep scan policy").
- **`.S` files under CMake** are not wrapped. With `-g` they record the absolute source path in
  `.debug_line`, so every archive and link that includes one differs per checkout. Build that
  assembly without debug info and the objects match.

### Text that changes between builds

- **`__DATE__`, `__TIME__` and `__TIMESTAMP__`** expand into the preprocessed text, so a TU that
  uses them gets a new key every second, every day, or for each source mtime. Find them with
  `-Wdate-time` or grep. Keep them in one small TU (a `version.cc`), or pin them with
  `SOURCE_DATE_EPOCH` and list that variable in `hash_env_vars` so that the key follows it.
- **Generated sources** that hold absolute paths, timestamps, host names or a git hash miss in
  every TU that includes them, and in every checkout. Keep the changing part in one TU. vcache
  keys on content, so regenerating identical bytes costs nothing.

### `-march=native`

vcache caches it. It runs the compiler once with `-dM -E` to find the target that `native`
resolves to, and keys on that, so hosts with the same CPU share entries and other hosts do not.
A failed probe gives "native unresolved"; `VCACHE_NATIVE_TARGET=uncacheable` gives "native
policy". To share across different CPUs or with CI, name the target (`-march=x86-64-v3`). If
hosts with different CPUs share one cache directory and one host name, use `uncacheable`.

### Precompiled headers

vcache does not cache the PCH build itself (`-x c++-header` counts as "unknown language"), so
each tree builds its own PCH. vcache caches the consumers, because `-E` expands the header text
in place of the PCH, which puts the header in the key. Thus:

- A consumer's command line must have every include directory that the PCH producer had. gcc
  and clang can compile a consumer without them, but `-E` then fails and the compile counts as
  "preprocessor error". Compare producer and consumer flags.
- No `-fpch-preprocess` and no `-fno-validate-pch` (see above).
- The key does not record how the PCH was built. Build it with the same flags and headers as
  its consumers. A stale `.gch` stores an object under the key of the edited header.
- clang: build PCHs with `-Xclang -fno-pch-timestamp`. The PCH then holds no header mtimes, and
  clang accepts a copy restored into another tree. Any cache that restores PCHs needs this, and
  the PCH caching proposed for vcache requires it. Do not add build steps that depend on header
  mtimes.
- A `<pch>.sum` digest of the PCH inputs (compiler identity, flags, preprocessed header) is what
  ccache's `pch_external_checksum` reads. vcache's proposed manifest mode would read it, or a
  `<pch>.vcache-key` that vcache writes. Neither changes the hit rate today. Mention one only as
  preparation.

### Rust

- **Wiring**: `RUSTC_WRAPPER=vcache`, or a `rustc` symlink on `PATH`. Roots must cover the
  workspace and the target directory, with the same names in every checkout.
- **Command shape**: vcache caches a rustc run with `--out-dir`, `--emit=<kinds>` (no `=path`),
  no `-o`, and one input. Otherwise: "no --out-dir", "no --emit", "--emit with path",
  "explicit -o". Cargo's own probes (stdin, `-vV`) count as "source from stdin" and "no input
  file".
- **`OUT_DIR`** points into the target directory of each checkout. Keyed raw, every crate with
  a build script misses in other checkouts. Set `rust_path_env_vars = ["OUT_DIR"]`
  (`VCACHE_RUST_PATH_ENV_VARS=OUT_DIR`). vcache then does not store a crate that writes the path
  into its output (`const D: &str = env!("OUT_DIR")`), counted as "env path in output". That
  check finds only the literal path. Flag crates that derive a value from the path (its length,
  a hash), and proc-macro or dylib crates, which store it compressed: their entries could give
  one checkout's path to another.
- **Other path-valued `env!`/`option_env!`** (`CARGO_MANIFEST_DIR` and similar) are keyed with
  their raw value, so the crate misses in every other checkout. Use a relative path or
  `include_str!`. Add a variable to `rust_path_env_vars` only when the crate uses it to find
  files.
- **`include!`, `include_str!` and `include_bytes!` of generated files** are keyed by canonical
  path and content. The file must be under a root, and its content must be deterministic.
- **Build scripts** that write absolute paths, timestamps or git state into generated code
  change the output of that crate. vcache keys `--extern` dependencies by content, so every
  crate that depends on it misses too.
- **`--remap-path-prefix`** in `RUSTFLAGS`, `.cargo/config.toml` or a profile: as for the C
  prefix maps above.
- **vcache keys every `-C` value verbatim** except `incremental=DIR`, and also `--cfg` and the
  other flags. It leaves out only the paths of `-L`, `--sysroot`, `--out-dir` and `--extern`.
  Compare the rustc command of one crate in two checkouts: a `-C metadata=`,
  `-C extra-filename=` or `--cfg` value that differs per checkout makes every crate miss.
- **Incremental**: cached. The directory is not in the key, but on or off is, so a profile must
  agree between checkouts. A hit does not touch the session state, so the first edit to a crate
  after a hit compiles that crate from scratch once.

### Links

Link caching is off by default and Linux only (`VCACHE_LINK_CACHE=1`); vcache traces each link
through `vcache-fstrace.so`. It declines `--build-id=uuid` ("build-id=uuid": the output is not a
function of the inputs), compile and link in one command, no `-o`, an inherited `LD_PRELOAD`, a
missing tracer, and a disabled disk cache. With link caching off, links count as "link" or
"multiple inputs". That is expected.

### Many checkouts, concurrency and cache size

- **Daemon and single flight.** With `VCACHE_DAEMON=auto` and `VCACHE_DAEMON_SINGLE_FLIGHT=1`,
  concurrent builds compile each missed key once. Measured on two and three worktrees, cold
  builds were 5-26% faster. A daemon serves only clients whose cache settings match its own
  (directory, size, read-only, S3 bucket, prefix, endpoint and identity). It refuses a client
  that differs, and that client runs in-process with no error in the build, so every checkout
  must export the same cache settings.
- **Size.** The default disk budget is 10 GiB, evicted least-recently-used per shard. Size it
  for every checkout and branch that shares the cache, and compare `cache size` in
  `--show-stats` with the limit.
- **Shared S3.** A CI job that reads a bucket that developers fill should set
  `VCACHE_READONLY=1`. Change the prefix to invalidate the whole cache.

### Reading `--show-stats`

`hit rate` is hits divided by hits plus misses. Uncacheable runs are not in it, so read the
reasons too. The "uncacheable" reasons are the command lines and features above. "preprocess
failed" ("preprocessor error", "no rust dep-info") means that `-E` or the dep-info run failed;
the log holds the compiler's stderr. The "passthrough" reasons ("no temp dir", "no cache key",
"unreadable source", "capture failed", ...) are local failures. "no cache key" usually means
that vcache could not read a file that a keyed flag names. Many misses in a second checkout
with no uncacheable reasons mean a key differs: roots, paths in text, or flags.

## 3. How to verify

Run in each checkout with the build's own environment (the same `VCACHE_DIR` and
`VCACHE_CACHE_SIZE`), one build at a time:

```console
$ vcache --show-roots                                   # in both checkouts; must agree
$ vcache --zero-stats
$ VCACHE_LOG=/tmp/vcache-a.log <clean build in checkout A>
$ vcache --show-stats
$ git worktree add ../audit-b HEAD && cd ../audit-b     # or a second clone elsewhere
$ vcache --zero-stats
$ VCACHE_LOG=/tmp/vcache-b.log <clean build in checkout B>
$ vcache --show-stats                                   # expect misses near 0
$ grep -E '\] (rust )?compile: ' /tmp/vcache-b.log      # each compile that missed in B
```

For a TU that missed in B, compare its `key input compiler=... roots=... native=...` lines in
the two logs. Then run the `preprocess:` command that each log records, replace each checkout's
path with one placeholder, and diff the outputs: the first difference that remains is the
cause. For a crate, diff the two `rust compile:` command lines and the `rust env-dep` lines.

## 4. Report format

Give a table, ordered by expected effect (the fix that recovers the most compiles first):

| Finding | Evidence (file:line, or the command line) | Fix | Expected effect |
| --- | --- | --- | --- |

"Expected effect" gives how many compiles or crates change from uncacheable or per-checkout
miss to hit, and what the fix costs (a correctness risk, a slower hit, a change to shipped
binaries). Then give one line: the hit-rate ceiling that you expect for a clean build in a
second checkout after the fixes, as hits divided by all compiler invocations, and what stays
uncached.
