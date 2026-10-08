# vcache release notes

Each release is also described in its annotated tag (`git tag -n99 vX.Y.Z`).

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
