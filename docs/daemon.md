# The vcache daemon

vcache is a fresh process per compilation, and some costs can never be
amortised by a process that lives for one compile: opening an S3 connection
(TCP plus TLS), loading libcurl, and waiting for an upload to finish before
make is allowed to start the next job. The daemon is one long-lived process per
cache directory that takes those over. Compiles still do everything that needs
their own environment — argument parsing, root mapping, preprocessing, hashing,
running the compiler — and hand the daemon only finished keys and blobs.

It is **off by default** and changes nothing until turned on.

## Turning it on

```sh
export VCACHE_DAEMON=auto     # start one on first use
```

or in `config.toml`:

```toml
[daemon]
mode = "auto"           # off | on | auto
idle_timeout = 900      # seconds with no client before it exits; 0 = never
upload_threads = 4      # background S3 uploaders
# socket = "/run/user/1000/vcache.sock"   # default: derived from cache.disk.dir
```

| Mode | Behaviour |
| --- | --- |
| `off` (default) | Every compile opens its own cache layers, as before. |
| `on` | Use a daemon if one is running; otherwise run in-process. Something else starts it: a CI step, systemd, launchd. |
| `auto` | As `on`, but the first compile that finds none starts one. |

| TOML key | Environment | Default |
| --- | --- | --- |
| `daemon.mode` | `VCACHE_DAEMON` | `off` |
| `daemon.idle_timeout` | `VCACHE_DAEMON_IDLE_TIMEOUT` | `900` |
| `daemon.upload_threads` | `VCACHE_DAEMON_UPLOAD_THREADS` | `4` |
| `daemon.socket` | `VCACHE_DAEMON_SOCKET` | `<cache dir>/daemon/sock` |
| `daemon.single_flight` | `VCACHE_DAEMON_SINGLE_FLIGHT` | `false` |
| `daemon.admission` | `VCACHE_DAEMON_ADMISSION` | `false` |
| `daemon.jobserver` | `VCACHE_DAEMON_JOBSERVER` | off |
| `daemon.jobserver_jobs` | `VCACHE_DAEMON_JOBSERVER_JOBS` | online CPUs |

## Commands

| Command | Effect |
| --- | --- |
| `vcache --start-daemon` | Start one in the background and wait until it listens. A no-op if one is already running. |
| `vcache --stop-daemon` | Wait for pending uploads to finish, then stop it. Prints how many uploaded, failed and were skipped. |
| `vcache --daemon-status` | Pid, socket, lookups by layer, stores, the upload queue. |
| `vcache --daemon-foreground` | Run in the foreground, for a service manager or a debugger. |
| `vcache --jobserver-env` | Print `MAKEFLAGS=-j --jobserver-auth=fifo:<path>` for the running pool, or exit 1. |

`--show-stats` also says whether a daemon is running.

A CI job that wants every upload to have landed before the job ends finishes
with:

```sh
vcache --stop-daemon
```

With `--error-on-cache-media-failure` (or `VCACHE_ERROR_ON_CACHE_MEDIA_FAILURE=1`)
it exits **90** if any upload failed, the same status a compile uses for a
broken cache layer.

## What changes with a daemon

**Stores return before the upload.** A store writes the disk layer and replies;
the S3 upload runs on a background worker. The compile that produced the entry
is not held up by it, and neither is the next job make was waiting to start.

**Connections are kept.** Each upload worker and each lookup slot keeps one
libcurl handle, so consecutive requests reuse the TCP connection and TLS
session. libcurl is loaded once, by the daemon. A compile using a daemon never
loads it.

**Uploads survive the daemon.** An upload queued for an entry that is on disk is
journalled as an empty file under `<cache dir>/daemon/pending/`. A daemon that
is killed or crashes leaves its queue there, and the next one uploads it.

**One process owns the cache.** Every store and backfill goes through it. That
makes it the obvious place to hang future work that needs a single view of the
cache, distributed compilation above all, without changing the wire format
compiles already speak.

## What does not change

**Entries.** The disk layer has the same layout, and blobs are the same bytes
under the same keys. A cache written with the daemon reads without it and the
other way round. Switching the mode needs no `--clear`.

**Statistics.** Compiles still count their own hits and misses in the stats
file, so `--show-stats` means what it always did. The daemon keeps separate
counters, which `--daemon-status` shows.

