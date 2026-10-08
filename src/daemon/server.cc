// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "daemon/jobserver.h"
#include "daemon/server.h"
#include "daemon/upload_queue.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "daemon/protocol.h"
#include "core/cost.h"
#include "storage/chain.h"
#include "storage/disk_storage.h"
#include "storage/s3_storage.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/str.h"

namespace vcache::daemon {

using Clock = std::chrono::steady_clock;

Clock::time_point LeaseWaitDeadline(Clock::time_point started, uint64_t bound_ms,
                                    Clock::duration memory_queued) {
  return started + std::chrono::milliseconds(std::min<uint64_t>(
      bound_ms, kReplyTimeoutSeconds * 1000)) +
      std::min(std::max(memory_queued, Clock::duration::zero()),
          std::chrono::duration_cast<Clock::duration>(
              std::chrono::milliseconds(kMemoryWaitBoundMs)));
}

Clock::time_point MemoryReserveDeadline(Clock::time_point started, uint64_t bound_ms) {
  return started + std::chrono::milliseconds(std::min(bound_ms, kMemoryWaitBoundMs));
}

namespace {

std::atomic<bool> g_signalled{false};

// Written to wake the accept loop at once: by a signal handler, and by a
// shutdown request. Without it the loop notices only at its next poll timeout.
int g_wake_fd = -1;

void Wake() {
  if (g_wake_fd < 0) return;
  const char byte = 1;
  ssize_t ignored = ::write(g_wake_fd, &byte, 1);  // async-signal-safe
  (void)ignored;
}

void OnTerminate(int) {
  g_signalled.store(true);
  Wake();
}

// Keys become file names under the cache and journal directories, so anything
// but a plain hex digest is refused before it can name a path. Every key vcache
// derives is one.
bool ValidKey(const std::string& key) {
  if (key.size() < 3 || key.size() > 128) return false;
  for (char c : key) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// The daemon's own log: lifecycle, refusals and failures, always on. Per-request
// detail stays behind VCACHE_LOG like everywhere else, since a busy build would
// otherwise grow this without bound. Rotated once at a few megabytes for the
// same reason.
class DaemonLog {
 public:
  void Open(std::string path) { path_ = std::move(path); }

  void Line(const std::string& message) {
    VCACHE_LOG("daemon: " + message);
    if (path_.empty()) return;
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    struct tm tm_buf;
    ::localtime_r(&now, &tm_buf);
    const size_t n = ::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    const std::string line = std::string(stamp, n) + " [" +
                             std::to_string(::getpid()) + "] " + message + "\n";
    std::lock_guard<std::mutex> lock(mutex_);
    auto size = util::FileSize(path_);
    if (size && *size > kRotateBytes) ::rename(path_.c_str(), (path_ + ".1").c_str());
    FILE* f = ::fopen(path_.c_str(), "a");
    if (f == nullptr) return;
    ::fwrite(line.data(), 1, line.size(), f);
    ::fclose(f);
  }

 private:
  static constexpr uint64_t kRotateBytes = 4ull << 20;
  std::string path_;
  std::mutex mutex_;
};

// A test shrinks the held-blob cap without changing the fingerprint a client
// and the daemon have to agree on. Unset, the queue keeps its 1 GiB cap.
uint64_t TestMaxHeldBytes() {
  const char* text = std::getenv("VCACHE_TEST_MAX_HELD_BYTES");
  if (text == nullptr || *text == '\0') return UploadQueue::kDefaultMaxHeldBytes;
  char* end = nullptr;
  const unsigned long long n = std::strtoull(text, &end, 10);
  if (end == text || *end != '\0' || n == 0) return UploadQueue::kDefaultMaxHeldBytes;
  return static_cast<uint64_t>(n);
}

// Half the client reply timeout, so a synchronous upload can still answer.
// Tests shrink it. A value above the cap is ignored.
std::chrono::milliseconds RefusalWaitBound() {
  const int cap_ms = kReplyTimeoutSeconds * 1000 / 2;
  const char* text = std::getenv("VCACHE_TEST_REFUSAL_WAIT_MS");
  if (text == nullptr || *text == '\0') return std::chrono::milliseconds(cap_ms);
  char* end = nullptr;
  const long n = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || n <= 0 || n > cap_ms) {
    return std::chrono::milliseconds(cap_ms);
  }
  return std::chrono::milliseconds(n);
}

struct Counters {
  std::atomic<uint64_t> connections{0};
  std::atomic<uint64_t> refused{0};
  std::atomic<uint64_t> lookups{0};
  std::atomic<uint64_t> hits_disk{0};
  std::atomic<uint64_t> hits_memory{0};
  std::atomic<uint64_t> hits_s3{0};
  std::atomic<uint64_t> misses{0};
  std::atomic<uint64_t> stores{0};
  std::atomic<uint64_t> stores_failed{0};
  std::atomic<uint64_t> uploads_queued{0};
  std::atomic<uint64_t> uploads_done{0};
  std::atomic<uint64_t> uploads_failed{0};
  std::atomic<uint64_t> uploads_skipped{0};
  std::atomic<uint64_t> upload_bytes{0};
  std::atomic<uint64_t> uploads_recovered{0};
  std::atomic<uint64_t> leases_expired{0};
  std::atomic<uint64_t> compiles_deduplicated{0};
};

struct CompileSession {
  CompileSession(int client_fd, pid_t peer_pid)
      : fd(client_fd), client_pid(peer_pid), opened_at(Clock::now()) {}
  ~CompileSession() { ::close(fd); }

  bool Reply(const std::string& reply) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (terminal_frame_sent_) return false;
    if (closing.load()) {
      SendTerminalFrame();
      return false;
    }
    const bool sent = SendFrame(fd, reply);
    if (!sent) {
      closing.store(true);
      ::shutdown(fd, SHUT_RDWR);
    }
    return sent;
  }

  void Shutdown() {
    closing.store(true);
    std::lock_guard<std::mutex> lock(send_mutex_);
    SendTerminalFrame();
  }

  const int fd;
  const pid_t client_pid;
  const Clock::time_point opened_at;
  std::atomic<bool> closing{false};
  Clock::duration memory_queued_time{};
  std::optional<Clock::time_point> memory_queued_at;

 private:
  void SendTerminalFrame() {
    if (terminal_frame_sent_) return;
    terminal_frame_sent_ = true;
    Writer out;
    out.U8(static_cast<uint8_t>(Status::kError));
    out.Str("daemon shutting down");
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    SendFrame(fd, out.data());
    ::shutdown(fd, SHUT_RDWR);
  }

  std::mutex send_mutex_;
  bool terminal_frame_sent_ = false;
};

struct LeaseWait {
  std::weak_ptr<CompileSession> holder;
  pid_t holder_pid = 0;
  Clock::time_point acquired_at = Clock::now();
  std::set<int> waiters;
  size_t stores_in_progress = 0;
  std::optional<LeaseOutcome> outcome;
  LeaseCompileReason reason = LeaseCompileReason::kNone;
  std::condition_variable changed;
};

struct CacheFetch {
  std::atomic<bool> done{false};
  bool hit = false;
  std::string value;
  std::vector<std::string> errors;
  std::condition_variable changed;
};

struct KeyLease {
  std::shared_ptr<LeaseWait> compile;
  std::shared_ptr<CacheFetch> fetch;
};

struct MemoryReservation {
  ~MemoryReservation() { if (pid_fd >= 0) ::close(pid_fd); }
  std::weak_ptr<CompileSession> session;
  std::string cost_key;
  uint64_t estimate_kb = 0;
  uint64_t realised_kb = 0;
  uint64_t peak_rss_kb = 0;
  bool rss_sampled = false;
  pid_t compiler_pid = 0;
  int pid_fd = -1;
  std::optional<Clock::time_point> fallback_started;
};

struct MemoryWait {
  std::shared_ptr<CompileSession> session;
  std::string cost_key;
  uint64_t estimate_kb = 0;
  Clock::time_point started = Clock::now();
  std::optional<Clock::time_point> queued_deadline;
  std::optional<MemoryOutcome> outcome;
  std::condition_variable changed;
};

Clock::duration MemoryQueueTime(const CompileSession& session, Clock::time_point now) {
  return session.memory_queued_time + (session.memory_queued_at && now > *session.memory_queued_at
      ? now - *session.memory_queued_at : Clock::duration::zero());
}

uint64_t SaturatingAddKb(uint64_t left, uint64_t right) {
  return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}

std::optional<uint64_t> StatusNumber(const std::string& text, const std::string& field) {
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream columns(line);
    std::string name, number;
    if (!(columns >> name >> number) || name != field + ":") continue;
    uint64_t value = 0;
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), value);
    if (parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size()) return value;
  }
  return std::nullopt;
}

std::optional<uint64_t> ProcessTreeRssKb(pid_t root_pid) {
  const char* failed_read = std::getenv("VCACHE_DAEMON_TEST_FAIL_RSS");
  if (failed_read && util::FileExists(failed_read)) return std::nullopt;
  std::vector<pid_t> pending{root_pid};
  std::set<pid_t> visited;
  uint64_t total_kb = 0;
  while (!pending.empty()) {
    const pid_t pid = pending.back();
    pending.pop_back();
    if (!visited.insert(pid).second) continue;
    const std::string process = "/proc/" + std::to_string(pid);
    const auto status = util::ReadFile(process + "/status");
    const auto rss_kb = status ? StatusNumber(*status, "VmRSS") : std::nullopt;
    if (!rss_kb && pid == root_pid) return std::nullopt;
    total_kb = SaturatingAddKb(total_kb, rss_kb.value_or(0));
    std::error_code error;
    for (std::filesystem::directory_iterator task(process + "/task", error), end;
         !error && task != end; task.increment(error)) {
      const auto children = util::ReadFile(task->path().string() + "/children");
      if (!children) continue;
      std::istringstream pids(*children);
      pid_t child_pid = 0;
      while (pids >> child_pid) if (child_pid > 0) pending.push_back(child_pid);
    }
    if (error && pid == root_pid) return std::nullopt;
  }
  return total_kb;
}

