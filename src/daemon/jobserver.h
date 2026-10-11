// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// A GNU-make fifo of shared job slots.
//
// make, ninja and cargo block in read(2) on this fifo for a '+' byte before
// they start a job, and write the byte back when the job ends. The daemon
// holds the fifo open read-write for its whole life: a fifo with no reader
// delivers EOF to writers, and FIONREAD needs a read end. Clients that close
// do not end the pool.
//
// GNU make's own server, and ninja's, put jobs-1 bytes in the fifo. A client
// started from MAKEFLAGS counts one slot as already taken (the one its parent
// would have used to launch it) and then draws the rest from the fifo, so N
// bytes would let it run N+1 jobs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace vcache::daemon {

// The line `vcache --jobserver-env` prints. The bare `-j` is what tells make
// it is a jobserver client rather than the owner of a new pool.
std::string JobserverMakeFlagsLine(const std::string& fifo_path);

// Negative values withdraw tokens; positive values restore them.
int JobserverSignedTokenChange(int total, int free_fifo_bytes, int withdrawn, uint64_t waiting,
                               uint64_t available_kb, int min_jobs, uint64_t default_estimate_kb);

class JobserverPool {
 public:
  JobserverPool(const JobserverPool&) = delete;
  JobserverPool& operator=(const JobserverPool&) = delete;
  JobserverPool(JobserverPool&& other) noexcept;
  JobserverPool& operator=(JobserverPool&& other) noexcept;
  ~JobserverPool();

  // Creates `path` as a fifo (0600), replacing a stale fifo already there.
  // `slots` is the number of jobs the pool admits. The fifo itself receives
  // slots-1 '+' bytes; see the note above. A non-fifo at the path, a path
  // that cannot be created, or a fifo that cannot hold the tokens leaves
  // nothing behind and returns nullopt.
  static std::optional<JobserverPool> Open(const std::string& path, int slots,
                                           std::string* error);

  const std::string& path() const { return path_; }
  int total() const { return total_; }

  // Jobs that can still start: the bytes currently in the fifo, plus the one
  // implicit client slot. Equals total() when nobody holds a fifo token.
  // Negative when the count cannot be read.
  int free_tokens() const;

  // '+' bytes sitting in the fifo, without the implicit slot.
  int fifo_bytes() const;

  int withdrawn() const { return withdrawn_; }
  uint64_t restored_total() const { return restored_total_; }
  int Withdraw(int count);
  int Restore(int count);

 private:
  JobserverPool(std::string path, int fd, int total, uint64_t fifo_dev, uint64_t fifo_ino);
  // Unlinks path_ only when it is still the fifo this pool opened. A daemon
  // that exits because its socket was replaced must not delete the new
  // daemon's fifo at the same path.
  void RemoveOwnedFifo();

  std::string path_;
  int fd_ = -1;
  int total_ = 0;  // job slots, one more than the bytes written at start
  int withdrawn_ = 0;
  uint64_t restored_total_ = 0;
  uint64_t fifo_dev_ = 0;
  uint64_t fifo_ino_ = 0;
};

}  // namespace vcache::daemon
