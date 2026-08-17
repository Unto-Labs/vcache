// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "storage/disk_storage.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

#include "util/fs.h"
#include "util/log.h"
#include "util/str.h"

namespace vcache::storage {
namespace {

constexpr int kShardCount = 256;

// Evict down to this fraction of the budget so a shard that is exactly at the
// limit does not trigger a scan on every single store.
constexpr double kTrimTargetFraction = 0.8;

}  // namespace

DiskStorage::DiskStorage(std::string dir, uint64_t max_size, bool read_only)
    : dir_(std::move(dir)), max_size_(max_size), read_only_(read_only) {}

std::string DiskStorage::ShardDir(const std::string& key) const {
  // Keys are hex digests, so the first two characters are already uniform.
  return dir_ + "/" + key.substr(0, 2);
}

std::string DiskStorage::PathForKey(const std::string& key) const {
  return ShardDir(key) + "/" + key.substr(2);
}

bool DiskStorage::Get(const std::string& key, std::string* value) {
  ClearError();
  if (key.size() < 3) return false;
  const std::string path = PathForKey(key);
  auto data = util::ReadFile(path);
  if (!data) {
    // An absent entry is an ordinary miss. Anything else -- permissions, a
    // truncated read, a filesystem in trouble -- means this layer is broken
    // and the caller may want to hear about it.
    if (errno != ENOENT) {
      SetError("read " + path + ": " + std::strerror(errno));
    }
    return false;
  }
  *value = std::move(*data);
  // The mtime doubles as the LRU timestamp: relatime makes atime unreliable,
  // so refresh mtime explicitly on every hit.
  ::utimensat(0, path.c_str(), nullptr, 0);
  return true;
}

bool DiskStorage::Put(const std::string& key, const std::string& value) {
  ClearError();
  if (read_only_ || key.size() < 3) return false;
  const std::string shard = ShardDir(key);
  if (!util::MakeDirs(shard)) {
    SetError("create shard directory " + shard + ": " + std::strerror(errno));
    VCACHE_LOG("disk: cannot create shard directory " + shard);
    return false;
  }
  if (!util::WriteFileAtomic(PathForKey(key), value)) {
    SetError("write " + PathForKey(key) + ": " + std::strerror(errno));
    VCACHE_LOG("disk: write failed for " + PathForKey(key));
    return false;
  }

  // Only this shard can have grown, so bound the eviction work to it.
  const uint64_t shard_budget = std::max<uint64_t>(max_size_ / kShardCount, 1);
  uint64_t shard_size = 0;
  for (const auto& entry : util::ListFilesRecursive(shard)) shard_size += entry.size;
  if (shard_size > shard_budget) {
    TrimShard(shard, static_cast<uint64_t>(shard_budget * kTrimTargetFraction));
  }
  return true;
}

void DiskStorage::TrimShard(const std::string& shard_dir, uint64_t target_bytes) {
  auto entries = util::ListFilesRecursive(shard_dir);
  uint64_t total = 0;
  for (const auto& entry : entries) total += entry.size;
  if (total <= target_bytes) return;

  // Oldest first.
  std::sort(entries.begin(), entries.end(),
            [](const util::FileEntry& a, const util::FileEntry& b) {
              return a.lru_time < b.lru_time;
            });

  size_t removed = 0;
  for (const auto& entry : entries) {
    if (total <= target_bytes) break;
    if (util::RemoveFile(entry.path)) {
      total -= entry.size;
      ++removed;
    }
  }
  if (removed > 0) {
    VCACHE_LOG("disk: evicted " + std::to_string(removed) + " entries from " +
               shard_dir);
  }
}

void DiskStorage::Trim() {
  if (read_only_) return;
  const uint64_t shard_budget = std::max<uint64_t>(max_size_ / kShardCount, 1);
  const auto target = static_cast<uint64_t>(shard_budget * kTrimTargetFraction);
  for (int i = 0; i < kShardCount; ++i) {
    char buf[3];
    std::snprintf(buf, sizeof(buf), "%02x", i);
    const std::string shard = dir_ + "/" + buf;
    if (util::IsDirectory(shard)) TrimShard(shard, target);
  }
}

bool DiskStorage::Clear() {
  if (read_only_) return false;
  bool ok = true;
  for (int i = 0; i < kShardCount; ++i) {
    char buf[3];
    std::snprintf(buf, sizeof(buf), "%02x", i);
    const std::string shard = dir_ + "/" + buf;
    if (util::IsDirectory(shard) && !util::RemoveRecursive(shard)) ok = false;
  }
  return ok;
}

uint64_t DiskStorage::TotalSize() const {
  uint64_t total = 0;
  for (int i = 0; i < kShardCount; ++i) {
    char buf[3];
    std::snprintf(buf, sizeof(buf), "%02x", i);
    const std::string shard = dir_ + "/" + buf;
    if (!util::IsDirectory(shard)) continue;
    for (const auto& entry : util::ListFilesRecursive(shard)) total += entry.size;
  }
  return total;
}

}  // namespace vcache::storage
