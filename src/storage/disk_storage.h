// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// Local filesystem cache.
//
// Entries live at <dir>/<xx>/<rest-of-key>, sharded on the first byte of the
// key into 256 directories.
//
// Eviction is global: a few large objects that happen to hash into one shard
// must not displace that shard's hot entries while most of the configured
// cache still sits empty.  Deciding globally means walking the whole tree,
// which is far too expensive to do on every store, so a store pays for it only
// when it moves its own shard across a multiple of the shard's even share of
// the budget -- cheap to detect from the one shard already being walked, and
// an event rather than a state, so a permanently oversized shard does not
// re-trigger it on every store.  See Put() for the measurements behind that.
#pragma once

#include <cstdint>
#include <string>

#include "storage/storage.h"

namespace vcache::storage {

class DiskStorage : public Storage {
 public:
  DiskStorage(std::string dir, uint64_t max_size, bool read_only);

  std::string Name() const override { return "disk"; }
  bool Get(const std::string& key, std::string* value) override;
  bool Put(const std::string& key, const std::string& value) override;
  bool writable() const override { return !read_only_; }
  void Trim() override;

  // Deletes every cache entry. Used by `vcache --clear`.
  bool Clear();

  // Sums the on-disk size of all entries. Walks the whole tree, so it is only
  // called by `vcache --show-stats`.
  uint64_t TotalSize() const;

  const std::string& dir() const { return dir_; }

 private:
  std::string PathForKey(const std::string& key) const;
  std::string ShardDir(const std::string& key) const;

  // If the cache holds more than `high_water` bytes, evicts globally
  // least-recently-used entries until it is back under `target_bytes`.  One
  // walk answers both, since walking is the expensive part.
  void TrimGlobal(uint64_t high_water, uint64_t target_bytes);

  std::string dir_;
  uint64_t max_size_;
  bool read_only_;
};

}  // namespace vcache::storage