std::string ReservationReleaseLog(const MemoryReservation& reservation) {
  return "reserve: " + std::to_string(reservation.compiler_pid) +
      (reservation.rss_sampled ? " rss " + std::to_string(reservation.peak_rss_kb) : " unsampled") +
      " of " + std::to_string(reservation.estimate_kb) + " at release";
}

void AwaitAdmissionTestGate(const char* variable, const std::string& message) {
  const char* path = std::getenv(variable);
  if (!path || !*path) return;
  const int fd = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return;
  VCACHE_LOG(message);
  pollfd gate{fd, POLLIN, 0};
  const auto started = Clock::now();
  const auto deadline = started + std::chrono::seconds(5);
  while (Clock::now() < deadline) {
    const auto left_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - Clock::now()).count();
    if (::poll(&gate, 1, static_cast<int>(left_ms)) > 0) {
      char byte = 0;
      if (::read(fd, &byte, 1) == 1) break;
    }
  }
  ::close(fd);
  VCACHE_LOG(message + " waited " + std::to_string(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()) +
      " ms");
}

bool SessionGone(const CompileSession& session) {
  if (session.closing.load()) return true;
  pollfd peer{session.fd, POLLIN, 0};
  if (::poll(&peer, 1, 0) <= 0) return false;
  char byte;
  return ::recv(session.fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT) == 0 ||
         (peer.revents & (POLLHUP | POLLERR | POLLNVAL));
}

class Server {
 public:
  explicit Server(const core::Config& config)
      : config_(config),
        state_dir_(StateDir(config)),
        socket_path_(SocketPath(config)),
        fingerprint_(ConfigFingerprint(config)),
        uploads_(state_dir_ + "/pending", TestMaxHeldBytes()) {}

  int Run(int ready_fd);

 private:
  bool PrepareStateDir(std::string* error);
  bool AcquireLock(std::string* error);
  bool Listen(std::string* error);
  void SetUpS3();
  void RecoverJournal();

  void Serve(int fd);
  bool HandleHello(const std::string& request, std::string* reply);
  void HandleGet(Reader* in, Writer* out);
  std::shared_ptr<CacheFetch> FetchKey(const std::string& key);
  void HandlePut(Reader* in, Writer* out, pid_t peer_pid);
  bool AwaitTestPut(const std::string& key);
  void HandleLeaseAcquire(Reader* in, Writer* out,
                          const std::shared_ptr<CompileSession>& session);
  void HandleLeaseRelease(Reader* in, Writer* out,
                          const std::shared_ptr<CompileSession>& session);
  void CompleteLease(const std::string& key, LeaseOutcome outcome, LeaseCompileReason reason);
  void ExpireLeases(const std::shared_ptr<CompileSession>& session);
  void HandleMemoryReserve(Reader* in, Writer* out,
                           const std::shared_ptr<CompileSession>& session);
  void HandleCompilerSpawned(Reader* in, Writer* out,
                             const std::shared_ptr<CompileSession>& session);
  void UpdateAdmission(std::unique_lock<std::mutex>& lock,
                       std::optional<int> spawned_fd = std::nullopt, bool periodic = false);
  void GrantMemoryWaiters();
  void RememberTreePeak(const MemoryReservation& reservation);
  void EndMemoryQueue(const std::shared_ptr<MemoryWait>& waiter);
  void AdmissionWorker();
  uint64_t UnrealisedKb() const;
  uint64_t AvailableKb() const;
  void HandleShutdown(Writer* out);
  std::string StatusText();

  std::unique_ptr<storage::DiskStorage> MakeDisk() const;
  std::unique_ptr<storage::S3Storage> MakeS3() const;
  std::unique_ptr<storage::S3Storage> AcquireS3();
  void ReleaseS3(std::unique_ptr<storage::S3Storage> s3);

  // Queues an upload. Returns false only when the blob could not be held.
  // `refused_in_flight` is set when that refusal replaced an upload already
  // being sent. `refusal_id` identifies that upload so the caller waits for
  // it, not for a later generation of the same key.
  bool Enqueue(const std::string& key, std::shared_ptr<const std::string> blob, bool journal,
               bool* refused_in_flight = nullptr, uint64_t* refusal_id = nullptr);
  void WaitForRefusal(uint64_t refusal_id, const std::string& key);
  void UploadWorker();
  void Shutdown();
  bool Idle() const;
  // Seconds of quiet this process waits before exiting. Twice the configured
  // timeout while a jobserver client holds a token, because that client is
  // not a socket connection.
  int IdleLimitSeconds() const;
  void StartJobserver();
  void TickJobserver();

  const core::Config config_;
  const std::string state_dir_;
  const std::string socket_path_;
  const std::string fingerprint_;
  DaemonLog log_;
  Counters counters_;
  const Clock::time_point started_ = Clock::now();

  int lock_fd_ = -1;
  int listen_fd_ = -1;
  ino_t socket_inode_ = 0;

  bool s3_enabled_ = false;
  bool s3_writable_ = false;
  std::mutex s3_pool_mutex_;
  std::vector<std::unique_ptr<storage::S3Storage>> s3_pool_;

  // Connection bookkeeping. `mutex_` guards everything below it.
  mutable std::mutex mutex_;
  std::unique_ptr<JobserverPool> jobserver_;
  int jobserver_min_jobs_ = 2;
  std::condition_variable cv_;
  std::set<int> connections_;
  std::map<int, std::shared_ptr<CompileSession>> sessions_;
  std::map<std::string, KeyLease> leases_;
  std::map<int, std::shared_ptr<MemoryReservation>> reservations_;
  std::map<std::string, uint64_t> tree_peak_rss_kb_;
  std::deque<std::shared_ptr<MemoryWait>> memory_waiters_;
  uint64_t mem_available_kb_ = 0;
  bool meminfo_known_ = false;
  bool meminfo_failure_logged_ = false;
  bool admission_sampling_ = false;
  bool test_rss_blocked_ = false;
  Clock::time_point rss_read_at_{};
  uint64_t reserve_waits_ = 0;
  uint64_t longest_reserve_wait_ms_ = 0;
  std::thread admission_worker_;
  bool test_put_blocked_ = false;
  int shutdown_waiters_ = 0;
  Clock::time_point last_activity_ = Clock::now();
  std::atomic<bool> stop_requested_{false};
  bool drained_ = false;
  std::string drain_summary_;

  // Upload queue, also under `mutex_`.
  UploadQueue uploads_;
  bool workers_stop_ = false;
  std::vector<std::thread> workers_;

  static constexpr int kMaxUploadAttempts = 3;
  static constexpr size_t kMaxConnections = 1024;
};

std::unique_ptr<storage::DiskStorage> Server::MakeDisk() const {
  if (!config_.disk.enabled) return nullptr;
  return std::make_unique<storage::DiskStorage>(config_.disk.dir, config_.disk.max_size,
                                                config_.read_only);
}

std::unique_ptr<storage::S3Storage> Server::MakeS3() const {
  auto s3 = std::make_unique<storage::S3Storage>(config_.s3);
  if (config_.read_only) s3->set_read_only(true);
  s3->set_reuse_connection(true);
  return s3;
}

std::unique_ptr<storage::S3Storage> Server::AcquireS3() {
  {
    std::lock_guard<std::mutex> lock(s3_pool_mutex_);
    if (!s3_pool_.empty()) {
      auto s3 = std::move(s3_pool_.back());
      s3_pool_.pop_back();
      return s3;
    }
  }
  return MakeS3();
}

void Server::ReleaseS3(std::unique_ptr<storage::S3Storage> s3) {
  std::lock_guard<std::mutex> lock(s3_pool_mutex_);
  s3_pool_.push_back(std::move(s3));
}

bool Server::PrepareStateDir(std::string* error) {
  if (!util::MakeDirs(state_dir_ + "/pending")) {
    *error = "cannot create " + state_dir_ + ": " + std::strerror(errno);
    return false;
  }
  // The socket lives here, and file permissions on the directory are the
  // portable way to keep other users off it: macOS ignores a socket's own mode.
  ::chmod(state_dir_.c_str(), 0700);
  log_.Open(state_dir_ + "/log");
  return true;
}

bool Server::AcquireLock(std::string* error) {
  const std::string path = state_dir_ + "/lock";
  lock_fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lock_fd_ < 0) {
    *error = "cannot open " + path + ": " + std::strerror(errno);
    return false;
  }
  if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
    ::close(lock_fd_);
    lock_fd_ = -1;
    return false;  // held: another daemon owns this cache
  }
  util::WriteFileAtomic(state_dir_ + "/pid", std::to_string(::getpid()) + "\n");
  return true;
}

bool Server::Listen(std::string* error) {
  const std::string dir = util::DirName(socket_path_);
  if (!util::MakeDirs(dir)) {
    *error = "cannot create " + dir + ": " + std::strerror(errno);
    return false;
  }
  // A socket outside the state directory sits in a directory that may be
  // shared. Insist it is ours and private rather than trust whoever made it.
  struct stat st;
  if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
      st.st_uid != ::getuid()) {
    *error = dir + " is not a directory owned by this user";
    return false;
  }
  ::chmod(dir.c_str(), 0700);

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socket_path_.size() >= sizeof(addr.sun_path)) {
    *error = "socket path too long: " + socket_path_;
    return false;
  }
  std::memcpy(addr.sun_path, socket_path_.c_str(), socket_path_.size() + 1);

  // Holding the lock means no other daemon serves this cache, so whatever is
  // at the path is a leftover from one that died.
  ::unlink(socket_path_.c_str());

  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    *error = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  ::fcntl(listen_fd_, F_SETFD, FD_CLOEXEC);
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    *error = "bind " + socket_path_ + ": " + std::strerror(errno);
    return false;
  }
  ::chmod(socket_path_.c_str(), 0600);
  if (::lstat(socket_path_.c_str(), &st) == 0) socket_inode_ = st.st_ino;
  if (::listen(listen_fd_, SOMAXCONN) != 0) {
    *error = std::string("listen: ") + std::strerror(errno);
    return false;
  }
  return true;
}

