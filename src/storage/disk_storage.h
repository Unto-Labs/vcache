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
#include <optional>
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

  // True when the daemon's upload journal still names the entry at this path.
  bool IsPendingUpload(const std::string& entry_path) const;

  // Content-addressed files beside the entries, for outputs too large to carry
  // inside one: <dir>/<digest[0:2]>/<digest>.<suffix>. They live in the shards
  // so that Trim, Clear and TotalSize count and evict them like entries, by
  // their own mtime. They are not fsynced: a lost one fails its hit's hash.
  enum class PutFileStatus { kStored, kReadOnly, kSourceMissing, kFailed };

  // Clones `source_path` into the store under `digest`. An existing file is
  // cloned over, which replaces one a crash tore. kFailed sets last_error().
  PutFileStatus PutFile(const std::string& digest, const std::string& suffix,
                        const std::string& source_path);

  // The stored file's path, or nullopt when there is none. Its mtime is
  // refreshed first, so a trim that starts while the caller hashes it sees a
  // fresh file.
  std::optional<std::string> GetFile(const std::string& digest, const std::string& suffix);

  // Removes a stored file that failed verification, so failed hits cannot
  // keep a torn file fresh until a store replaces it.
  void RemoveFile(const std::string& digest, const std::string& suffix);

 private:
  std::string PathForKey(const std::string& key) const;
  std::string ShardDir(const std::string& key) const;
  std::string PathForFile(const std::string& digest, const std::string& suffix) const;

  // Runs the eviction check when `added_bytes` moved `shard` across a multiple
  // of its share of the budget. See Put() for why it is an event.
  void CheckShardGrowth(const std::string& shard, uint64_t added_bytes, bool sweep_scratch);

  // If the cache holds more than `high_water` bytes, evicts globally
  // least-recently-used entries until it is back under `target_bytes`.  One
  // walk answers both, since walking is the expensive part.
  void TrimGlobal(uint64_t high_water, uint64_t target_bytes);

  std::string dir_;
  uint64_t max_size_;
  bool read_only_;
};

}  // namespace vcache::storage
