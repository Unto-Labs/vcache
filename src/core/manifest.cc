// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "core/manifest.h"

#include "hash/hasher.h"

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

bool ParseManifestFile(std::string_view line, ManifestFile* file) {
  const size_t space = line.find(' ');
  if (space != hash::kDigestHexLen) return false;
  file->first = std::string(line.substr(space + 1));
  file->second = std::string(line.substr(0, space));
  return true;
}

}  // namespace vcache::core