void Server::SetUpS3() {
  if (!config_.s3.enabled) return;
  auto probe = MakeS3();
  if (!probe->available()) {
    log_.Line("s3 layer disabled: " + probe->load_error());
    return;
  }
  s3_enabled_ = true;
  s3_writable_ = probe->writable();
  ReleaseS3(std::move(probe));
  if (!s3_writable_) return;
  for (int i = 0; i < config_.daemon.upload_threads; ++i) {
    workers_.emplace_back([this] { UploadWorker(); });
  }
}

void Server::RecoverJournal() {
  const std::string dir = state_dir_ + "/pending";
  std::vector<std::string> keys;
  for (const auto& entry : util::ListFilesRecursive(dir)) {
    const std::string key = util::BaseName(entry.path);
    if (ValidKey(key)) keys.push_back(key);
  }
  if (keys.empty()) return;
  if (!s3_writable_ || !config_.disk.enabled) {
    // Kept, not deleted: a later daemon configured with a writable bucket can
    // still upload them.
    log_.Line(std::to_string(keys.size()) +
              " journalled uploads left pending: no writable s3 layer");
    return;
  }
  for (const std::string& key : keys) {
    if (Enqueue(key, nullptr, /*journal=*/false)) counters_.uploads_recovered++;
  }
  log_.Line("recovered " + std::to_string(keys.size()) +
            " uploads journalled by a previous daemon");
}

bool Server::Enqueue(const std::string& key, std::shared_ptr<const std::string> blob, bool journal,
                     bool* refused_in_flight, uint64_t* refusal_id) {
  std::unique_lock<std::mutex> lock(mutex_);
  bool merged = false;
  bool in_flight = false;
  bool dropped_waiting = false;
  uint64_t id = 0;
  if (!uploads_.Enqueue(key, std::move(blob), journal, &merged, &in_flight, &id,
                        &dropped_waiting)) {
    if (refused_in_flight != nullptr) *refused_in_flight = in_flight;
    if (refusal_id != nullptr) *refusal_id = id;
    if (dropped_waiting) counters_.uploads_skipped++;
    lock.unlock();
    cv_.notify_all();
    return false;
  }
  // A key already waiting keeps its one queued upload. A key in flight is
  // counted when that upload finishes and the newer value is queued.
  if (!merged && !in_flight) counters_.uploads_queued++;
  lock.unlock();
  cv_.notify_all();
  return true;
}

void Server::WaitForRefusal(uint64_t refusal_id, const std::string& key) {
  std::unique_lock<std::mutex> lock(mutex_);
  // The caller retries this wait. Uploading beside the flight that is still
  // running is how an older value wins.
  const auto deadline = Clock::now() + RefusalWaitBound();
  while (uploads_.RefusalStillInFlight(refusal_id)) {
    if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
      log_.Line("refused re-put of " + key + " still in flight");
      return;
    }
  }
}

void Server::UploadWorker() {
  auto s3 = MakeS3();
  auto disk = MakeDisk();
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    // The first item whose retry delay has passed. Retries go to the back, so
    // this is nearly always the front.
    Clock::time_point soonest = Clock::time_point::max();
    auto ready = uploads_.TakeReady(Clock::now(), &soonest);
    if (!ready) {
      if (workers_stop_ && uploads_.empty()) return;
      if (soonest == Clock::time_point::max()) {
        cv_.wait(lock);
      } else {
        cv_.wait_until(lock, soonest);
      }
      continue;
    }
    UploadItem item = std::move(*ready);
    lock.unlock();

    std::string from_disk;
    const std::string* blob = item.blob.get();
    bool have = blob != nullptr;
    if (!have && disk != nullptr) {
      have = disk->Get(item.key, &from_disk);
      blob = &from_disk;
    }

    bool done = true;
    bool put_ok = false;
    std::string failed_detail;
    if (have && s3->Put(item.key, *blob)) {
      put_ok = true;
    } else if (have && ++item.attempts < kMaxUploadAttempts) {
      // S3Storage already retries throttling inside one attempt; this covers
      // the connection-level failures it does not, with a longer gap.
      done = false;
      item.not_before = Clock::now() + std::chrono::seconds(1 << item.attempts);
      VCACHE_LOG("daemon: upload " + item.key + " failed (" + s3->last_error() +
                 "); will retry");
    } else if (have) {
      failed_detail = s3->failed() ? s3->last_error() : std::string("rejected");
    }

    // The terminal count and Finish/Requeue share this lock with StatusText,
    // so one upload cannot be both completed and still pending.
    lock.lock();
    if (!have) {
      // Evicted between the store and its turn. Nothing to send.
      counters_.uploads_skipped++;
    } else if (put_ok) {
      counters_.uploads_done++;
      counters_.upload_bytes += blob->size();
    } else if (done) {
      counters_.uploads_failed++;
      log_.Line("upload " + item.key + " failed after " +
                std::to_string(item.attempts) + " attempts: " + failed_detail);
    }
    if (done) {
      bool count_requeue = false;
      if (uploads_.Finish(item, &count_requeue)) {
        if (count_requeue) counters_.uploads_queued++;
        log_.Line("re-queued " + item.key + " (rewritten during upload)");
      }
    } else if (uploads_.Requeue(std::move(item))) {
      counters_.uploads_skipped++;
    }
    cv_.notify_all();
  }
}

bool Server::HandleHello(const std::string& request, std::string* reply) {
  Reader in(request);
  uint8_t op = 0;
  uint64_t version = 0;
  std::string theirs;
  Writer out;
  if (!in.U8(&op) || op != static_cast<uint8_t>(Op::kHello) || !in.U64(&version) ||
      !in.Str(&theirs)) {
    out.U8(static_cast<uint8_t>(Status::kRefused));
    out.Str("malformed hello");
    *reply = out.data();
    return false;
  }
  std::string why;
  if (version != kProtocolVersion) {
    why = "protocol version " + std::to_string(version) + ", daemon speaks " +
          std::to_string(kProtocolVersion);
  } else {
    why = FingerprintMismatch(fingerprint_, theirs);
  }
  if (!why.empty()) {
    counters_.refused++;
    log_.Line("refused a client: " + why);
    out.U8(static_cast<uint8_t>(Status::kRefused));
    out.Str(why);
    *reply = out.data();
    return false;
  }
  out.U8(static_cast<uint8_t>(Status::kOk));
  out.U64(static_cast<uint64_t>(::getpid()));
  *reply = out.data();
  return true;
}

void Server::CompleteLease(const std::string& key, LeaseOutcome outcome,
                            LeaseCompileReason reason) {
  auto found = leases_.find(key);
  if (found == leases_.end()) return;
  auto lease = found->second.compile;
  if (lease && !lease->outcome) {
    lease->outcome = outcome;
    lease->reason = reason;
    lease->changed.notify_all();
    found->second.compile.reset();
  }
  if (!found->second.compile && !found->second.fetch) {
    leases_.erase(found);
  }
}

void Server::ExpireLeases(const std::shared_ptr<CompileSession>& session) {
  std::vector<std::string> held_keys;
  for (const auto& [key, state] : leases_) {
    // A Put already accepted on another connection decides the result even
    // if its holder is killed before storage finishes.
    if (state.compile && state.compile->holder.lock() == session &&
        state.compile->stores_in_progress == 0) {
      held_keys.push_back(key);
    }
  }
  for (const auto& key : held_keys) {
    counters_.leases_expired++;
    CompleteLease(key, LeaseOutcome::kCompile, LeaseCompileReason::kHolderGone);
  }
}

void Server::HandleLeaseAcquire(Reader* in, Writer* out,
                                const std::shared_ptr<CompileSession>& session) {
  std::string key;
  uint64_t bound_ms = 0;
  if (!session || config_.read_only ||
      !in->Str(&key) || !ValidKey(key) || !in->U64(&bound_ms) || !in->done()) {
    out->U8(static_cast<uint8_t>(Status::kError));
    out->Str("invalid lease acquire");
    return;
  }
  bound_ms = std::min<uint64_t>(bound_ms, kReplyTimeoutSeconds * 1000);
  const auto started = Clock::now();
  std::unique_lock<std::mutex> lock(mutex_);
  LeaseOutcome outcome = LeaseOutcome::kCompile;
  LeaseCompileReason reason = LeaseCompileReason::kShutdown;
  uint64_t holder_pid = 0;
  if (!stop_requested_.load()) {
    auto& state = leases_[key];
    if (!state.compile) {
      state.compile = std::make_shared<LeaseWait>();
      state.compile->holder = session;
      state.compile->holder_pid = session->client_pid;
    }
    auto lease = state.compile;
    const auto holder = lease->holder.lock();
    holder_pid = lease->holder_pid;
    if (holder == session) {
      outcome = LeaseOutcome::kGranted;
      reason = LeaseCompileReason::kNone;
    } else {
      const auto queue_time_at_start = holder ? MemoryQueueTime(*holder, started) :
                                               Clock::duration::zero();
      auto wait_deadline = [&] {
        const auto queued_time = holder ? MemoryQueueTime(*holder, Clock::now()) -
                                         queue_time_at_start : Clock::duration::zero();
        return LeaseWaitDeadline(started, bound_ms, queued_time);
      };
      lease->waiters.insert(session->fd);
      VCACHE_LOG("lease: waiting on holder pid " + std::to_string(holder_pid) + " up to " +
                 std::to_string(bound_ms) + " ms");
      bool waiter_gone = false;
      while (!lease->outcome && !stop_requested_.load() && Clock::now() < wait_deadline()) {
        lease->changed.wait_until(lock, std::min(wait_deadline(),
                                      Clock::now() + std::chrono::milliseconds(250)));
        if (lease->outcome || stop_requested_.load()) break;
        lock.unlock();
        pollfd peer{session->fd, POLLIN, 0};
        if (::poll(&peer, 1, 0) > 0) {
          char byte;
          if (::recv(session->fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT) == 0 ||
              (peer.revents & (POLLHUP | POLLERR | POLLNVAL))) waiter_gone = true;
        }
        lock.lock();
        if (waiter_gone) break;
      }
      lease->waiters.erase(session->fd);
      if (lease->outcome) {
        outcome = *lease->outcome;
        reason = lease->reason;
        if (outcome == LeaseOutcome::kStored) counters_.compiles_deduplicated++;
      } else {
        reason = stop_requested_.load() ? LeaseCompileReason::kShutdown : LeaseCompileReason::kBound;
      }
      const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          Clock::now() - started).count();
      const auto paused_ms = std::min<uint64_t>(kMemoryWaitBoundMs,
          std::chrono::duration_cast<std::chrono::milliseconds>(
              holder ? MemoryQueueTime(*holder, Clock::now()) - queue_time_at_start :
                       Clock::duration::zero()).count());
      if (paused_ms > 0) {
        VCACHE_LOG("lease: memory queue paused bound by " + std::to_string(paused_ms) + " ms");
      }
      VCACHE_LOG("lease: wait ended after " + std::to_string(waited_ms) + " ms");
      if (waiter_gone) VCACHE_LOG("lease: waiter gone after " + std::to_string(waited_ms) + " ms");
    }
  }
  out->U8(static_cast<uint8_t>(Status::kOk));
  out->U8(static_cast<uint8_t>(outcome));
  out->U8(static_cast<uint8_t>(reason));
  out->U64(holder_pid);
  out->U64(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
}

