# vcache release notes

Each release is also described in its annotated tag (`git tag -n99 vX.Y.Z`).

## 2.0.0

A major release, because every cache key moved: entries written by 1.3.0 and
earlier are not read, and the first build after upgrading misses. New
behaviour beyond correctness fixes is off unless configured. Most of this
release was written by Richard Geary and ported from his fork.

Breaking:

- **Cache keys:** `vcache-key` v6, `vcache-depscan` v5, `vcache-link` v4,
  `vcache-rust-key` v5, `vcache-rust-manifest` v3, `vcache-cxx-manifest` v7.
- **Daemon protocol v2:** a running 1.3.0 daemon refuses 2.0.0 clients, which
  then run in-process. Stop the old daemon (`vcache --stop-daemon`) after
  upgrading.

Correctness fixes, on by default:

- **gcc precompiled headers:**
  - The key covers the `.gch` a compile actually loads (`-fpch-preprocess`),
    so a rebuilt PCH behind an unchanged header is no longer hidden.
  - A `.gch` without a `.vcache-key` or `.sum` sidecar is keyed by its bytes.
    gcc's are not reproducible, so its consumers miss after each PCH rebuild.
    A PCH built with `-c` through vcache gets the sidecar.
- **Assembly and objects:** `-S` and `-c` compiles get separate entries.
- **`-MD` and depfiles:** a `-MD` compile no longer hits an entry stored
  without a depfile.
- **Rust:**
  - `env-dep` lines are hashed rather than treated as sources;
  - `-C incremental`'s directory stays out of the key;
  - path-valued env deps are keyed canonically (`rust_path_env_vars`);
  - JSON artifact notifications name the placed output on hits and misses.
- **Output placement:**
  - a symlink output is never stored as its target's bytes;
  - execute bits honour the umask;
  - clone and atomic-write fixes.
- **Scratch directories:** they live under `<cache>/tmp`, and `--trim` sweeps
  those a killed compile left. The owner holds an `flock`, so a live long
  compile is never swept.
- **Dependency scans:** they key on the canonical source path, and share
  across trees with joined include flags (`-I/abs/inc`).
- **Masquerade guard:** vcache stops, with an error after four hops, when
  another wrapper on `PATH` runs it again.
- **Build portability:** vcache builds without libcurl headers, on glibc
  older than 2.27, and links libstdc++ statically only when the archive
  exists.

Opt-in:

- **C/C++ manifest mode (`cxx_manifest`)**, like ccache's depend mode. A hit
  hashes the source and a recorded set of headers, and skips the
  preprocessor.
  - On Linux, an `LD_PRELOAD` tracer also records the paths the preprocessor
    probed and did not find. A header created earlier on the include path
    then misses instead of serving a stale object.
  - A preprocessor hit records a manifest state from its `-E` run.
  - `cxx_manifest_absent_deps` takes the absent paths from a compiler patched
    to list them (`-fabsent-deps=`), where there is no tracer.
- **Daemon scheduler (issue #24):**
  - single-flight leases for identical concurrent compiles;
  - memory admission against the compiler tree's RSS;
  - a jobserver pool, Linux-only, which needs GNU make 4.4 or newer.

Also, on by default:

- **Rust large outputs:** a miss places its outputs by rename, copying only
  across filesystems. With a disk cache and no S3, outputs of 4 MiB or more
  are stored as content-addressed sidecars and restored by clone. With S3
  they stay inline, so the remote tier carries them.
- **Stats:** uncacheable and passthrough decisions are broken down by reason
  (`--show-stats`).
- **Hit path:** preprocessed text is hashed in large batches, and restored
  outputs are not fsynced.

## 1.3.0

An optional cache daemon: one long-lived process per cache directory that owns
the cache layers on behalf of every compile. Off by default; `VCACHE_DAEMON=auto`
starts one on first use, `VCACHE_DAEMON=on` uses one that is already running.

- S3 uploads are asynchronous. A store writes the disk layer and returns; the
  upload runs on a background worker, so neither the compile nor the next make
  job waits on a PUT.
- S3 connections are kept between requests, and compiles no longer load
  libcurl. Against the bundled mock bucket (`tests/mock_s3.py`) with 20 ms per
  request and 60 ms per new connection, a 256-file `-j16` build served from S3
  went from 2.33 s to 1.41 s, and a cold build from 10.63 s to 9.73 s. These are
  mock figures, not measurements against AWS.
- Pending uploads are journalled on disk, so a daemon that is killed or crashes
  leaves them for the next one to send.
- A daemon serves only clients with the same cache settings (directory, size,
  read-only, bucket, prefix, endpoint, credentials identity); anything else is
  refused and runs in-process. A missing, dead or refusing daemon never breaks
  a build.
- New commands: `--start-daemon`, `--stop-daemon`, `--daemon-status`,
  `--daemon-foreground`. `--stop-daemon` drains the upload queue and, under
  `--error-on-cache-media-failure`, exits 90 if any upload failed. With uploads
  now asynchronous, that is where a CI job checks them.

The cache key version has not moved, and the entry format is unchanged, so
entries written by 1.2.x stay readable, with or without the daemon.

[docs/daemon.md](docs/daemon.md) has the design, configuration and measurements.
