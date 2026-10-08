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

  // False when a held blob would exceed the cap. A new key is left untouched.
  // A key already queued is dropped, because the caller uploads the newer
  // value itself. A key in flight is marked superseded and its generation
  // moves, so finishing that upload does not send the old value again.
  // An accepted store clears that mark: the newer value is the one to send.
  // `merged` is set only when the key is waiting, not when it is in flight.
  // `in_flight_state` reports that, including when the store is refused.
  // `journal` creates the pending-upload marker.
  bool Enqueue(const std::string& key, std::shared_ptr<const std::string> blob, bool journal,
               bool* merged = nullptr, bool* in_flight_state = nullptr);

  // The next item whose retry time has passed, or nullopt. Sets `soonest`
  // to the earliest retry when nothing is ready yet.
  std::optional<UploadItem> TakeReady(std::chrono::steady_clock::time_point now,
                                      std::chrono::steady_clock::time_point* soonest);

  // True when the key was rewritten while this upload ran, and that newer
  // value is now at the back of the queue. `count_requeue` is set when that
  // re-queue is another queued upload (an in-flight rewrite, not a store that
  // was already counted while it waited). The journal stays until the
  // generation that finished is the current one.
  bool Finish(const UploadItem& item, bool* count_requeue = nullptr);

  // A failed attempt that will be retried. Picks up a newer generation so
  // the retry carries the latest value.
  void Requeue(UploadItem item);

  bool empty() const { return waiting_.empty(); }
  size_t size() const { return waiting_.size(); }
  uint64_t held_bytes() const { return held_bytes_; }
  uint64_t generation(const std::string& key) const;
  std::shared_ptr<const std::string> Held(const std::string& key) const;
  bool JournalExists(const std::string& key) const;
  bool in_flight(const std::string& key) const { return in_flight_.count(key) != 0; }
  size_t in_flight_count() const { return in_flight_.size(); }
  size_t superseded_count() const { return superseded_.size(); }

 private:
  void CreateJournal(const std::string& key) const;
  void RemoveJournal(const std::string& key) const;
  std::string JournalPath(const std::string& key) const;
  void DropHeld(const std::string& key);
  void ReleaseFlightPin(const std::string& key);
  void ForgetQueued(const std::string& key);
  void SupersedeInFlight(const std::string& key);

  // The blob an in-flight upload already copied. It stays in the byte count
  // after a newer store drops it from held_, until that upload finishes.
  struct FlightPin {
    std::shared_ptr<const std::string> blob;
    bool counted_outside_held = false;
  };

  std::string journal_dir_;
  uint64_t max_held_bytes_;
  std::deque<std::string> order_;
  std::map<std::string, UploadItem> waiting_;
  std::map<std::string, uint64_t> generation_;
  std::set<std::string> in_flight_;
  std::set<std::string> superseded_;
  std::set<std::string> recount_on_requeue_;
  std::map<std::string, FlightPin> flight_pin_;
  std::map<std::string, std::shared_ptr<const std::string>> held_;
  uint64_t held_bytes_ = 0;
};

}  // namespace vcache::daemon