void Server::HandleLeaseRelease(Reader* in, Writer* out,
                                const std::shared_ptr<CompileSession>& session) {
  std::string key;
  uint8_t outcome = 0;
  if (!session || !in->Str(&key) || !ValidKey(key) ||
      !in->U8(&outcome) || outcome > 1 || !in->done()) {
    out->U8(static_cast<uint8_t>(Status::kError));
    out->Str("invalid lease release");
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = leases_.find(key);
  if (found != leases_.end() && found->second.compile &&
      found->second.compile->holder.lock() != session) {
    out->U8(static_cast<uint8_t>(Status::kError));
    out->Str("lease held by another session");
    return;
  }
  CompleteLease(key, outcome == 0 ? LeaseOutcome::kStored : LeaseOutcome::kCompile,
                 outcome == 0 ? LeaseCompileReason::kNone : LeaseCompileReason::kHolderFailed);
  out->U8(static_cast<uint8_t>(Status::kOk));
}

uint64_t Server::UnrealisedKb() const {
  uint64_t unrealised_kb = 0;
  for (const auto& [fd, reservation] : reservations_) {
    unrealised_kb = SaturatingAddKb(unrealised_kb, reservation->estimate_kb -
        std::min(reservation->estimate_kb, reservation->realised_kb));
  }
  return unrealised_kb;
}

uint64_t Server::AvailableKb() const {
  if (!meminfo_known_) return UINT64_MAX;
  return mem_available_kb_ - std::min(mem_available_kb_, UnrealisedKb());
}

void Server::UpdateAdmission(std::unique_lock<std::mutex>& lock,
                             std::optional<int> spawned_fd, bool periodic) {
  cv_.wait(lock, [&] { return !admission_sampling_ || stop_requested_.load(); });
  if (stop_requested_.load()) return;
  const auto now = Clock::now();
  if (!periodic && !spawned_fd && now - rss_read_at_ < std::chrono::milliseconds(500)) {
    GrantMemoryWaiters();
    return;
  }
  struct Sample {
    int fd;
    std::shared_ptr<MemoryReservation> reservation;
    bool released = false;
    std::optional<uint64_t> rss_kb;
  };
  std::vector<Sample> samples;
  for (const auto& [fd, reservation] : reservations_) {
    if (!spawned_fd || fd == *spawned_fd) samples.push_back({fd, reservation});
  }
  admission_sampling_ = true;
  const bool block_sample = !test_rss_blocked_ &&
      std::getenv("VCACHE_DAEMON_TEST_RSS_GATE") &&
      std::any_of(samples.begin(), samples.end(), [](const auto& sample) {
        return sample.reservation->compiler_pid > 0;
      });
  if (block_sample) test_rss_blocked_ = true;
  lock.unlock();
  if (block_sample) AwaitAdmissionTestGate("VCACHE_DAEMON_TEST_RSS_GATE",
                                          "reserve: test RSS sample blocked");
  const char* test_path = std::getenv("VCACHE_DAEMON_MEMINFO");
  const std::string path = test_path && *test_path ? test_path : "/proc/meminfo";
  const auto available_kb = StatusNumber(util::ReadFile(path).value_or(""), "MemAvailable");
  for (auto& sample : samples) {
    const auto& reservation = *sample.reservation;
    if (spawned_fd && std::getenv("VCACHE_DAEMON_TEST_TRACE_RSS")) {
      VCACHE_LOG("reserve: test spawn sample " + std::to_string(reservation.compiler_pid));
    }
    const auto session = reservation.session.lock();
    pollfd exited{reservation.pid_fd, POLLIN, 0};
    const bool compiler_exited = reservation.pid_fd >= 0 && ::poll(&exited, 1, 0) > 0;
    sample.released = !session || SessionGone(*session) || compiler_exited;
    if (!sample.released && reservation.compiler_pid > 0) {
      if (reservation.fallback_started) {
        if (now - *reservation.fallback_started >= std::chrono::seconds(10)) {
          sample.rss_kb = reservation.estimate_kb;
        }
      } else {
        sample.rss_kb = ProcessTreeRssKb(reservation.compiler_pid);
      }
    }
  }
  lock.lock();
  std::vector<std::string> messages;
  bool meminfo_warning = false;
  if (available_kb) {
    mem_available_kb_ = *available_kb;
    meminfo_known_ = true;
  } else if (!meminfo_failure_logged_) {
    meminfo_failure_logged_ = true;
    meminfo_warning = true;
    messages.push_back(meminfo_known_
        ? "reserve: MemAvailable unreadable, retaining last good value"
        : "reserve: MemAvailable unreadable, admission unavailable");
  }
  for (const auto& sample : samples) {
    const auto found = reservations_.find(sample.fd);
    if (found == reservations_.end() || found->second != sample.reservation) continue;
    auto& reservation = *found->second;
    if (sample.released) {
      RememberTreePeak(reservation);
      messages.push_back(ReservationReleaseLog(reservation));
      reservations_.erase(found);
    } else if (sample.rss_kb) {
      if (!reservation.fallback_started && *sample.rss_kb > reservation.peak_rss_kb) {
        if (*sample.rss_kb > reservation.estimate_kb &&
            reservation.peak_rss_kb <= reservation.estimate_kb) {
          messages.push_back("reserve: rss overshoot " + std::to_string(reservation.compiler_pid) +
              " by " + std::to_string(*sample.rss_kb - reservation.estimate_kb) + " kB");
        }
        reservation.peak_rss_kb = *sample.rss_kb;
      }
      if (!reservation.fallback_started) reservation.rss_sampled = true;
      if (available_kb || !meminfo_known_ || reservation.fallback_started) {
        reservation.realised_kb = *sample.rss_kb;
      }
    }
  }
  if (!spawned_fd) rss_read_at_ = now;
  admission_sampling_ = false;
  cv_.notify_all();
  GrantMemoryWaiters();
  lock.unlock();
  if (meminfo_warning) log_.Line(messages.front());
  for (size_t index = meminfo_warning ? 1 : 0; index < messages.size(); ++index) {
    VCACHE_LOG(messages[index]);
  }
  lock.lock();
}

void Server::EndMemoryQueue(const std::shared_ptr<MemoryWait>& waiter) {
  auto& session = *waiter->session;
  if (!session.memory_queued_at) return;
  session.memory_queued_time = MemoryQueueTime(session, Clock::now());
  session.memory_queued_at.reset();
  for (const auto& [key, state] : leases_) {
    if (state.compile && state.compile->holder.lock() == waiter->session) {
      state.compile->changed.notify_all();
    }
  }
}

void Server::GrantMemoryWaiters() {
  while (!memory_waiters_.empty() && !stop_requested_.load()) {
    auto waiter = memory_waiters_.front();
    const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - waiter->started).count();
    const bool gone = SessionGone(*waiter->session);
    const bool expired = waiter->queued_deadline && Clock::now() >= *waiter->queued_deadline;
    if (gone || expired) {
      memory_waiters_.pop_front();
      EndMemoryQueue(waiter);
      waiter->outcome = MemoryOutcome::kBound;
      VCACHE_LOG(std::string("reserve: ") + (gone ? "waiter gone after " :
                 "wait bound reached after ") + std::to_string(waited_ms) + " ms");
      waiter->changed.notify_all();
      continue;
    }
    const auto peak = tree_peak_rss_kb_.find(waiter->cost_key);
    if (peak != tree_peak_rss_kb_.end()) {
      waiter->estimate_kb = std::max(waiter->estimate_kb, peak->second);
    }
    const uint64_t available_kb = AvailableKb();
    if (!reservations_.empty() && available_kb < waiter->estimate_kb) break;
    auto reservation = std::make_shared<MemoryReservation>();
    reservation->session = waiter->session;
    reservation->cost_key = waiter->cost_key;
    reservation->estimate_kb = waiter->estimate_kb;
    VCACHE_LOG("reserve: granted " + std::to_string(waiter->estimate_kb) + " kB after " +
               std::to_string(waited_ms) + " ms (available " + std::to_string(available_kb) +
               ", unrealised " + std::to_string(UnrealisedKb()) + ")");
    reservations_.emplace(waiter->session->fd, std::move(reservation));
    memory_waiters_.pop_front();
    EndMemoryQueue(waiter);
    waiter->outcome = MemoryOutcome::kGranted;
    waiter->changed.notify_all();
  }
}

void Server::RememberTreePeak(const MemoryReservation& reservation) {
  if (reservation.peak_rss_kb == 0) return;
  auto& peak = tree_peak_rss_kb_[reservation.cost_key];
  peak = std::max(peak, reservation.peak_rss_kb);
}

