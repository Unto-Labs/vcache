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
#include <vector>

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

  // Only this shard can have grown, and walking it is cheap.  What it has to
  // decide is when to pay for the global walk, which is not cheap: measured on
  // a 30k-entry cache with a warm dentry cache, one shard costs ~0.3ms and the
  // whole cache ~95ms.
  //
  // "Is this shard over its even share?" is the wrong question, because it is
  // a state and not an event.  A shard holding a few large objects is over its
  // share permanently, so every store into it would pay the global walk -- and
  // vcache is a fresh process per compile, so there is no in-process guard that
  // could ever fire.  That is the shape this change is meant to serve, so it is
  // exactly the shape that must not be quadratic.
  //
  // Trigger on the crossing instead: pay for the global walk only when this
  // store moves the shard across a multiple of its share.  A shard that is
  // already fat stays quiet until it has grown by another whole share, which
  // bounds the walks to one per `shard_budget` bytes written into a shard
  // rather than one per store.  It degrades sanely too: a workload that
  // somehow concentrated in a single shard keeps crossing multiples and so
  // keeps being checked, instead of falling silent after the first crossing.
  const uint64_t shard_budget = std::max<uint64_t>(max_size_ / kShardCount, 1);
  uint64_t shard_size = 0;
  for (const auto& entry : util::ListFilesRecursive(shard)) shard_size += entry.size;
  // Rewriting an existing key makes this an underestimate and so can trigger a
  // walk that was not due.  That costs time and never correctness, and a
  // rewrite means the same key hashed to the same content, which is rare.
  const uint64_t before =
      shard_size >= value.size() ? shard_size - value.size() : 0;
  if (shard_size / shard_budget != before / shard_budget) {
    TrimGlobal(max_size_, static_cast<uint64_t>(max_size_ * kTrimTargetFraction));
  }
  return true;
}

// The walk is what costs -- ListFilesRecursive() stats every entry -- so the
// over-budget test and the eviction share one.  Asking TotalSize() first and
// then walking again to evict doubles the price of a store, and does so
// precisely in the case that motivated this change: a cache whose shards are
// skewed past their even share but whose global total is still comfortably
// under the limit, where the answer is "no eviction" and the second walk never
// happens at all.
void DiskStorage::TrimGlobal(uint64_t high_water, uint64_t target_bytes) {
  std::vector<util::FileEntry> entries;
  uint64_t total = 0;
  for (int i = 0; i < kShardCount; ++i) {
    char buf[3];
    std::snprintf(buf, sizeof(buf), "%02x", i);
    const std::string shard = dir_ + "/" + buf;
    if (!util::IsDirectory(shard)) continue;
    auto shard_entries = util::ListFilesRecursive(shard);
    for (const auto& entry : shard_entries) total += entry.size;
    entries.insert(entries.end(),
                   std::make_move_iterator(shard_entries.begin()),
                   std::make_move_iterator(shard_entries.end()));
  }
  if (total <= high_water) return;

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
      continue;
    }
    // A parallel build runs many vcache processes against one cache, so an
    // entry can be evicted by another of them between this walk and this
    // unlink.  Those bytes have left the cache, so they have to be counted as
    // freed.  Counting only our own successes makes each concurrent trimmer
    // evict a full budget's worth *itself*, walking further down a list whose
    // head is already gone and taking live entries with it -- so N trimmers
    // hollow the cache to roughly N times the intended depth.  A removal that
    // failed for any other reason (a permission problem, a directory in the
    // way) really has left the bytes in place, and must not be counted.
    if (errno == ENOENT) total -= entry.size;
  }
  if (removed > 0) {
    VCACHE_LOG("disk: globally evicted " + std::to_string(removed) +
               " entries");
  }
}

void DiskStorage::Trim() {
  if (read_only_) return;
  // An explicit trim has no hysteresis to preserve: evict whenever the cache
  // is above the target rather than waiting for it to cross max_size_.
  const auto target = static_cast<uint64_t>(max_size_ * kTrimTargetFraction);
  TrimGlobal(target, target);
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
