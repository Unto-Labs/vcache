// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "core/manifest.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>

#include "hash/hasher.h"
#include "util/log.h"

namespace vcache::core {

std::optional<std::string> FindStaleManifestFile(const std::vector<ManifestFile>& files,
                                                 const RootMap& roots) {
  for (const auto& [canonical_path, digest] : files) {
    const std::string local = roots.Localize(canonical_path);
    auto actual = hash::HashFile(local);
    if (!actual) return local + " is gone";
    if (*actual != digest) return local + " changed";
  }
  return std::nullopt;
}

std::string RenderManifestFile(const ManifestFile& file) {
  std::string out = file.second;
  out.push_back(' ');
  out += file.first;
  return out;
}

void PauseBeforeManifestPut() {
  const char* path = std::getenv("VCACHE_TEST_PAUSE_BEFORE_MANIFEST_PUT");
  if (path == nullptr || *path == '\0') return;
  // Anything but a fifo is a stale setting. Waiting on it would block the
  // build, and a missing path has nobody to wake us.
  struct stat st {};
  if (::lstat(path, &st) != 0 || !S_ISFIFO(st.st_mode)) return;

  VCACHE_LOG(std::string("manifest: waiting before put on ") + path);
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return;

  // A fifo left over with no writer must not hold the compile. 60s is long
  // enough for the test that releases it, and short enough to give the build
  // back if the variable was set by accident. Tests shrink the bound.
  int bound_ms = 60 * 1000;
  if (const char* bound = std::getenv("VCACHE_TEST_PAUSE_BOUND_MS")) {
    char* end = nullptr;
    const long parsed = std::strtol(bound, &end, 10);
    if (end != bound && *end == '\0' && parsed > 0 && parsed <= bound_ms) {
      bound_ms = static_cast<int>(parsed);
    }
  }
  const auto started = std::chrono::steady_clock::now();
  pollfd pfd {};
  pfd.fd = fd;
  pfd.events = POLLIN;
  int rc = 0;
  for (;;) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    if (elapsed.count() >= bound_ms) {
      rc = 0;
      break;
    }
    rc = ::poll(&pfd, 1, static_cast<int>(bound_ms - elapsed.count()));
    if (rc < 0 && errno == EINTR) continue;
    break;
  }
  if (rc <= 0) {
    VCACHE_LOG(rc < 0 ? "manifest: test pause failed" : "manifest: test pause timed out");
    ::close(fd);
    return;
  }
  char byte = 0;
  for (;;) {
    const ssize_t n = ::read(fd, &byte, 1);
    if (n < 0 && errno == EINTR) continue;
    break;
  }
  ::close(fd);
}

bool ParseManifestFile(std::string_view line, ManifestFile* file) {
  const size_t space = line.find(' ');
  if (space != hash::kDigestHexLen) return false;
  file->first = std::string(line.substr(space + 1));
  file->second = std::string(line.substr(0, space));
  return true;
}

}  // namespace vcache::core