void Server::HandleMemoryReserve(Reader* in, Writer* out,
                                 const std::shared_ptr<CompileSession>& session) {
  std::string cost_key;
  uint64_t estimate_kb = 0, bound_ms = 0;
  uint8_t reply_fields = 0;
  if (!session || config_.read_only ||
      !in->Str(&cost_key) || !ValidKey(cost_key) || !in->U64(&estimate_kb) ||
      !in->U64(&bound_ms) ||
      (!in->done() && (!in->U8(&reply_fields) ||
                      reply_fields != kMemoryReserveReplyEstimate)) || !in->done()) {
    out->U8(static_cast<uint8_t>(Status::kError));
    out->Str("invalid memory reserve");
    return;
  }
  if (estimate_kb == 0) {
    const auto cost = core::LoadCompileCost(config_.disk.dir, cost_key);
    estimate_kb = cost.operation == "link" ? config_.daemon.default_link_kb :
                                           config_.daemon.default_compile_kb;
  }
  auto waiter = std::make_shared<MemoryWait>();
  waiter->session = session;
  waiter->cost_key = cost_key;
  waiter->estimate_kb = estimate_kb;
  const auto deadline = MemoryReserveDeadline(waiter->started, bound_ms);
  AwaitAdmissionTestGate("VCACHE_DAEMON_TEST_RESERVE_GATE", "reserve: test reserve blocked");
  std::unique_lock<std::mutex> lock(mutex_);
  UpdateAdmission(lock);
  if (!stop_requested_.load() && !admission_worker_.joinable()) {
    admission_worker_ = std::thread([this] { AdmissionWorker(); });
  }
  if (reservations_.contains(session->fd)) {
    waiter->outcome = MemoryOutcome::kGranted;
  } else {
    const size_t behind = memory_waiters_.size();
    memory_waiters_.push_back(waiter);
    GrantMemoryWaiters();
    if (!waiter->outcome) {
      waiter->queued_deadline = deadline;
      session->memory_queued_at = Clock::now();
      ++reserve_waits_;
      VCACHE_LOG("reserve: waiting (" + std::to_string(AvailableKb()) + " < " +
                 std::to_string(waiter->estimate_kb) + ") behind " + std::to_string(behind));
      VCACHE_LOG("reserve: wait bound " +
                 std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                     deadline - waiter->started).count()) + " ms");
    }
  }
  waiter->changed.wait_until(lock, deadline,
      [&] { return waiter->outcome || stop_requested_.load(); });
  const uint64_t waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      Clock::now() - waiter->started).count();
  longest_reserve_wait_ms_ = std::max(longest_reserve_wait_ms_, waited_ms);
  if (!waiter->outcome) {
    waiter->outcome = MemoryOutcome::kBound;
    std::erase(memory_waiters_, waiter);
    EndMemoryQueue(waiter);
    VCACHE_LOG(std::string("reserve: ") + (stop_requested_.load() ? "shutdown after " :
               "wait bound reached after ") + std::to_string(waited_ms) + " ms");
    GrantMemoryWaiters();
  }
  out->U8(static_cast<uint8_t>(Status::kOk));
  out->U8(static_cast<uint8_t>(*waiter->outcome));
  out->U64(waited_ms);
  if (reply_fields == kMemoryReserveReplyEstimate) {
    const auto reservation = reservations_.find(session->fd);
    out->U64(*waiter->outcome == MemoryOutcome::kGranted && reservation != reservations_.end() ?
                 reservation->second->estimate_kb : 0);
  }
}

void Server::HandleCompilerSpawned(Reader* in, Writer* out,
                                   const std::shared_ptr<CompileSession>& session) {
  uint64_t compiler_pid = 0;
  if (!session || !in->U64(&compiler_pid) || !in->done() ||
      compiler_pid == 0 || compiler_pid > static_cast<uint64_t>(std::numeric_limits<pid_t>::max())) {
    out->U8(static_cast<uint8_t>(Status::kError));
    out->Str("invalid compiler pid");
    return;
  }
  auto replacement = std::make_shared<MemoryReservation>();
  replacement->compiler_pid = static_cast<pid_t>(compiler_pid);
  const auto status = util::ReadFile("/proc/" + std::to_string(compiler_pid) + "/status");
  const bool visible_child = session->client_pid > 0 && status &&
      StatusNumber(*status, "PPid") == static_cast<uint64_t>(session->client_pid);
  if (visible_child) {
#if defined(__linux__) && defined(SYS_pidfd_open)
    if (!std::getenv("VCACHE_DAEMON_TEST_NO_PIDFD")) {
      replacement->pid_fd = static_cast<int>(::syscall(SYS_pidfd_open, compiler_pid, 0));
    }
#endif
  } else {
    replacement->fallback_started = Clock::now();
    VCACHE_LOG("reserve: " + std::to_string(compiler_pid) +
               " pid not visible as peer child, whole estimate unrealised for 10000 ms");
  }
  std::unique_lock<std::mutex> lock(mutex_);
  const auto found = reservations_.find(session->fd);
  if (found != reservations_.end() && !stop_requested_.load()) {
    replacement->session = found->second->session;
    replacement->cost_key = found->second->cost_key;
    replacement->estimate_kb = found->second->estimate_kb;
    replacement->peak_rss_kb = found->second->peak_rss_kb;
    replacement->rss_sampled = found->second->rss_sampled;
    found->second = std::move(replacement);
    UpdateAdmission(lock, session->fd);
  }
  out->U8(static_cast<uint8_t>(Status::kOk));
}

void Server::AdmissionWorker() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_requested_.load()) {
    UpdateAdmission(lock, std::nullopt, true);
    TickJobserver();
    cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return stop_requested_.load(); });
  }
}

void Server::TickJobserver() {
  if (!jobserver_) return;
  const uint64_t waiting = memory_waiters_.size();
  const uint64_t available_kb = AvailableKb();
  const int change = JobserverSignedTokenChange(jobserver_->total(), jobserver_->fifo_bytes(),
      jobserver_->withdrawn(), waiting, available_kb, jobserver_min_jobs_,
      std::max(config_.daemon.default_compile_kb, config_.daemon.default_link_kb));
  std::string message;
  if (change < 0) {
    const int taken = jobserver_->Withdraw(-change);
    if (taken > 0) message = "jobserver: withdrew " + std::to_string(taken) +
        " tokens (waiting " + std::to_string(waiting) + ", available " +
        std::to_string(available_kb) + " kB)";
  } else if (change > 0) {
    const int returned = jobserver_->Restore(change);
    if (returned > 0) message = "jobserver: restored " + std::to_string(returned) + " tokens";
  }
  if (!message.empty()) {
    log_.Line(message);
  }
}

std::shared_ptr<CacheFetch> Server::FetchKey(const std::string& key) {
  std::shared_ptr<CacheFetch> fetch;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    auto& state = leases_[key];
    fetch = state.fetch;
    if (fetch) {
      const auto started = Clock::now();
      VCACHE_LOG("lease: fetch waiting " + key.substr(0, 16) + " up to " +
                 std::to_string(kReplyTimeoutSeconds * 1000) + " ms");
      fetch->changed.wait_for(lock, std::chrono::seconds(kReplyTimeoutSeconds),
                              [&] { return fetch->done || stop_requested_.load(); });
      VCACHE_LOG("lease: fetch waited " + key.substr(0, 16) + " for " +
                 std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                     Clock::now() - started).count()) + " ms");
      if (fetch->done || stop_requested_.load()) return fetch;
      // An expired fetch wait follows the original independent lookup path.
      fetch.reset();
    } else {
      fetch = std::make_shared<CacheFetch>();
      state.fetch = fetch;
    }
  }
  if (!fetch) fetch = std::make_shared<CacheFetch>();
  auto s3 = AcquireS3();
  fetch->hit = s3->Get(key, &fetch->value);
  if (!fetch->hit && s3->failed()) fetch->errors.push_back("s3: " + s3->last_error());
  if (fetch->hit) {
    auto disk = MakeDisk();
    if (disk && disk->writable() && !disk->Put(key, fetch->value) && disk->failed()) {
      fetch->errors.push_back("disk: " + disk->last_error());
    }
  }
  ReleaseS3(std::move(s3));
  {
    std::lock_guard<std::mutex> lock(mutex_);
    fetch->done = true;
    fetch->changed.notify_all();
    const auto found = leases_.find(key);
    if (found != leases_.end() && found->second.fetch == fetch) {
      found->second.fetch.reset();
      if (!found->second.compile) leases_.erase(found);
    }
  }
  return fetch;
}

void Server::HandleGet(Reader* in, Writer* out) {
  std::string key;
  if (!in->Str(&key) || !ValidKey(key)) {
    out->U8(static_cast<uint8_t>(Status::kError));
    out->Str("invalid key");
    return;
  }
  counters_.lookups++;
  std::vector<std::string> errors;
  auto disk = MakeDisk();

  std::string value;
  std::string layer;
  if (disk != nullptr) {
    if (disk->Get(key, &value)) {
      layer = "disk";
      counters_.hits_disk++;
    } else if (disk->failed()) {
      errors.push_back("disk: " + disk->last_error());
    }
  }
  if (layer.empty()) {
    // Stored moments ago, still waiting for its upload, and no disk layer to
    // have put it in. Serving it is what makes the queue invisible to builds.
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto held = uploads_.Held(key)) {
      value = *held;
      layer = HeldHitLayerName();
      counters_.hits_memory++;
    }
  }
  if (layer.empty() && s3_enabled_) {
    const auto fetch = FetchKey(key);
    if (!fetch->done) {
      out->U8(static_cast<uint8_t>(Status::kError));
      out->Str("daemon shutting down");
      return;
    }
    errors.insert(errors.end(), fetch->errors.begin(), fetch->errors.end());
    if (fetch->hit) {
      value = fetch->value;
      layer = "s3";
      counters_.hits_s3++;
    }
  }

  if (layer.empty()) {
    counters_.misses++;
    out->U8(static_cast<uint8_t>(Status::kMiss));
    out->StrList(errors);
    return;
  }
  out->U8(static_cast<uint8_t>(Status::kOk));
  out->Str(layer);
  out->StrList(errors);
  out->Str(value);
}

bool Server::AwaitTestPut(const std::string& key) {
  const char* fifo_path = std::getenv("VCACHE_DAEMON_TEST_BLOCK_PUT");
  if (!fifo_path || !*fifo_path) return true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (test_put_blocked_) return true;
    test_put_blocked_ = true;
  }
  const int gate_fd = ::open(fifo_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (gate_fd < 0) return false;
  const auto started = Clock::now();
  const auto deadline = started + std::chrono::seconds(5);
  VCACHE_LOG("lease: test Put blocked " + key.substr(0, 16) + " up to 5000 ms");
  char outcome = 0;
  while (!stop_requested_.load() && Clock::now() < deadline) {
    pollfd gate{gate_fd, POLLIN, 0};
    if (::poll(&gate, 1, 20) > 0 && ::read(gate_fd, &outcome, 1) == 1) break;
  }
  ::close(gate_fd);
  VCACHE_LOG("lease: test Put waited " + key.substr(0, 16) + " for " +
             std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                 Clock::now() - started).count()) + " ms");
  return outcome == 's';
}

