// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// Local filesystem cache.
//
// Entries live at <dir>/<xx>/<rest-of-key>, sharded on the first byte of the
// key into 256 directories.  A shard crossing its even share of the global
// budget triggers a global size check.  Eviction itself is global so a few
// large objects that happen to hash into one shard cannot displace hot entries
// while most of the configured cache is still empty.
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

  // Evicts globally least-recently-used entries until the cache is back under
  // `target_bytes`.
  void TrimGlobal(uint64_t target_bytes);

  std::string dir_;
  uint64_t max_size_;
  bool read_only_;
};

}  // namespace vcache::storage
