// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// The daemon's upload queue.
//
// A second store of a key that is already queued or in flight must not be
// dropped: manifests are rewritten under one key. The generation is the
// count of stores. Finishing an upload whose generation is no longer current
// queues the newer value instead of forgetting it.
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace vcache::daemon {

struct UploadItem {
  std::string key;
  std::shared_ptr<const std::string> blob;  // null: read it from disk
  uint64_t generation = 0;
  int attempts = 0;
  std::chrono::steady_clock::time_point not_before{};
};

class UploadQueue {
 public:
  static constexpr uint64_t kDefaultMaxHeldBytes = 1ull << 30;

  explicit UploadQueue(std::string journal_dir, uint64_t max_held_bytes = kDefaultMaxHeldBytes);

  // False only when a held blob would exceed the cap. A refusal does not
  // change the generation. `journal` creates the pending-upload marker.
  bool Enqueue(const std::string& key, std::shared_ptr<const std::string> blob, bool journal);

  // The next item whose retry time has passed, or nullopt. Sets `soonest`
  // to the earliest retry when nothing is ready yet.
  std::optional<UploadItem> TakeReady(std::chrono::steady_clock::time_point now,
                                      std::chrono::steady_clock::time_point* soonest);

  // True when the key was rewritten while this upload ran, and that newer
  // value is now at the back of the queue. The journal stays until the
  // generation that finished is the current one.
  bool Finish(const UploadItem& item);

  // A failed attempt that will be retried. Picks up a newer generation so
  // the retry carries the latest value.
  void Requeue(UploadItem item);

  bool empty() const { return queue_.empty(); }
  size_t size() const { return queue_.size(); }
  uint64_t held_bytes() const { return held_bytes_; }
  uint64_t generation(const std::string& key) const;
  std::shared_ptr<const std::string> Held(const std::string& key) const;
  bool JournalExists(const std::string& key) const;

 private:
  void CreateJournal(const std::string& key) const;
  void RemoveJournal(const std::string& key) const;
  std::string JournalPath(const std::string& key) const;

  std::string journal_dir_;
  uint64_t max_held_bytes_;
  std::deque<UploadItem> queue_;
  std::map<std::string, uint64_t> generation_;
  std::set<std::string> in_flight_;
  std::map<std::string, std::shared_ptr<const std::string>> held_;
  uint64_t held_bytes_ = 0;
};

}  // namespace vcache::daemon