void Server::HandlePut(Reader* in, Writer* out, pid_t peer_pid) {
  std::string key;
  auto value = std::make_shared<std::string>();
  if (!in->Str(&key) || !ValidKey(key) || !in->Str(value.get())) {
    out->U8(static_cast<uint8_t>(Status::kError));
    out->Str("invalid store request");
    return;
  }
  counters_.stores++;
  std::shared_ptr<LeaseWait> storing_lease;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = leases_.find(key);
    if (found != leases_.end() && found->second.compile) {
      const pid_t holder_pid = found->second.compile->holder_pid;
      if (peer_pid == 0 || holder_pid == 0 || holder_pid == peer_pid) {
        storing_lease = found->second.compile;
        ++storing_lease->stores_in_progress;
      }
    }
  }
  std::vector<std::string> errors;
  bool stored = false;

  const bool test_put_allowed = AwaitTestPut(key);
  auto disk = test_put_allowed ? MakeDisk() :
      std::make_unique<storage::DiskStorage>(config_.disk.dir, config_.disk.max_size, true);
  bool on_disk = false;
  if (disk != nullptr && disk->writable()) {
    on_disk = disk->Put(key, *value);
    if (!on_disk && disk->failed()) errors.push_back("disk: " + disk->last_error());
  }
  stored = on_disk;

  if (s3_writable_ && test_put_allowed) {
    if (on_disk) {
      stored = Enqueue(key, nullptr, /*journal=*/true) || stored;
    } else {
      bool refused_in_flight = false;
      uint64_t refusal_id = 0;
      if (Enqueue(key, value, /*journal=*/false, &refused_in_flight, &refusal_id)) {
        stored = true;
      } else {
        // The memory queue is full: upload here and make this one store wait,
        // rather than grow without bound. The overtaken check and the flight
        // registration share one lock, so a newer store cannot be dequeued
        // between them. Finding the key already in flight waits and retries;
        // uploading beside that flight is how an older value wins.
        bool overtaken = false;
        uint64_t sync_generation = 0;
        bool have_refusal = refused_in_flight;
        while (!overtaken && sync_generation == 0) {
          if (have_refusal) WaitForRefusal(refusal_id, key);
          std::lock_guard<std::mutex> lock(mutex_);
          if (have_refusal) {
            overtaken = uploads_.TakeRefusal(refusal_id);
            have_refusal = false;
          }
          if (overtaken) break;
          sync_generation = uploads_.BeginSync(key);
          if (sync_generation != 0) {
            counters_.uploads_queued++;
            break;
          }
          refusal_id = uploads_.WatchFlight(key);
          if (refusal_id == 0) {
            sync_generation = uploads_.BeginSync(key);
            if (sync_generation != 0) counters_.uploads_queued++;
            break;
          }
          have_refusal = true;
        }
        if (overtaken) {
          stored = true;
        } else if (sync_generation != 0) {
          auto s3 = AcquireS3();
          const bool uploaded = s3->Put(key, *value);
          std::string s3_error;
          if (!uploaded && s3->failed()) s3_error = s3->last_error();
          ReleaseS3(std::move(s3));
          if (uploaded) stored = true;
          else if (!s3_error.empty()) errors.push_back("s3: " + s3_error);
          std::lock_guard<std::mutex> lock(mutex_);
          if (uploaded) {
            counters_.uploads_done++;
            counters_.upload_bytes += value->size();
          } else {
            counters_.uploads_failed++;
          }
          bool count_requeue = false;
          UploadItem finished;
          finished.key = key;
          finished.generation = sync_generation;
          if (uploads_.Finish(finished, &count_requeue)) {
            if (count_requeue) counters_.uploads_queued++;
            log_.Line("re-queued " + key + " (rewritten during upload)");
          }
        }
      }
    }
  }

  if (!stored) counters_.stores_failed++;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = leases_.find(key);
    if (storing_lease) --storing_lease->stores_in_progress;
    if (found != leases_.end() && storing_lease && found->second.compile == storing_lease) {
      CompleteLease(key, stored ? LeaseOutcome::kStored : LeaseOutcome::kCompile,
                     stored ? LeaseCompileReason::kNone : LeaseCompileReason::kHolderFailed);
    }
    cv_.notify_all();
  }
  out->U8(static_cast<uint8_t>(Status::kOk));
  out->U8(stored ? 1 : 0);
  out->StrList(errors);
}

std::string Server::StatusText() {
  auto row = [](const std::string& name, const std::string& value) {
    std::string line = name;
    if (line.size() < 22) line.append(22 - line.size(), ' ');
    else line.push_back(' ');
    return line + value + "\n";
  };
  size_t active = 0;
  size_t compile_sessions = 0;
  size_t leases_held = 0;
  size_t leases_waiting = 0;
  size_t pending = 0;
  size_t superseded = 0;
  uint64_t held = 0;
  uint64_t reserved_kb = 0, realised_kb = 0, memory_waiting = 0, reserve_waits = 0;
  uint64_t longest_reserve_wait_ms = 0;
  uint64_t uploads_queued = 0, uploads_done = 0, uploads_failed = 0, uploads_skipped = 0;
  uint64_t upload_bytes = 0, uploads_recovered = 0;
  uint64_t connections = 0, refused = 0, lookups = 0, hits_disk = 0, hits_memory = 0;
  uint64_t hits_s3 = 0, misses = 0, stores = 0, stores_failed = 0;
  uint64_t leases_expired = 0, compiles_deduplicated = 0;
  std::string jobserver_status;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    connections = counters_.connections.load();
    refused = counters_.refused.load();
    lookups = counters_.lookups.load();
    hits_disk = counters_.hits_disk.load();
    hits_memory = counters_.hits_memory.load();
    hits_s3 = counters_.hits_s3.load();
    misses = counters_.misses.load();
    stores = counters_.stores.load();
    stores_failed = counters_.stores_failed.load();
    uploads_queued = counters_.uploads_queued.load();
    uploads_done = counters_.uploads_done.load();
    uploads_failed = counters_.uploads_failed.load();
    uploads_skipped = counters_.uploads_skipped.load();
    upload_bytes = counters_.upload_bytes.load();
    uploads_recovered = counters_.uploads_recovered.load();
    leases_expired = counters_.leases_expired.load();
    compiles_deduplicated = counters_.compiles_deduplicated.load();
    active = connections_.size();
    compile_sessions = sessions_.size();
    for (const auto& [fd, reservation] : reservations_) {
      reserved_kb = SaturatingAddKb(reserved_kb, reservation->estimate_kb);
      realised_kb = SaturatingAddKb(realised_kb, reservation->realised_kb);
    }
    memory_waiting = memory_waiters_.size();
    reserve_waits = reserve_waits_;
    longest_reserve_wait_ms = longest_reserve_wait_ms_;
    if (jobserver_) {
      const int free = jobserver_->free_tokens();
      jobserver_status += row("jobserver tokens total", std::to_string(jobserver_->total()));
      jobserver_status += row("jobserver tokens free",
                             free < 0 ? std::string("unknown") : std::to_string(free));
      jobserver_status += row("jobserver tokens withdrawn", std::to_string(jobserver_->withdrawn()));
      jobserver_status += row("jobserver tokens restored total",
                             std::to_string(jobserver_->restored_total()));
    }
    for (const auto& [key, state] : leases_) {
      if (state.compile) {
        ++leases_held;
        leases_waiting += state.compile->waiters.size();
      }
    }
    superseded = uploads_.superseded_count();
    pending = uploads_.size() + uploads_.in_flight_count() - superseded;
    held = uploads_.held_bytes();
  }
  const auto uptime =
      std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started_).count();
  std::string s;
  s += row("daemon pid", std::to_string(::getpid()));
  s += row("socket", socket_path_);
  s += row("uptime", std::to_string(uptime) + " s");
  s += row("idle timeout", config_.daemon.idle_timeout_seconds > 0
                               ? std::to_string(config_.daemon.idle_timeout_seconds) + " s"
                               : std::string("none"));
  s += row("s3 layer", !s3_enabled_ ? "off" : (s3_writable_ ? "read-write" : "read-only"));
  s += row("connections", std::to_string(connections) + " (" + std::to_string(active) +
                              " open, " + std::to_string(refused) + " refused)");
  s += row("compile sessions", std::to_string(compile_sessions));
  s += row("leases held", std::to_string(leases_held));
  s += row("leases waiting", std::to_string(leases_waiting));
  s += row("leases expired", std::to_string(leases_expired));
  s += row("compiles deduplicated", std::to_string(compiles_deduplicated));
  s += row("memory reserved", std::to_string(reserved_kb));
  s += row("memory realised", std::to_string(realised_kb));
  s += row("memory waiting", std::to_string(memory_waiting));
  s += row("reserve waits", std::to_string(reserve_waits));
  s += row("longest wait ms", std::to_string(longest_reserve_wait_ms));
  s += row("lookups", std::to_string(lookups));
  s += row("  hit (disk)", std::to_string(hits_disk));
  s += row("  hit (memory)", std::to_string(hits_memory));
  s += row("  hit (s3)", std::to_string(hits_s3));
  s += row("  miss", std::to_string(misses));
  s += row("stores", std::to_string(stores));
  s += row("  failed", std::to_string(stores_failed));
  s += row("uploads queued", std::to_string(uploads_queued));
  s += row("  recovered", std::to_string(uploads_recovered));
  s += row("  completed", std::to_string(uploads_done));
  s += row("  bytes", std::to_string(upload_bytes));
  s += row("  failed", std::to_string(uploads_failed));
  s += row("  skipped", std::to_string(uploads_skipped));
  s += row("  pending", std::to_string(pending));
  s += row("  held in memory", std::to_string(held) + " bytes");
  s += row("uploads superseded", std::to_string(superseded));
  s += jobserver_status;
  return s;
}

