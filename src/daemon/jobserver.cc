// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "daemon/jobserver.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <utility>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

namespace vcache::daemon {
namespace {

bool WriteAll(int fd, const char* data, size_t len) {
  size_t off = 0;
  while (off < len) {
    const ssize_t n = ::write(fd, data + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

std::string JobserverMakeFlagsLine(const std::string& fifo_path) {
  return "MAKEFLAGS=-j --jobserver-auth=fifo:" + fifo_path + "\n";
}

int JobserverTokenChange(int total, int free, int withdrawn, uint64_t waiting,
                         uint64_t available_kb, int min_jobs, uint64_t default_estimate_kb) {
  const int floor = std::clamp(min_jobs, 1, std::max(1, total));
  if (waiting > 0 && free > 0) {
    return -static_cast<int>(std::min<uint64_t>(waiting,
        std::max(0, std::min(free, total - withdrawn - floor))));
  }
  if (waiting == 0 && withdrawn > 0 && available_kb > default_estimate_kb) return 1;
  return 0;
}

JobserverPool::JobserverPool(std::string path, int fd, int total, uint64_t fifo_dev,
                             uint64_t fifo_ino)
    : path_(std::move(path)), fd_(fd), total_(total), fifo_dev_(fifo_dev), fifo_ino_(fifo_ino) {}

JobserverPool::JobserverPool(JobserverPool&& other) noexcept
    : path_(std::move(other.path_)),
      fd_(other.fd_),
      total_(other.total_),
      withdrawn_(other.withdrawn_),
      restored_total_(other.restored_total_),
      fifo_dev_(other.fifo_dev_),
      fifo_ino_(other.fifo_ino_) {
  other.fd_ = -1;
  other.path_.clear();
  other.total_ = 0;
  other.withdrawn_ = 0;
  other.restored_total_ = 0;
  other.fifo_dev_ = 0;
  other.fifo_ino_ = 0;
}

JobserverPool& JobserverPool::operator=(JobserverPool&& other) noexcept {
  if (this != &other) {
    Restore(withdrawn_);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    RemoveOwnedFifo();
    path_ = std::move(other.path_);
    fd_ = other.fd_;
    total_ = other.total_;
    withdrawn_ = other.withdrawn_;
    restored_total_ = other.restored_total_;
    fifo_dev_ = other.fifo_dev_;
    fifo_ino_ = other.fifo_ino_;
    other.fd_ = -1;
    other.path_.clear();
    other.total_ = 0;
    other.withdrawn_ = 0;
    other.restored_total_ = 0;
    other.fifo_dev_ = 0;
    other.fifo_ino_ = 0;
  }
  return *this;
}

void JobserverPool::RemoveOwnedFifo() {
  if (path_.empty()) return;
  struct stat st {};
  if (::lstat(path_.c_str(), &st) == 0 &&
      static_cast<uint64_t>(st.st_dev) == fifo_dev_ &&
      static_cast<uint64_t>(st.st_ino) == fifo_ino_) {
    ::unlink(path_.c_str());
  }
  path_.clear();
}

JobserverPool::~JobserverPool() {
  Restore(withdrawn_);
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  RemoveOwnedFifo();
}

std::optional<JobserverPool> JobserverPool::Open(const std::string& path, int slots,
                                                 std::string* error) {
  auto fail = [&](const std::string& why) -> std::optional<JobserverPool> {
    if (error != nullptr) *error = why;
    return std::nullopt;
  };
  if (slots <= 0) return fail("no slots");
  if (path.size() >= static_cast<size_t>(PATH_MAX)) return fail("path too long: " + path);

  struct stat st {};
  if (::lstat(path.c_str(), &st) == 0) {
    // A crashed daemon leaves its fifo behind. It is ours: it lives in the
    // state directory. Anything else at that path is not a pool we can replace.
    if (!S_ISFIFO(st.st_mode)) return fail(path + " exists and is not a fifo");
    if (::unlink(path.c_str()) != 0) {
      return fail("cannot replace " + path + ": " + std::strerror(errno));
    }
  }

  if (::mkfifo(path.c_str(), 0600) != 0) {
    return fail("mkfifo " + path + ": " + std::strerror(errno));
  }
  ::chmod(path.c_str(), 0600);

  // O_RDWR, not O_WRONLY: FIONREAD needs a read end, and a write-only opener
  // makes the first client to close the last reader, which delivers EOF to
  // every other build still using the pool.
  const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    const int saved = errno;
    ::unlink(path.c_str());
    return fail("open " + path + ": " + std::strerror(saved));
  }

  // slots-1 bytes. A one-slot pool leaves the fifo empty: the client's
  // implicit slot is the only job, and there is no byte to write.
  const int fifo_tokens = slots - 1;
  if (fifo_tokens > 0) {
    const std::string bytes(static_cast<size_t>(fifo_tokens), '+');
    if (!WriteAll(fd, bytes.data(), bytes.size())) {
      const int saved = errno;
      ::close(fd);
      ::unlink(path.c_str());
      return fail("write tokens to " + path + ": " + std::strerror(saved));
    }
  }
  struct stat owned {};
  if (::fstat(fd, &owned) != 0) {
    const int saved = errno;
    ::close(fd);
    ::unlink(path.c_str());
    return fail("fstat " + path + ": " + std::strerror(saved));
  }
  return JobserverPool(path, fd, slots, static_cast<uint64_t>(owned.st_dev),
                       static_cast<uint64_t>(owned.st_ino));
}

int JobserverPool::fifo_bytes() const {
  if (fd_ < 0) return -1;
  int available = 0;
  if (::ioctl(fd_, FIONREAD, &available) != 0) return -1;
  return available;
}

int JobserverPool::free_tokens() const {
  const int bytes = fifo_bytes();
  if (bytes < 0) return -1;
  return bytes + 1;
}

int JobserverPool::Withdraw(int count) {
  int taken = 0;
  std::array<char, 256> tokens{};
  count = std::clamp(count, 0, std::max(0, total_ - 1 - withdrawn_));
  while (fd_ >= 0 && taken < count) {
    const ssize_t size = ::read(fd_, tokens.data(),
        std::min<size_t>(tokens.size(), count - taken));
    if (size < 0 && errno == EINTR) continue;
    if (size <= 0) break;
    taken += static_cast<int>(size);
    withdrawn_ += static_cast<int>(size);
  }
  return taken;
}

int JobserverPool::Restore(int count) {
  int returned = 0;
  const std::string tokens(static_cast<size_t>(std::clamp(count, 0, withdrawn_)), '+');
  while (fd_ >= 0 && returned < static_cast<int>(tokens.size())) {
    const ssize_t size = ::write(fd_, tokens.data() + returned, tokens.size() - returned);
    if (size < 0 && errno == EINTR) continue;
    if (size <= 0) break;
    returned += static_cast<int>(size);
    withdrawn_ -= static_cast<int>(size);
    restored_total_ += static_cast<uint64_t>(size);
  }
  return returned;
}

}  // namespace vcache::daemon
