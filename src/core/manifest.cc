// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "core/manifest.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
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
  VCACHE_LOG(std::string("manifest: waiting before put on ") + path);
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
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