void Server::HandleShutdown(Writer* out) {
  std::unique_lock<std::mutex> lock(mutex_);
  ++shutdown_waiters_;
  stop_requested_.store(true);
  Wake();
  cv_.notify_all();
  cv_.wait(lock, [this] { return drained_; });
  --shutdown_waiters_;
  out->U8(static_cast<uint8_t>(Status::kOk));
  out->U64(counters_.uploads_failed.load());
  out->Str(drain_summary_);
}

void Server::Serve(int fd) {
  std::string request;
  std::string reply;
  bool ok = RecvFrame(fd, &request) && HandleHello(request, &reply);
  SendFrame(fd, reply);
  bool first_request = true;
  pid_t peer_pid = 0;
#if defined(__linux__)
  struct ucred peer_credentials;
  socklen_t peer_size = sizeof(peer_credentials);
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer_credentials, &peer_size) == 0) {
    peer_pid = peer_credentials.pid;
  }
#endif
  std::shared_ptr<CompileSession> compile_session;

  while (ok && RecvFrame(fd, &request)) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      last_activity_ = Clock::now();
    }
    Reader in(request);
    Writer out;
    uint8_t op = 0;
    in.U8(&op);
    if (compile_session && op != static_cast<uint8_t>(Op::kSessionOpen) &&
        op != static_cast<uint8_t>(Op::kLeaseAcquire) &&
        op != static_cast<uint8_t>(Op::kLeaseRelease) &&
        op != static_cast<uint8_t>(Op::kMemoryReserve) &&
        op != static_cast<uint8_t>(Op::kCompilerSpawned)) {
      out.U8(static_cast<uint8_t>(Status::kError));
      out.Str("unknown compile session request");
      compile_session->Reply(out.data());
      break;
    }
    switch (static_cast<Op>(op)) {
      case Op::kSessionOpen: {
        std::lock_guard<std::mutex> lock(mutex_);
        pid_t client_pid = 0;
#if defined(__linux__)
        struct ucred credentials;
        socklen_t credentials_size = sizeof(credentials);
        if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials,
                         &credentials_size) == 0) client_pid = credentials.pid;
#endif
        bool pid_already_open = false;
        for (const auto& [session_fd, session] : sessions_) {
          if (client_pid > 0 && session->client_pid == client_pid) pid_already_open = true;
        }
        if (!first_request || !in.done() || stop_requested_.load() || pid_already_open) {
          out.U8(static_cast<uint8_t>(Status::kError));
          out.Str(stop_requested_.load() ? "daemon shutting down" :
                  pid_already_open ? "pid already holds a compile session" :
                                     "session open requires a fresh connection");
        } else {
          compile_session = std::make_shared<CompileSession>(fd, client_pid);
          sessions_.emplace(fd, compile_session);
          timeval send_timeout{1, 0};
          ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
          out.U8(static_cast<uint8_t>(Status::kOk));
          VCACHE_LOG("session: opened for pid " + std::to_string(client_pid));
        }
        break;
      }
      case Op::kGet: HandleGet(&in, &out); break;
      case Op::kPut: HandlePut(&in, &out, peer_pid); break;
      case Op::kLeaseAcquire: HandleLeaseAcquire(&in, &out, compile_session); break;
      case Op::kLeaseRelease: HandleLeaseRelease(&in, &out, compile_session); break;
      case Op::kMemoryReserve: HandleMemoryReserve(&in, &out, compile_session); break;
      case Op::kCompilerSpawned: HandleCompilerSpawned(&in, &out, compile_session); break;
      case Op::kStatus:
        out.U8(static_cast<uint8_t>(Status::kOk));
        out.Str(StatusText());
        break;
      case Op::kShutdown: HandleShutdown(&out); break;
      default:
        out.U8(static_cast<uint8_t>(Status::kError));
        out.Str("unknown request");
        break;
    }
    request.clear();
    request.shrink_to_fit();
    first_request = false;
    if (compile_session) {
      if (!compile_session->Reply(out.data())) break;
    } else if (!SendFrame(fd, out.data())) {
      break;
    }
  }

  std::unique_lock<std::mutex> lock(mutex_);
  std::string reservation_log;
  const auto session = sessions_.find(fd);
  if (session != sessions_.end()) {
    ExpireLeases(session->second);
    const auto reservation = reservations_.find(fd);
    if (reservation != reservations_.end()) {
      RememberTreePeak(*reservation->second);
      reservation_log = ReservationReleaseLog(*reservation->second);
    }
    reservations_.erase(fd);
    std::erase_if(memory_waiters_, [fd](const auto& waiter) {
      return waiter->session->fd == fd;
    });
    GrantMemoryWaiters();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - session->second->opened_at).count();
    VCACHE_LOG("session: closed after " + std::to_string(elapsed_ms) + " ms for pid " +
               std::to_string(session->second->client_pid));
    sessions_.erase(session);
  }
  connections_.erase(fd);
  if (!compile_session) ::close(fd);
  last_activity_ = Clock::now();
  cv_.notify_all();
  lock.unlock();
  if (!reservation_log.empty()) VCACHE_LOG(reservation_log);
}

int Server::IdleLimitSeconds() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (config_.daemon.idle_timeout_seconds <= 0) return 0;
  // A build blocked in read() on the fifo is not a socket client, so the
  // ordinary idle clock would exit under it and take the tokens with it.
  // One extra timeout is enough to tell "the build went quiet" from "the
  // build is still holding slots".
  const int periods =
      (jobserver_ && jobserver_->free_tokens() + jobserver_->withdrawn() <
                         jobserver_->total()) ? 2 : 1;
  return config_.daemon.idle_timeout_seconds * periods;
}

bool Server::Idle() const {
  const int limit_seconds = IdleLimitSeconds();
  if (limit_seconds <= 0) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!connections_.empty() || !uploads_.empty() || uploads_.in_flight_count() != 0) {
    return false;
  }
  return Clock::now() - last_activity_ > std::chrono::seconds(limit_seconds);
}

// Only the user the daemon runs as may talk to it. The directory permissions
// already say so; this is the check that does not depend on them.
bool PeerIsSelf(int fd) {
#if defined(__linux__)
  struct ucred cred;
  socklen_t len = sizeof(cred);
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return false;
  return cred.uid == ::getuid();
#else
  uid_t uid = 0;
  gid_t gid = 0;
  if (::getpeereid(fd, &uid, &gid) != 0) return false;
  return uid == ::getuid();
#endif
}

void Server::Shutdown() {
  std::vector<std::shared_ptr<CompileSession>> closing_sessions;
  std::vector<std::string> reservation_logs;
  std::thread admission_worker;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_.store(true);
    admission_worker = std::move(admission_worker_);
    for (const auto& waiter : memory_waiters_) {
      EndMemoryQueue(waiter);
      waiter->changed.notify_all();
    }
    for (const auto& [fd, reservation] : reservations_) {
      reservation_logs.push_back(ReservationReleaseLog(*reservation));
    }
    reservations_.clear();
    memory_waiters_.clear();
    cv_.notify_all();
    if (jobserver_) {
      const int returned = jobserver_->Restore(jobserver_->withdrawn());
      if (returned > 0) {
        const std::string message = "jobserver: restored " + std::to_string(returned) + " tokens";
        log_.Line(message);
      }
      const std::string path = jobserver_->path();
      jobserver_.reset();
      log_.Line("jobserver: removed " + path);
    }
    for (auto& [key, state] : leases_) {
      if (state.compile) {
        state.compile->outcome = LeaseOutcome::kCompile;
        state.compile->reason = LeaseCompileReason::kShutdown;
        state.compile->changed.notify_all();
      }
      if (state.fetch) state.fetch->changed.notify_all();
    }
    leases_.clear();
    for (const auto& [session_fd, session] : sessions_) {
      session->closing.store(true);
      closing_sessions.push_back(session);
    }
  }
  ::close(listen_fd_);
  listen_fd_ = -1;
  struct stat st;
  if (::lstat(socket_path_.c_str(), &st) == 0 && st.st_ino == socket_inode_) {
    ::unlink(socket_path_.c_str());
  }

  for (const auto& session : closing_sessions) session->Shutdown();
  closing_sessions.clear();
  for (const auto& message : reservation_logs) VCACHE_LOG(message);
  if (admission_worker.joinable()) admission_worker.join();
  if (std::getenv("VCACHE_DAEMON_TEST_RESERVE_GATE")) {
    VCACHE_LOG("reserve: test sampler joined during shutdown");
  }
  std::unique_lock<std::mutex> lock(mutex_);
  // Let compiles that are mid-request finish; a stuck one is cut off rather
  // than allowed to hold the daemon open. The shutdown requesters are
  // connections too, and they are waiting on us.
  auto others = [this] {
    return connections_.size() - sessions_.size() - static_cast<size_t>(shutdown_waiters_);
  };
  if (!cv_.wait_for(lock, std::chrono::seconds(60), [&] { return others() == 0; })) {
    log_.Line("closing " + std::to_string(others()) + " connections still open");
    for (int fd : connections_) ::shutdown(fd, SHUT_RDWR);
    cv_.wait_for(lock, std::chrono::seconds(5), [&] { return others() == 0; });
  }

  const size_t pending = uploads_.size() + uploads_.in_flight_count();
  if (pending > 0) log_.Line("draining " + std::to_string(pending) + " uploads");
  workers_stop_ = true;
  cv_.notify_all();
  lock.unlock();
  for (auto& t : workers_) t.join();
  workers_.clear();

  lock.lock();
  drain_summary_ = "uploaded " + std::to_string(counters_.uploads_done.load()) +
                   ", failed " + std::to_string(counters_.uploads_failed.load()) +
                   ", skipped " + std::to_string(counters_.uploads_skipped.load());
  drained_ = true;
  cv_.notify_all();
  cv_.wait_for(lock, std::chrono::seconds(5), [this] { return connections_.empty(); });
  lock.unlock();

  log_.Line("stopped: " + drain_summary_);
  ::unlink((state_dir_ + "/pid").c_str());
  if (lock_fd_ >= 0) ::close(lock_fd_);

  // A connection thread still running here is stuck in a request it could
  // not be cut out of. It is detached and uses this object, so returning --
  // and destroying it -- is not safe; ending the process is.
  lock.lock();
  if (!connections_.empty()) {
    log_.Line(std::to_string(connections_.size()) +
              " connections still busy at exit; exiting without waiting");
    ::_exit(kServerOk);
  }
}

