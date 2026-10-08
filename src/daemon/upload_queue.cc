// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "daemon/upload_queue.h"

#include <fcntl.h>
#include <unistd.h>

#include "util/fs.h"

namespace vcache::daemon {

UploadQueue::UploadQueue(std::string journal_dir, uint64_t max_held_bytes)
    : journal_dir_(std::move(journal_dir)), max_held_bytes_(max_held_bytes) {}

std::string UploadQueue::JournalPath(const std::string& key) const {
  return journal_dir_ + "/" + key;
}

void UploadQueue::CreateJournal(const std::string& key) const {
  if (journal_dir_.empty()) return;
  const int fd = ::open(JournalPath(key).c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
  if (fd >= 0) ::close(fd);
}

void UploadQueue::RemoveJournal(const std::string& key) const {
  if (journal_dir_.empty()) return;
  ::unlink(JournalPath(key).c_str());
}

uint64_t UploadQueue::generation(const std::string& key) const {
  const auto it = generation_.find(key);
  return it == generation_.end() ? 0 : it->second;
}

std::shared_ptr<const std::string> UploadQueue::Held(const std::string& key) const {
  const auto it = held_.find(key);
  return it == held_.end() ? nullptr : it->second;
}

bool UploadQueue::JournalExists(const std::string& key) const {
  return !journal_dir_.empty() && util::FileExists(JournalPath(key));
}

void UploadQueue::DropHeld(const std::string& key) {
  const auto held = held_.find(key);
  if (held == held_.end()) return;
  const auto pin = flight_pin_.find(key);
  if (pin != flight_pin_.end() && pin->second.blob == held->second &&
      !pin->second.counted_outside_held) {
    // The worker still has this blob. Keep it in the byte count.
    pin->second.counted_outside_held = true;
  } else {
    held_bytes_ -= held->second->size();
  }
  held_.erase(held);
}

void UploadQueue::ReleaseFlightPin(const std::string& key) {
  const auto pin = flight_pin_.find(key);
  if (pin == flight_pin_.end()) return;
  if (pin->second.counted_outside_held && pin->second.blob) {
    held_bytes_ -= pin->second.blob->size();
  }
  flight_pin_.erase(pin);
}

void UploadQueue::ForgetQueued(const std::string& key) {
  waiting_.erase(key);
  std::deque<std::string> kept;
  for (const std::string& queued : order_) {
    if (queued != key) kept.push_back(queued);
  }
  order_.swap(kept);
  DropHeld(key);
  generation_.erase(key);
  RemoveJournal(key);
}

void UploadQueue::OvertakeRefusals(const std::string& key) {
  for (auto& entry : refusals_) {
    if (entry.second.key == key) entry.second.overtaken = true;
  }
}

uint64_t UploadQueue::SupersedeInFlight(const std::string& key) {
  // A newer refusal replaces one that is still waiting on this upload.
  OvertakeRefusals(key);
  const auto flight = in_flight_id_.find(key);
  const uint64_t flight_id = flight == in_flight_id_.end() ? 0 : flight->second;
  ++generation_[key];
  superseded_.insert(key);
  DropHeld(key);
  const uint64_t id = next_refusal_id_++;
  refusals_.emplace(id, FlightRefusal{key, flight_id, false});
  return id;
}

const UploadQueue::FlightRefusal* UploadQueue::FindRefusal(uint64_t refusal_id) const {
  const auto it = refusals_.find(refusal_id);
  if (it == refusals_.end()) return nullptr;
  return &it->second;
}

bool UploadQueue::RefusalStillInFlight(uint64_t refusal_id) const {
  const FlightRefusal* refusal = FindRefusal(refusal_id);
  if (refusal == nullptr) return false;
  const auto flight = in_flight_id_.find(refusal->key);
  if (flight == in_flight_id_.end()) return false;
  return flight->second == refusal->flight_id;
}

bool UploadQueue::RefusalOvertaken(uint64_t refusal_id) const {
  const FlightRefusal* refusal = FindRefusal(refusal_id);
  return refusal != nullptr && refusal->overtaken;
}

void UploadQueue::MarkInFlight(const std::string& key) {
  in_flight_.insert(key);
  // generation_ restarts after a superseded finish. The flight id does not,
  // so a refusal recorded against this attempt cannot latch onto the next one.
  in_flight_id_[key] = next_flight_id_++;
}

uint64_t UploadQueue::BeginSync(const std::string& key) {
  if (in_flight_.count(key) != 0) return 0;
  uint64_t& gen = generation_[key];
  ++gen;
  MarkInFlight(key);
  return gen;
}

bool UploadQueue::TakeRefusal(uint64_t refusal_id) {
  const auto it = refusals_.find(refusal_id);
  if (it == refusals_.end()) return false;
  const bool overtaken = it->second.overtaken;
  refusals_.erase(it);
  return overtaken;
}

bool UploadQueue::Enqueue(const std::string& key, std::shared_ptr<const std::string> blob,
                          bool journal, bool* merged, bool* in_flight_state,
                          uint64_t* refusal_id, bool* dropped_waiting) {
  if (merged != nullptr) *merged = false;
  if (dropped_waiting != nullptr) *dropped_waiting = false;
  const bool flying = in_flight_.count(key) != 0;
  const bool waiting = waiting_.find(key) != waiting_.end();
  if (in_flight_state != nullptr) *in_flight_state = flying;
  std::shared_ptr<const std::string> previous;
  const auto held = held_.find(key);
  if (held != held_.end()) previous = held->second;

  if (blob != nullptr) {
    const uint64_t old_bytes = previous ? previous->size() : 0;
    bool old_stays = false;
    if (previous && flying) {
      const auto pin = flight_pin_.find(key);
      if (pin != flight_pin_.end() && pin->second.blob == previous &&
          !pin->second.counted_outside_held) {
        old_stays = true;
      }
    }
    const uint64_t next = held_bytes_ - (old_stays ? 0 : old_bytes) + blob->size();
    if (next > max_held_bytes_) {
      if (flying) {
        const uint64_t id = SupersedeInFlight(key);
        if (refusal_id != nullptr) *refusal_id = id;
      } else if (waiting || previous) {
        ForgetQueued(key);
        if (dropped_waiting != nullptr) *dropped_waiting = waiting;
      }
      return false;
    }
  }

  // A refusal may have marked this key superseded. This store is the one to
  // upload, so finishing the older in-flight attempt must not drop it. It
  // also overtakes a refusal that was waiting to upload an older value.
  OvertakeRefusals(key);
  superseded_.erase(key);
  if (merged != nullptr) *merged = waiting;
  if (flying) recount_on_requeue_.insert(key);
  uint64_t& gen = generation_[key];
  ++gen;
  if (blob != nullptr) {
    if (previous) DropHeld(key);
    held_[key] = blob;
    held_bytes_ += blob->size();
  } else if (previous) {
    DropHeld(key);
  }
  if (journal) CreateJournal(key);

  if (flying) return true;
  const auto queued = waiting_.find(key);
  if (queued != waiting_.end()) {
    queued->second.generation = gen;
    queued->second.blob = std::move(blob);
    return true;
  }
  waiting_.emplace(key, UploadItem{key, std::move(blob), gen, 0, {}});
  order_.push_back(key);
  return true;
}

std::optional<UploadItem> UploadQueue::TakeReady(std::chrono::steady_clock::time_point now,
                                                 std::chrono::steady_clock::time_point* soonest) {
  auto ready = order_.end();
  std::chrono::steady_clock::time_point earliest = std::chrono::steady_clock::time_point::max();
  for (auto it = order_.begin(); it != order_.end(); ++it) {
    const auto queued = waiting_.find(*it);
    if (queued == waiting_.end()) continue;
    if (queued->second.not_before <= now) {
      ready = it;
      break;
    }
    earliest = std::min(earliest, queued->second.not_before);
  }
  if (soonest != nullptr) *soonest = earliest;
  if (ready == order_.end()) return std::nullopt;
  const std::string key = *ready;
  order_.erase(ready);
  UploadItem item = std::move(waiting_.find(key)->second);
  waiting_.erase(key);
  MarkInFlight(item.key);
  if (item.blob) flight_pin_[item.key] = FlightPin{item.blob, false};
  return item;
}

bool UploadQueue::Finish(const UploadItem& item, bool* count_requeue) {
  if (count_requeue != nullptr) *count_requeue = false;
  in_flight_.erase(item.key);
  in_flight_id_.erase(item.key);
  ReleaseFlightPin(item.key);
  if (superseded_.erase(item.key) > 0) {
    recount_on_requeue_.erase(item.key);
    DropHeld(item.key);
    generation_.erase(item.key);
    RemoveJournal(item.key);
    return false;
  }
  const auto gen = generation_.find(item.key);
  if (gen != generation_.end() && gen->second != item.generation) {
    UploadItem again;
    again.key = item.key;
    again.generation = gen->second;
    const auto held = held_.find(item.key);
    if (held != held_.end()) again.blob = held->second;
    waiting_[again.key] = again;
    order_.push_back(again.key);
    if (count_requeue != nullptr) {
      *count_requeue = recount_on_requeue_.erase(item.key) > 0;
    } else {
      recount_on_requeue_.erase(item.key);
    }
    return true;
  }
  generation_.erase(item.key);
  DropHeld(item.key);
  RemoveJournal(item.key);
  recount_on_requeue_.erase(item.key);
  return false;
}

bool UploadQueue::Requeue(UploadItem item) {
  in_flight_.erase(item.key);
  in_flight_id_.erase(item.key);
  ReleaseFlightPin(item.key);
  if (superseded_.erase(item.key) > 0) {
    recount_on_requeue_.erase(item.key);
    DropHeld(item.key);
    generation_.erase(item.key);
    RemoveJournal(item.key);
    return true;
  }
  const auto gen = generation_.find(item.key);
  if (gen != generation_.end() && gen->second != item.generation) {
    item.generation = gen->second;
    item.attempts = 0;
    item.blob.reset();
    const auto held = held_.find(item.key);
    if (held != held_.end()) item.blob = held->second;
  }
  waiting_[item.key] = item;
  order_.push_back(item.key);
  return false;
}

}  // namespace vcache::daemon