**Failure policy.** A daemon problem never breaks a build. Each case falls back
to in-process layers for that compile:

- no daemon is running in `on` mode
- the daemon refuses the client (see below)
- the daemon dies or stops answering partway through a compile
- `auto` mode fails to start one

A failed `auto` start is not retried for 60 seconds, so a cache directory the
daemon cannot use does not cost every job a fork and an exec.

## Safety

**Wrong cache.** A connection opens with a handshake that carries the
protocol version and every setting that decides *where entries live*:
- disk directory and size
- read-only
- S3 bucket, region, prefix, endpoint, path style and TTL
- a digest of the S3 access key id

The daemon refuses a client that differs in any of them and logs which setting
differed. The client then runs in-process. A build with a different
`VCACHE_DIR` or bucket is therefore never served from someone else's cache.
The access key itself never crosses the socket.

**Other users.** The socket sits in a `0700` directory. The daemon also checks
each connection's peer uid (`SO_PEERCRED` on Linux, `getpeereid` on macOS) and
closes any that is not its own. Keys must be plain hex digests before they are
used as file names.

**Inherited descriptors.** A daemon started from inside a build would otherwise
inherit that build's pipes, such as make's jobserver or the stdout a CI runner
waits to see closed, and the build would appear never to finish. It detaches
with `setsid` and a double fork, points stdio at `/dev/null` and closes
everything else before it execs.

**Credentials.** The daemon uses the AWS credentials it was started with. If
they are short-lived session credentials, it fails uploads once they expire.
Restart it, or let `idle_timeout` retire it, when credentials rotate. A client
with a *different* access key id is refused rather than served with the
daemon's identity.

## Jobserver mode

With `daemon.jobserver` on, the daemon keeps one GNU-make fifo of job slots
for every build on the machine. A build script exports the line from
`vcache --jobserver-env`:

```text
MAKEFLAGS=-j --jobserver-auth=fifo:<cache dir>/daemon/jobserver.fifo
```

The bare `-j` is what tells make it is a client of that fifo rather than the
owner of a new pool. The fifo holds `daemon.jobserver_jobs - 1` '+' bytes
(online CPUs when the count is unset). Each top-level jobserver client
(make, ninja, cargo) holds one implicit slot, so the pool caps concurrent
jobs at N-1 shared + 1 per concurrent top-level build (measured with N=2:
one make 2, two makes 3, three makes 4). make and ninja count that implicit
slot as already taken — the slot a parent make would have spent to launch
them — and draw every further job from the fifo, so writing the full count
would let them run one job too many. A job reads one byte before it starts
and writes it back when it finishes. The daemon holds the fifo open
read-write, so a client closing does not look like end-of-file to the
others.

Versions that follow this line: GNU make 4.4.1 and ninja 1.13.2, and only the
fifo form. ninja ignores `--jobserver-auth=R,W` (the pipe form this daemon
does not print) and ignores the fifo when its own command line passes `-j`.
cargo and rustc speak the same protocol through the `jobserver` crate; that
is checked separately before a sibling repo relies on it.

A build that still has the `MAKEFLAGS` line after the daemon has gone falls
back to the tool's own default. ninja warns and uses its usual `-j`. A client
that writes back more tokens than it took grows the pool, which is the same
property make's own fifo has. A client that dies holding a token loses it
until the daemon restarts.

`--daemon-status` shows `jobserver tokens total`, `free` and `withdrawn`.
Withdrawn counts the shared tokens held by the daemon under memory pressure.
The daemon does not treat a build blocked on the fifo as a connected client,
so while a build holds any token it waits through the idle timeout twice before
exiting, and the idle log line names that doubled wait. An explicit 0, a
negative `jobserver_jobs`, or a value above the platform integer maximum
warns and uses the online-CPU default. If the fifo cannot be created the
daemon still runs, and `--jobserver-env` exits 1. It also exits 1 when the
status still names a pool but the path is no longer a fifo.

### Elastic jobserver pool

Every 500 ms the daemon uses the admission queue to adjust the shared fifo.
For total jobs `N`, free shared bytes `F`, daemon-held tokens `W`, and waiting
reservations `Q`, pressure withdraws `min(F, Q, N-W-min_jobs)` tokens.
`daemon.jobserver_min_jobs` defaults to 2 and is clamped to `N` with a warning
when larger. Tokens held by running jobs are never taken. Reads are nonblocking,
so a client taking the last byte first makes the daemon retry on a later tick.