void SignalReady(int* ready_fd, char code, const std::string& message) {
  if (*ready_fd < 0) return;
  std::string payload(1, code);
  payload += message;
  ssize_t ignored = ::write(*ready_fd, payload.data(), payload.size());
  (void)ignored;
  ::close(*ready_fd);
  *ready_fd = -1;
}

void Server::StartJobserver() {
  if (!config_.daemon.jobserver) return;
  int jobs = config_.daemon.jobserver_jobs;
  if (jobs <= 0) {
    const long online = ::sysconf(_SC_NPROCESSORS_ONLN);
    jobs = online > 0 ? static_cast<int>(online) : 1;
  }
  std::string why;
  auto pool = JobserverPool::Open(state_dir_ + "/jobserver.fifo", jobs, &why);
  if (!pool) {
    log_.Line("jobserver: not started (" + why + ")");
    return;
  }
  log_.Line("jobserver: pool " + pool->path() + " with " + std::to_string(pool->total()) +
            " slots (" + std::to_string(pool->total() - 1) + " tokens)");
  jobserver_ = std::make_unique<JobserverPool>(std::move(*pool));
  jobserver_min_jobs_ = static_cast<int>(std::min<uint64_t>(config_.daemon.jobserver_min_jobs,
                                                         jobserver_->total()));
  if (config_.daemon.jobserver_min_jobs > static_cast<uint64_t>(jobserver_->total())) {
    const std::string message = "jobserver: jobserver_min_jobs exceeds pool, clamped to " +
                                std::to_string(jobserver_min_jobs_);
    log_.Line(message);
  }
  admission_worker_ = std::thread([this] { AdmissionWorker(); });
}

int Server::Run(int ready_fd) {
  std::string error;
  if (!PrepareStateDir(&error)) {
    SignalReady(&ready_fd, 'E', error);
    return kServerFailed;
  }
  if (!AcquireLock(&error)) {
    if (error.empty()) {
      SignalReady(&ready_fd, 'L', "");
      return kServerAlreadyRunning;
    }
    SignalReady(&ready_fd, 'E', error);
    return kServerFailed;
  }
  if (!Listen(&error)) {
    log_.Line("failed to start: " + error);
    SignalReady(&ready_fd, 'E', error);
    return kServerFailed;
  }
  int wake[2];
  if (::pipe(wake) != 0) {
    error = std::string("pipe: ") + std::strerror(errno);
    log_.Line("failed to start: " + error);
    SignalReady(&ready_fd, 'E', error);
    return kServerFailed;
  }
  for (int fd : wake) {
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
  }
  g_wake_fd = wake[1];

  SetUpS3();
  RecoverJournal();
  StartJobserver();
  log_.Line("listening on " + socket_path_);
  SignalReady(&ready_fd, 'R', "");

  while (!g_signalled.load() && !stop_requested_.load()) {
    // The timeout only paces the idle check; requests and wake-ups end the
    // wait immediately.
    pollfd pfds[2] = {{listen_fd_, POLLIN, 0}, {wake[0], POLLIN, 0}};
    const int rc = ::poll(pfds, 2, 1000);
    if (rc < 0 && errno != EINTR) {
      log_.Line(std::string("poll: ") + std::strerror(errno));
      break;
    }
    if (rc > 0 && (pfds[1].revents & POLLIN)) {
      char drain[64];
      while (::read(wake[0], drain, sizeof(drain)) > 0) {
      }
      continue;
    }
    pollfd& pfd = pfds[0];
    if (rc > 0 && (pfd.revents & POLLIN)) {
      const int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) continue;
      ::fcntl(fd, F_SETFD, FD_CLOEXEC);
      if (!PeerIsSelf(fd)) {
        log_.Line("rejected a connection from another user");
        ::close(fd);
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connections_.size() >= kMaxConnections) {
          ::close(fd);
          continue;
        }
        connections_.insert(fd);
        last_activity_ = Clock::now();
      }
      counters_.connections++;
      std::thread([this, fd] { Serve(fd); }).detach();
      continue;
    }
    // Someone deleted the cache directory, or another daemon took the path
    // over. Either way no client can reach this one any more, and lingering
    // until the idle timeout would only hold its credentials and threads.
    struct stat st;
    if (::lstat(socket_path_.c_str(), &st) != 0 || st.st_ino != socket_inode_) {
      log_.Line("socket " + socket_path_ + " was removed or replaced; exiting");
      break;
    }
    if (Idle()) {
      log_.Line("idle for " + std::to_string(IdleLimitSeconds()) + " s; exiting");
      break;
    }
  }
  if (g_signalled.load()) log_.Line("signalled; shutting down");
  Shutdown();
  g_wake_fd = -1;
  ::close(wake[0]);
  ::close(wake[1]);
  return kServerOk;
}

// Closes every inherited descriptor except `keep`. A daemon started from inside
// a build inherits that build's pipes -- make's jobserver, the stdout a CI
// runner is waiting to see closed -- and holding them would make the build
// appear never to finish.
void CloseInheritedFds(int keep) {
  struct rlimit rl;
  int max_fd = 1024;
  if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
    max_fd = static_cast<int>(std::min<rlim_t>(rl.rlim_cur, 65536));
  }
  for (int fd = 3; fd < max_fd; ++fd) {
    if (fd != keep) ::close(fd);
  }
}

bool WaitForSocket(const std::string& path, int timeout_ms) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) return false;
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (Clock::now() < deadline) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
      const bool ok =
          ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
      ::close(fd);
      if (ok) return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

}  // namespace

int RunServer(const core::Config& config, int ready_fd) {
  struct sigaction sa{};
  sa.sa_handler = OnTerminate;
  sigemptyset(&sa.sa_mask);
  ::sigaction(SIGTERM, &sa, nullptr);
  ::sigaction(SIGINT, &sa, nullptr);
  ::signal(SIGPIPE, SIG_IGN);
  ::signal(SIGHUP, SIG_IGN);

  Server server(config);
  return server.Run(ready_fd);
}

int StartDetached(const core::Config& config, std::string* error) {
  int fds[2];
  if (::pipe(fds) != 0) {
    *error = std::string("pipe: ") + std::strerror(errno);
    return kServerFailed;
  }
  const std::string self = util::SelfPath().value_or("");
  // Resolved here, while the caller's working directory is still current.
  core::Config absolute = config;
  absolute.disk.dir = util::AbsoluteLexical(config.disk.dir);
  if (!config.daemon.socket.empty()) {
    absolute.daemon.socket = util::AbsoluteLexical(config.daemon.socket);
  }
  if (!config.loaded_from.empty()) {
    absolute.loaded_from = util::AbsoluteLexical(config.loaded_from);
  }
  std::string log_path;
  if (const char* log = std::getenv("VCACHE_LOG")) {
    if (*log != '\0' && std::string(log) != "stderr") log_path = util::AbsoluteLexical(log);
  }

  const pid_t child = ::fork();
  if (child < 0) {
    *error = std::string("fork: ") + std::strerror(errno);
    ::close(fds[0]);
    ::close(fds[1]);
    return kServerFailed;
  }
  if (child == 0) {
    // Detach fully: a new session so the build's terminal and process group
    // signals do not reach it, and a second fork so it is reparented to init
    // instead of lingering as this process's child.
    ::close(fds[0]);
    ::setsid();
    if (::fork() != 0) ::_exit(0);
    const int null_fd = ::open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
      ::dup2(null_fd, 0);
      ::dup2(null_fd, 1);
      ::dup2(null_fd, 2);
      if (null_fd > 2) ::close(null_fd);
    }
    CloseInheritedFds(fds[1]);
    // The daemon runs from "/", and the exec'd one rebuilds its configuration
    // from the environment. A relative cache directory, config file or log
    // path would then name a different file -- and a different cache dir is a
    // daemon that refuses every client that started it.
    ::setenv("VCACHE_DIR", absolute.disk.dir.c_str(), 1);
    if (!absolute.daemon.socket.empty()) {
      ::setenv("VCACHE_DAEMON_SOCKET", absolute.daemon.socket.c_str(), 1);
    }
    if (!absolute.loaded_from.empty()) {
      ::setenv("VCACHE_CONFIG", absolute.loaded_from.c_str(), 1);
    }
    if (!log_path.empty()) ::setenv("VCACHE_LOG", log_path.c_str(), 1);
    if (::chdir("/") != 0) ::_exit(kServerFailed);
    if (!self.empty()) {
      // Exec'd afresh rather than run in this copy of the parent, so the daemon
      // starts from a clean image and shows up in `ps` as what it is.
      const std::string fd_env = std::to_string(fds[1]);
      ::setenv("VCACHE_DAEMON_READY_FD", fd_env.c_str(), 1);
      ::execl(self.c_str(), self.c_str(), "--daemon-foreground",
              static_cast<char*>(nullptr));
    }
    ::_exit(RunServer(absolute, fds[1]));
  }

  ::close(fds[1]);
  int status = 0;
  ::waitpid(child, &status, 0);

  std::string reply;
  char buf[512];
  for (;;) {
    const ssize_t n = ::read(fds[0], buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    reply.append(buf, static_cast<size_t>(n));
  }
  ::close(fds[0]);

  if (reply.empty()) {
    *error = "the daemon exited before it was ready (see " + StateDir(config) + "/log)";
    return kServerFailed;
  }
  switch (reply[0]) {
    case 'R': return kServerOk;
    case 'L':
      // Another daemon holds the lock. It may still be starting, so give it a
      // moment to listen before deciding it is not there.
      if (WaitForSocket(SocketPath(config), 5000)) return kServerAlreadyRunning;
      *error = "another daemon holds " + StateDir(config) + "/lock but is not listening";
      return kServerFailed;
    default:
      *error = reply.substr(1);
      return kServerFailed;
  }
}

}  // namespace vcache::daemon
