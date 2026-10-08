// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "daemon/client.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>

#include "daemon/protocol.h"
#include "daemon/server.h"
#include "util/fs.h"
#include "util/log.h"

namespace vcache::daemon {
namespace {

// After a failed auto-start, compiles in the next minute do not try again. A
// cache directory the daemon cannot use would otherwise cost every compile in
// a parallel build a fork, an exec and a failure.
constexpr std::time_t kStartRetrySeconds = 60;

}  // namespace

DaemonClient::~DaemonClient() { Close(); }

void DaemonClient::Close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

bool DaemonClient::Open(std::string* why) {
  Close();
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socket_path_.size() >= sizeof(addr.sun_path)) {
    *why = "socket path too long: " + socket_path_;
    return false;
  }
  std::memcpy(addr.sun_path, socket_path_.c_str(), socket_path_.size() + 1);

  fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd_ < 0) {
    *why = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  // Not inherited by the compiler vcache is about to run.
  ::fcntl(fd_, F_SETFD, FD_CLOEXEC);
  if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    *why = "no daemon at " + socket_path_ + " (" + std::strerror(errno) + ")";
    Close();
    return false;
  }
  timeval tv{};
  tv.tv_sec = kReplyTimeoutSeconds;
  ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  Writer hello;
  hello.U8(static_cast<uint8_t>(Op::kHello));
  hello.U64(kProtocolVersion);
  hello.Str(fingerprint_);
  std::string reply;
  if (!SendFrame(fd_, hello.data()) || !RecvFrame(fd_, &reply)) {
    *why = "daemon at " + socket_path_ + " did not answer";
    Close();
    return false;
  }
  Reader in(reply);
  uint8_t status = 0;
  in.U8(&status);
  if (status != static_cast<uint8_t>(Status::kOk)) {
    std::string reason;
    in.Str(&reason);
    *why = "daemon refused this client: " + reason;
    Close();
    return false;
  }
  in.U64(&daemon_pid_);
  return true;
}

std::unique_ptr<DaemonClient> DaemonClient::Connect(const core::Config& config,
                                                    std::string* why) {
  std::unique_ptr<DaemonClient> client(new DaemonClient());
  client->config_ = config;
  client->socket_path_ = SocketPath(config);
  client->fingerprint_ = ConfigFingerprint(config);
  if (!client->Open(why)) return nullptr;
  return client;
}

bool DaemonClient::RoundTrip(const std::string& request, std::string* reply,
                             int timeout_seconds) {
  // The connection made by Connect() serves the first request; every later
  // one reconnects.
  if (fd_ < 0) {
    std::string why;
    if (!Open(&why)) {
      VCACHE_LOG(why);
      return false;
    }
  }
  timeval tv{};
  tv.tv_sec = timeout_seconds;
  ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  const bool ok = SendFrame(fd_, request) && RecvFrame(fd_, reply);
  Close();
  return ok;
}

bool DaemonClient::Get(const std::string& key, storage::GetResult* result) {
  Writer req;
  req.U8(static_cast<uint8_t>(Op::kGet));
  req.Str(key);
  std::string reply;
  if (!RoundTrip(req.data(), &reply, kReplyTimeoutSeconds)) return false;

  Reader in(reply);
  uint8_t status = 0;
  if (!in.U8(&status)) return false;
  if (status == static_cast<uint8_t>(Status::kMiss)) {
    return in.StrList(&result->errors) && in.done();
  }
  if (status != static_cast<uint8_t>(Status::kOk)) return false;
  if (!in.Str(&result->layer) || !in.StrList(&result->errors) ||
      !in.Str(&result->value) || !in.done()) {
    return false;
  }
  result->hit = true;
  return true;
}

bool DaemonClient::Put(const std::string& key, const std::string& value,
                       storage::PutResult* result) {
  Writer req;
  req.U8(static_cast<uint8_t>(Op::kPut));
  req.Str(key);
  req.Str(value);
  std::string reply;
  if (!RoundTrip(req.data(), &reply, kReplyTimeoutSeconds)) return false;

  Reader in(reply);
  uint8_t status = 0;
  uint8_t stored = 0;
  if (!in.U8(&status) || status != static_cast<uint8_t>(Status::kOk)) return false;
  if (!in.U8(&stored) || !in.StrList(&result->errors) || !in.done()) return false;
  result->stored = stored != 0;
  return true;
}

bool DaemonClient::Status(std::string* text) {
  Writer req;
  req.U8(static_cast<uint8_t>(Op::kStatus));
  std::string reply;
  if (!RoundTrip(req.data(), &reply, kReplyTimeoutSeconds)) return false;
  Reader in(reply);
  uint8_t status = 0;
  return in.U8(&status) && status == static_cast<uint8_t>(Status::kOk) && in.Str(text);
}

bool DaemonClient::Shutdown(std::string* summary, uint64_t* failed_uploads) {
  Writer req;
  req.U8(static_cast<uint8_t>(Op::kShutdown));
  std::string reply;
  // No timeout: the reply comes when the upload queue has drained, which takes
  // as long as the queue is.
  if (!RoundTrip(req.data(), &reply, /*timeout_seconds=*/0)) return false;
  Reader in(reply);
  uint8_t status = 0;
  return in.U8(&status) && status == static_cast<uint8_t>(Status::kOk) &&
         in.U64(failed_uploads) && in.Str(summary);
}

std::unique_ptr<DaemonClient> ConnectForCompile(const core::Config& config) {
  std::string why;
  auto client = DaemonClient::Connect(config, &why);
  if (client != nullptr) return client;
  VCACHE_LOG(why);
  if (config.daemon.mode != core::DaemonMode::kAuto) return nullptr;
  // A refusal means a daemon is running and serving a different cache; starting
  // another would only fail on its lock or, worse, fight it for the socket.
  if (why.find("refused") != std::string::npos) return nullptr;

  const std::string marker = StateDir(config) + "/start-failed";
  if (auto mtime = util::FileMtime(marker)) {
    if (std::time(nullptr) - *mtime / 1000000000 < kStartRetrySeconds) {
      VCACHE_LOG("daemon start failed recently; not retrying yet");
      return nullptr;
    }
  }

  std::string error;
  const int rc = StartDetached(config, &error);
  if (rc != kServerOk && rc != kServerAlreadyRunning) {
    VCACHE_LOG("could not start daemon: " + error);
    if (util::MakeDirs(StateDir(config))) util::WriteFileAtomic(marker, error + "\n");
    return nullptr;
  }
  VCACHE_LOG("started daemon");
  client = DaemonClient::Connect(config, &why);
  if (client == nullptr) VCACHE_LOG(why);
  return client;
}

}  // namespace vcache::daemon