When `Q` is zero and available memory exceeds both default memory estimates,
the daemon returns one token per tick. Shutdown returns all its held tokens
before closing and removing the fifo. The daemon log includes
`jobserver: withdrew ...` and `jobserver: restored ...`; each is mirrored once
to `VCACHE_LOG` with the `daemon: ` prefix. Status adds the live
`jobserver tokens withdrawn` count and lifetime `jobserver tokens restored total`.
The existing free row still counts fifo bytes plus one implicit slot.
A daemon that crashes or is killed loses its withdrawn tokens for builds still
using the old fifo; only builds started after restart receive the full new pool.

Each top-level make, ninja or cargo build owns an implicit slot. With `k`
concurrent builds, the shared pool permits approximately `N-1-W+k` jobs;
withdrawals cannot take those implicit slots. Memory admission bounds admitted
memory, while the pool controls CPU concurrency approximately. A pool with no
admission requests remains fixed, including clients with admission off.
Tokens withdrawn by the daemon do not count as active clients for its idle
timeout; tokens held by builds retain the existing doubled timeout.

## Files

Everything lives under `<cache dir>/daemon/`, which the disk layer never walks:

| Path | Purpose |
| --- | --- |
| `lock` | `flock`ed for the daemon's lifetime; decides which of two simultaneous starts wins |
| `pid` | the running daemon's pid |
| `sock` | the Unix socket |
| `log` | lifecycle, refusals and upload failures; rotated at 4 MiB |
| `jobserver.fifo` | the job-slot fifo, present only while `daemon.jobserver` is on |
| `pending/<key>` | the upload journal |
| `start-failed` | the reason the last `auto` start failed, which also gates the retry |

`sockaddr_un` holds 104 bytes on macOS and 108 on Linux. When
`<cache dir>/daemon/sock` would not fit, the socket moves to
`/tmp/vcache-<uid>/<digest of the cache dir>.sock`.

## Running it under a service manager

```ini
# ~/.config/systemd/user/vcache.service
[Service]
ExecStart=/usr/local/bin/vcache --daemon-foreground
Environment=VCACHE_DAEMON_IDLE_TIMEOUT=0
EnvironmentFile=%h/.config/vcache/env   # VCACHE_DIR, S3 settings, credentials
```

Set clients to `VCACHE_DAEMON=on`. They must see the same cache settings as the
service, or they are refused.

## Measurements

256 small C++ translation units, `make -j16`, on a 16-core Linux host. The S3
layer was the bundled mock (`tests/mock_s3.py`), with simulated per-request
latency and per-connection setup cost to stand in for a remote bucket. Wall
time in seconds, best of three; the other two runs were within 0.3 s.

| Simulated S3 | Build | Daemon off | Daemon on |
| --- | --- | --- | --- |
| 5 ms/request, 10 ms/connection | cold (every compile misses and stores) | 9.07 | 8.88 |
| | warm, served from disk | 1.20 | 1.10 |
| | warm, served from S3 (disk emptied) | 1.36 | 1.18 |
| 20 ms/request, 60 ms/connection | cold | 10.63 | 9.73 |
| | warm, served from disk | 1.20 | 1.11 |
| | warm, served from S3 | 2.33 | 1.41 |

The same tree with no cache took 7.3 s.

- **Cold.** By the end of the build, every upload had already completed in the
  background. `--stop-daemon` took 0.02 s and had nothing left to drain.
- **Served from S3.** This gains the most, because kept connections remove the
  per-compile connection setup. The gain grows with the distance to the bucket.
- **Served from disk.** The saving is the in-process S3 setup that a compile
  using the daemon no longer does: loading libcurl and constructing the layer.

These figures come from the mock, not from AWS. A real bucket's TLS handshake
costs more than the mock's simulated one, which should favour the daemon
further, but that has not been measured here.

The benchmark harness exposes two mock settings, `MOCK_S3_LATENCY_MS` and
`MOCK_S3_HANDSHAKE_MS`. Either one turns on HTTP/1.1 keep-alive and
`TCP_NODELAY`. Without `TCP_NODELAY`, Python's server writes headers and body
separately, and a client that reuses its connection waits on the delayed-ACK
timer. Real endpoints do not do this. Measured without it, the daemon looked
*slower* on S3 hits (1.48 s against 1.37 s).

## Single-flight

