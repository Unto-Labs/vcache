// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// The cache daemon.
//
// One long-lived process per cache directory owns that cache's storage layers
// and serves lookups and stores to compiles over a Unix socket. A compile still
// does everything that needs its own environment -- argument parsing, root
// mapping, preprocessing, hashing, running the compiler -- and hands the
// daemon only finished keys and blobs. What moves into the daemon is what a
// per-compile process can never amortise:
//
//   * S3 connections. Each upload worker and each lookup slot keeps one libcurl
//     handle, so the TCP connection and TLS session survive between requests
//     instead of being rebuilt on every compile. libcurl is also loaded once,
//     not once per compile.
//   * S3 uploads. A store writes the disk layer and returns; the upload runs on
//     a background worker. The compile no longer waits on a PUT.
//   * Authority over the cache. Every store and every backfill goes through one
//     process, which is the place a future distributed-compilation scheduler
//     would hang off.
//
// Uploads queued for an entry already on disk are journalled as empty marker
// files under <state>/pending/, so a daemon that is killed or crashes uploads
// them after it restarts rather than losing them.
#pragma once

#include "core/config.h"

namespace vcache::daemon {

// Exit codes from RunServer and the --start-daemon path.
constexpr int kServerOk = 0;
constexpr int kServerFailed = 1;
constexpr int kServerAlreadyRunning = 3;

// Runs the daemon in the calling process until it is stopped, idles out, or
// receives SIGTERM/SIGINT. When `ready_fd` is >= 0, a single status byte is
// written to it once the socket is listening ('R'), or when startup fails
// ('L' another daemon holds the lock, 'E' anything else), and it is closed.
int RunServer(const core::Config& config, int ready_fd);

// Forks a detached daemon and waits until it is listening. Returns kServerOk
// once it is, kServerAlreadyRunning if another daemon for this cache answered
// instead, and kServerFailed otherwise with the reason in `error`.
int StartDetached(const core::Config& config, std::string* error);

}  // namespace vcache::daemon
