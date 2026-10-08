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
#include <vector>

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
  // An accepted store clears that mark and overtakes a refusal that is still
  // waiting on the older upload: that refusal must not upload its value.
  // `merged` is set only when the key is waiting, not when it is in flight.
  // `in_flight_state` reports that, including when the store is refused.
  // `refusal_id` is set when an in-flight upload was superseded. `journal`
  // creates the pending-upload marker. `dropped_waiting` is set when a queued
  // value was dropped because this store was refused.
  bool Enqueue(const std::string& key, std::shared_ptr<const std::string> blob, bool journal,
               bool* merged = nullptr, bool* in_flight_state = nullptr,
               uint64_t* refusal_id = nullptr, bool* dropped_waiting = nullptr);

  // True while the same flight recorded for `refusal_id` is still in flight.
  // A later flight of the key, even one whose generation restarted at 1, does
  // not count.
  bool RefusalStillInFlight(uint64_t refusal_id) const;
  bool RefusalOvertaken(uint64_t refusal_id) const;
  // Reports whether the refusal was overtaken and drops its record.
  bool TakeRefusal(uint64_t refusal_id);
  size_t refusal_count() const { return refusals_.size(); }

  // The caller uploads this key itself. A later store then waits or is
  // re-queued, the same as for a worker upload. The blob is not pinned.
  // Returns 0 when the key is already in flight.
  uint64_t BeginSync(const std::string& key);

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
  // the retry carries the latest value. True when the attempt was superseded
  // and dropped instead of retried.
  bool Requeue(UploadItem item);

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
  // The blob an in-flight upload already copied. It stays in the byte count
  // after a newer store drops it from held_, until that upload finishes.
  struct FlightPin {
    std::shared_ptr<const std::string> blob;
    bool counted_outside_held = false;
  };

  // One refused re-put waiting on one flight. `flight_id` is never reused.
  struct FlightRefusal {
    std::string key;
    uint64_t flight_id = 0;
    bool overtaken = false;
  };

  void CreateJournal(const std::string& key) const;
  void RemoveJournal(const std::string& key) const;
  std::string JournalPath(const std::string& key) const;
  void DropHeld(const std::string& key);
  void ReleaseFlightPin(const std::string& key);
  void ForgetQueued(const std::string& key);
  uint64_t SupersedeInFlight(const std::string& key);
  void OvertakeRefusals(const std::string& key);
  void MarkInFlight(const std::string& key);
  const FlightRefusal* FindRefusal(uint64_t refusal_id) const;

  std::string journal_dir_;
  uint64_t max_held_bytes_;
  std::deque<std::string> order_;
  std::map<std::string, UploadItem> waiting_;
  std::map<std::string, uint64_t> generation_;
  std::set<std::string> in_flight_;
  std::map<std::string, uint64_t> in_flight_id_;
  std::set<std::string> superseded_;
  std::set<std::string> recount_on_requeue_;
  std::map<uint64_t, FlightRefusal> refusals_;
  uint64_t next_refusal_id_ = 1;
  uint64_t next_flight_id_ = 1;
  std::map<std::string, FlightPin> flight_pin_;
  std::map<std::string, std::shared_ptr<const std::string>> held_;
  uint64_t held_bytes_ = 0;
};

}  // namespace vcache::daemon