Set `daemon.single_flight = true` (`VCACHE_DAEMON_SINGLE_FLIGHT=1`) to share
one compile of a missed key across concurrent worktrees. It defaults to false.
The switches control each client's requests. The daemon always serves scheduler
operations, so a tree can enable them after another tree has started the daemon.
A compile session holds a memory-only `KeyLease`; another compile of the same
key waits for its holder to store or release it. Different keys remain independent.
C and Rust lease the entry key. Links lease the pre-key that locates their
result manifest. Rust's dep-info and manifest keys never receive compile leases.
Read-only clients hold no compile lease. Recache requests
compile independently so they still replace existing entries.

After a stored reply the waiter uses an ordinary Get and restores the outputs.
A failed holder, a lost session, daemon shutdown, or the wait bound makes it
compile locally. A missing or unusable entry after a stored reply also compiles:
release-stored can precede Put, so that ordering remains safe without guaranteeing
a hit. A granted holder rechecks the cache too, covering a store between its
initial miss and acquire. A restored result counts as a normal cache hit; only
a wrapper that actually compiles records a miss.

If a holder closes while a Put of its key is already in flight, the lease stays
held until that Put finishes: success wakes stored, failure wakes compile. A
close with no Put in flight wakes compile immediately. The waiter's own bound
still limits its wait if storage stalls; there is no timer grace on holder close.
Put completes a lease only when its peer pid matches the holder's pid. If either
pid is unavailable, the daemon accepts the Put as it did before pid checks.
The test-only `VCACHE_DAEMON_TEST_BLOCK_PUT=<fifo>` seam pauses the first Put
after marking it in flight, for at most five seconds. A byte `s` permits storage;
any other byte or timeout makes that Put's disk read-only and skips remote stores.
The seam is inactive when the variable is absent.

The wait bound is the larger of twice the waiter's own recorded wall time and
30 seconds, capped at the 300-second reply timeout. Without a usable cost record
the bound is 30 seconds. It uses the highest wall time in the matching cost
record, rather than only its latest observation. A waiter still occupies its
build tool's job slot while it waits.
Time the holder spends queued for memory pauses this lease bound, for up to
600 additional seconds. The client receive timeout includes that headroom
and the existing 15-second reply margin.
Decision logs include holder pid, bound and elapsed wait; the client receives
this metadata with its scheduling reply. Session loss never fails the build.
The socket receive timeout exceeds a scheduling bound by 15 seconds, allowing
the bound reply to arrive. Disconnect checks run outside the shared server lock
at most every 250 ms; holder completion wakes the lease condition immediately.

`--daemon-status` shows `leases held`, `leases waiting`, lifetime `leases expired`
and `compiles deduplicated`. A stored wake increments the deduplication counter;
the subsequent Get can still miss after eviction. Restart drops all leases and
waiters continue locally. Concurrent S3 Gets also share one fetch through the
key table, without holding a compile lease or affecting compile deduplication
counters. Fetch waits are bounded by the same 300-second reply timeout and log
their elapsed duration.

Measured cross-worktree deduplication: pending the scheduler benchmark.

## Memory admission

`daemon.admission` defaults to false and enables admission for that client.
The daemon serves scheduling requests regardless of the starter's switches.
After a cache miss, and after acquiring a lease when single-flight is enabled,
the wrapper reserves memory before its actual compile or link. Preprocessing
and Rust dep-info runs do not reserve memory. A lease waiter holds no reservation.

The estimate is the highest RSS in the matching cost record. Missing or zero
records use `daemon.default_compile_kb` (2097152 kB, 2 GiB) or
`daemon.default_link_kb` (4194304 kB, 4 GiB). Every 500 ms the daemon reads
`MemAvailable`, then sums `VmRSS` over each compiler's live process tree,
including children and grandchildren. A spawn samples only the new tree
with a fresh memory reading. Process reads run outside the shared server lock.

```
unrealised_kb = sum(max(0, estimate_kb - realised_kb))
available_kb = max(0, MemAvailable - unrealised_kb)
```

