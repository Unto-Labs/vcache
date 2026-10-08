// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// The compile's side of the daemon connection.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "core/config.h"
#include "storage/chain.h"

namespace vcache::daemon {

class CompileSessionHandle {
 public:
  ~CompileSessionHandle();
  CompileSessionHandle(const CompileSessionHandle&) = delete;
  CompileSessionHandle& operator=(const CompileSessionHandle&) = delete;

 private:
  friend class DaemonClient;
  explicit CompileSessionHandle(int fd) : fd_(fd) {}

  int fd_;
  const std::chrono::steady_clock::time_point opened_at_ = std::chrono::steady_clock::now();
};

class DaemonClient : public storage::RemoteCache {
 public:
  ~DaemonClient() override;

  // Connects and completes the hello. Returns null when there is no daemon, or
  // when there is one that refuses this client, with the reason in `why`.
  static std::unique_ptr<DaemonClient> Connect(const core::Config& config,
                                               std::string* why);
  static std::unique_ptr<CompileSessionHandle> OpenCompileSession(const core::Config& config);

  std::string Name() const override { return "daemon"; }
  bool Get(const std::string& key, storage::GetResult* result) override;
  bool Put(const std::string& key, const std::string& value,
           storage::PutResult* result) override;

  bool Status(std::string* text);

  // Asks the daemon to drain its uploads and exit, and waits until it has.
  // `failed_uploads` is the daemon's lifetime count of uploads it gave up on.
  bool Shutdown(std::string* summary, uint64_t* failed_uploads);

  uint64_t daemon_pid() const { return daemon_pid_; }

 private:
  DaemonClient() = default;

  // Cache requests use separate connections so waiting on a compile session
  // never prevents a lookup or store.
  bool RoundTrip(const std::string& request, std::string* reply,
                 int timeout_seconds);
  bool Open(std::string* why);
  void Close();

  core::Config config_;
  std::string socket_path_;
  std::string fingerprint_;
  int fd_ = -1;
  uint64_t daemon_pid_ = 0;
};

// What a compile does at startup in "on" and "auto" modes: connect to the
// daemon, starting one first in "auto" mode when none answers. Returns null
// when the compile should use in-process layers instead; the reason is logged.
std::unique_ptr<DaemonClient> ConnectForCompile(const core::Config& config);

}  // namespace vcache::daemon
