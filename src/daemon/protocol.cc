// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "daemon/protocol.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <map>

#include "hash/hasher.h"
#include "util/fs.h"
#include "util/str.h"

namespace vcache::daemon {

void Writer::U64(uint64_t v) {
  for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<char>(v >> (8 * i)));
}

void Writer::Str(const std::string& s) {
  U64(s.size());
  buf_.append(s);
}

void Writer::StrList(const std::vector<std::string>& list) {
  U64(list.size());
  for (const std::string& s : list) Str(s);
}

bool Reader::U8(uint8_t* v) {
  if (pos_ + 1 > data_.size()) {
    pos_ = data_.size() + 1;  // poisoned: done() can never be true again
    return false;
  }
  *v = static_cast<uint8_t>(data_[pos_++]);
  return true;
}

bool Reader::U64(uint64_t* v) {
  if (pos_ + 8 > data_.size()) {
    pos_ = data_.size() + 1;
    return false;
  }
  uint64_t out = 0;
  for (int i = 0; i < 8; ++i) {
    out |= static_cast<uint64_t>(static_cast<unsigned char>(data_[pos_ + i])) << (8 * i);
  }
  pos_ += 8;
  *v = out;
  return true;
}

bool Reader::Str(std::string* s) {
  uint64_t len = 0;
  if (!U64(&len)) return false;
  if (len > data_.size() - pos_) {
    pos_ = data_.size() + 1;
    return false;
  }
  s->assign(data_, pos_, len);
  pos_ += len;
  return true;
}

bool Reader::StrList(std::vector<std::string>* list) {
  uint64_t n = 0;
  if (!U64(&n)) return false;
  // Each element costs at least its 8-byte length, which bounds a corrupt
  // count before it can drive a huge reserve.
  if (n > (data_.size() - pos_) / 8) {
    pos_ = data_.size() + 1;
    return false;
  }
  list->clear();
  for (uint64_t i = 0; i < n; ++i) {
    std::string s;
    if (!Str(&s)) return false;
    list->push_back(std::move(s));
  }
  return true;
}

namespace {

bool WriteAll(int fd, const char* data, size_t len) {
  while (len > 0) {
    // MSG_NOSIGNAL where it exists; the daemon also ignores SIGPIPE, and a
    // compile never outlives a broken daemon connection long enough to care.
#ifdef MSG_NOSIGNAL
    const ssize_t n = ::send(fd, data, len, MSG_NOSIGNAL);
#else
    const ssize_t n = ::write(fd, data, len);
#endif
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    data += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

bool ReadAll(int fd, char* data, size_t len) {
  while (len > 0) {
    const ssize_t n = ::read(fd, data, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;  // peer closed mid-frame
    data += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

bool SendFrame(int fd, const std::string& body) {
  char header[8];
  const uint64_t len = body.size();
  for (int i = 0; i < 8; ++i) header[i] = static_cast<char>(len >> (8 * i));
  return WriteAll(fd, header, sizeof(header)) && WriteAll(fd, body.data(), body.size());
}

bool RecvFrame(int fd, std::string* body) {
  unsigned char header[8];
  if (!ReadAll(fd, reinterpret_cast<char*>(header), sizeof(header))) return false;
  uint64_t len = 0;
  for (int i = 0; i < 8; ++i) len |= static_cast<uint64_t>(header[i]) << (8 * i);
  if (len > kMaxFrame) return false;
  body->resize(len);
  return ReadAll(fd, body->data(), len);
}

std::string StateDir(const core::Config& config) {
  return util::AbsoluteLexical(config.disk.dir) + "/daemon";
}

std::string SocketPath(const core::Config& config) {
  if (!config.daemon.socket.empty()) return util::AbsoluteLexical(config.daemon.socket);
  const std::string preferred = StateDir(config) + "/sock";
  if (preferred.size() < sizeof(sockaddr_un{}.sun_path)) return preferred;
  // Keyed by the cache directory so two caches still get two daemons, and by
  // uid so two users on one machine cannot collide in the shared /tmp.
  const std::string digest = hash::HashString(util::AbsoluteLexical(config.disk.dir));
  return "/tmp/vcache-" + std::to_string(::getuid()) + "/" + digest.substr(0, 16) +
         ".sock";
}

std::string ConfigFingerprint(const core::Config& config) {
  std::string out;
  auto line = [&out](const std::string& name, const std::string& value) {
    out += name + ": " + value + "\n";
  };
  line("disk.enabled", config.disk.enabled ? "yes" : "no");
  line("disk.dir", util::AbsoluteLexical(config.disk.dir));
  line("disk.size", std::to_string(config.disk.max_size));
  line("read_only", config.read_only ? "yes" : "no");
  line("s3.enabled", config.s3.enabled ? "yes" : "no");
  if (config.s3.enabled) {
    line("s3.bucket", config.s3.bucket);
    line("s3.region", config.s3.region);
    line("s3.prefix", config.s3.prefix);
    line("s3.endpoint", config.s3.endpoint);
    line("s3.path_style", config.s3.use_path_style ? "yes" : "no");
    line("s3.ttl_days", std::to_string(config.s3.ttl_days));
    std::string identity = "anonymous";
    if (!config.s3.no_credentials) {
      identity = config.s3.access_key.empty()
                     ? "none"
                     : hash::HashString(config.s3.access_key).substr(0, 16);
    }
    line("s3.identity", identity);
  }
  return out;
}

std::string FingerprintMismatch(const std::string& ours, const std::string& theirs) {
  auto parse = [](const std::string& text) {
    std::vector<std::pair<std::string, std::string>> fields;
    for (const std::string& l : util::Split(text, '\n', /*skip_empty=*/true)) {
      const size_t colon = l.find(": ");
      if (colon == std::string::npos) {
        fields.emplace_back(l, "");
      } else {
        fields.emplace_back(l.substr(0, colon), l.substr(colon + 2));
      }
    }
    return fields;
  };
  const auto a = parse(ours);
  const auto b = parse(theirs);
  std::map<std::string, std::string> bm(b.begin(), b.end());
  for (const auto& [name, value] : a) {
    auto it = bm.find(name);
    const std::string other = (it == bm.end()) ? "(unset)" : it->second;
    if (other != value) {
      return name + ": daemon has '" + value + "', client has '" + other + "'";
    }
  }
  std::map<std::string, std::string> am(a.begin(), a.end());
  for (const auto& [name, value] : b) {
    if (am.find(name) == am.end()) {
      return name + ": daemon has (unset), client has '" + value + "'";
    }
  }
  return "";
}

}  // namespace vcache::daemon