Resident memory is already reflected in `MemAvailable`, so it is subtracted
from each reservation's unrealised amount. Grants follow strict FIFO order;
a small request cannot pass a larger head request. An estimate above available
RAM can start when no reservation is held, allowing at least one job to proceed.
Session close or confirmed compiler exit releases the reservation immediately.
A failed proc read alone retains it. RSS above the estimate contributes zero
unrealised memory and logs the overshoot. Release logs the tree's peak RSS;
steady samples do not write per-tick lines. If `MemAvailable` is unreadable,
the daemon logs once and retains its last good value, deferring new sampled RSS
realisation until a fresh memory reading succeeds. With no good reading yet,
admission is unavailable and requests proceed without waiting for memory.

The reported compiler pid is sampled only when visible with the session peer
as its parent. Otherwise the daemon logs the pid mismatch and treats the whole
estimate as unrealised for ten seconds, then as realised. This fallback allows
clients in another pid namespace to proceed without sampling an unrelated pid.

Waiting is bounded to ten minutes by default. A bound reply, refusal, disconnect
or daemon shutdown runs the compiler unreserved. Decision logs contain the
estimate, elapsed wait and admission arithmetic. `--daemon-status` adds
`memory reserved`, `memory realised`, `memory waiting`, `reserve waits` and
`longest wait ms`. The first two values are in kB. Reservations and waiters are
memory-only and disappear on restart.

## Protocol

Every message is one frame: an 8-byte little-endian length, then the body. A
request body starts with an op byte and a reply body with a status byte. Fields
are 8-byte integers and length-prefixed strings, in a fixed order per op
(`src/daemon/protocol.h`):

| Op | Request | Reply |
| --- | --- | --- |
| hello | version, fingerprint | ok + daemon pid, or refused + reason |
| get | key | ok + layer + errors + blob, or miss + errors |
| put | key, blob | ok + stored flag + errors |
| status | — | ok + text |
| shutdown | — | ok + failed-upload count + summary, once uploads have drained |
| session open | —, immediately after hello | ok, holding this connection until compile completion |
| lease acquire | key, bound ms | ok + outcome + compile reason + holder pid + waited ms |
| lease release | key, stored or failed | ok |
| memory reserve | cost key, estimate kB, bound ms | ok + granted or bound + waited ms |
| compiler spawned | compiler pid | ok |
| any session op, or idle session | — | terminal error + "daemon shutting down" |

Any session frame may be a terminal `error + "daemon shutting down"`. Shutdown
sends this frame to idle sessions too, then closes their sockets. A terminal
frame replaces a pending scheduling reply; clients continue locally.

Lease outcomes are one-byte `granted`, `stored`, or `compile`. Compile reasons
are one-byte `none`, `holder_failed`, `holder_gone`, `bound`, or `shutdown`.
Holder pid (zero if unknown) and waited milliseconds are unsigned 64-bit integers.
Memory outcomes are one-byte `granted` (0) or `bound` (1), followed by unsigned
64-bit waited milliseconds. Compiler pid, estimate and bound use unsigned
64-bit integers too.

Get and Put use one connection per request. Protocol version 2 also supports
**compile sessions**: one separate connection per cache miss when
`daemon.single_flight` or `daemon.admission` is enabled. These switches default
to false; this session scaffolding carries the scheduling operations added by
those features. Read-only clients and daemon-off invocations hold no session.

A session opens after key computation and lookup, before the real compile or
link, and closes after output restoration and stores, or on failure. It has no
idle timeout: a compiler can legitimately run for an hour. An open session keeps
the daemon alive, and `--daemon-status` reports `compile sessions N`.
Shutdown sends a terminal error and closes sessions immediately, while ordinary
requests and uploads retain their existing drain behavior. A missing, refused,
or interrupted session logs `session: daemon unavailable (...)`, and the compile
continues. Successful sessions log their opening and closing duration.

The daemon allows one session per peer pid on Linux. Sessions and ordinary
connections share the 1024-connection limit; a `-j128` build can hold up to 128
sessions alongside its cache requests. A client that never sends another message
keeps one server thread until its socket closes. Sessions are memory-only and
are dropped on restart.

A new op, distributed compilation for example, is a version bump. Clients and
daemons that disagree on the version refuse each other at hello and fall back
cleanly. Version 1 and version 2 refuse each other with both versions in the reason.

## Not done yet

- **Refreshing credentials.** See above.
- **Deciding eviction centrally.** The disk layer still evicts the way it did
  without a daemon (see `design.md`). It works unchanged with or without one,
  but a daemon could keep the cache's size in memory instead of walking the
  tree.
- **Distributed compilation.** The daemon is where it would go, but nothing
  here schedules remote work yet.
