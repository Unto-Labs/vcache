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

bool UploadQueue::Enqueue(const std::string& key, std::shared_ptr<const std::string> blob,
                          bool journal) {
  std::shared_ptr<const std::string> previous;
  const auto held = held_.find(key);
  if (held != held_.end()) previous = held->second;
  if (blob != nullptr) {
    const uint64_t old_bytes = previous ? previous->size() : 0;
    const uint64_t next = held_bytes_ - old_bytes + blob->size();
    if (next > max_held_bytes_) return false;
  }

  uint64_t& gen = generation_[key];
  ++gen;
  if (blob != nullptr) {
    if (previous) held_bytes_ -= previous->size();
    held_[key] = blob;
    held_bytes_ += blob->size();
  }
  if (journal) CreateJournal(key);

  if (in_flight_.count(key) != 0) return true;
  for (UploadItem& item : queue_) {
    if (item.key != key) continue;
    item.generation = gen;
    if (blob != nullptr) item.blob = std::move(blob);
    return true;
  }
  queue_.push_back(UploadItem{key, std::move(blob), gen, 0, {}});
  return true;
}

std::optional<UploadItem> UploadQueue::TakeReady(std::chrono::steady_clock::time_point now,
                                                 std::chrono::steady_clock::time_point* soonest) {
  auto ready = queue_.end();
  std::chrono::steady_clock::time_point earliest = std::chrono::steady_clock::time_point::max();
  for (auto it = queue_.begin(); it != queue_.end(); ++it) {
    if (it->not_before <= now) {
      ready = it;
      break;
    }
    earliest = std::min(earliest, it->not_before);
  }
  if (soonest != nullptr) *soonest = earliest;
  if (ready == queue_.end()) return std::nullopt;
  UploadItem item = std::move(*ready);
  queue_.erase(ready);
  in_flight_.insert(item.key);
  return item;
}

bool UploadQueue::Finish(const UploadItem& item) {
  in_flight_.erase(item.key);
  const auto gen = generation_.find(item.key);
  if (gen != generation_.end() && gen->second != item.generation) {
    UploadItem again;
    again.key = item.key;
    again.generation = gen->second;
    const auto held = held_.find(item.key);
    if (held != held_.end()) again.blob = held->second;
    queue_.push_back(std::move(again));
    return true;
  }
  generation_.erase(item.key);
  const auto held = held_.find(item.key);
  if (held != held_.end()) {
    held_bytes_ -= held->second->size();
    held_.erase(held);
  }
  RemoveJournal(item.key);
  return false;
}

void UploadQueue::Requeue(UploadItem item) {
  in_flight_.erase(item.key);
  const auto gen = generation_.find(item.key);
  if (gen != generation_.end() && gen->second != item.generation) {
    item.generation = gen->second;
    item.blob.reset();
    const auto held = held_.find(item.key);
    if (held != held_.end()) item.blob = held->second;
  }
  queue_.push_back(std::move(item));
}

}  // namespace vcache::daemon
