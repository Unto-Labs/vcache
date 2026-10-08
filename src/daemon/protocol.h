// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// Wire protocol between a compile and the cache daemon.
//
// Every message is one frame: an 8-byte little-endian length, then that many
// bytes. A request frame starts with an Op byte and a reply frame with a
// Status byte; the fields after them are fixed per op and written with the
// Writer/Reader pair below, so the whole format is the one in this file.
//
// A connection opens with kHello, which carries the protocol version and the
// client's description of the cache it expects (see ConfigFingerprint). The
// daemon refuses a client whose cache differs from its own -- a different
// directory, bucket or prefix -- rather than serve it entries from the wrong
// place, and the client then runs with in-process layers as if no daemon
// existed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/config.h"

namespace vcache::daemon {

// Bumped on any incompatible change to a frame. A mismatch is refused at
// kHello, which makes a client and daemon from different releases fall back
// cleanly instead of misparsing each other.
constexpr uint64_t kProtocolVersion = 2;

enum class Op : uint8_t {
  kHello = 1,     // u64 version, str fingerprint
  kGet = 2,       // str key
  kPut = 3,       // str key, str value
  kStatus = 4,    // (nothing)
  kShutdown = 5,  // (nothing); the reply comes once uploads have drained
  kSessionOpen = 6,  // (nothing); keeps this connection for the compile's lifetime
};

enum class Status : uint8_t {
  kOk = 0,
  kMiss = 1,     // kGet only
  kRefused = 2,  // kHello: str reason
  kError = 3,    // str reason
};

// Appends fields to a frame body.
class Writer {
 public:
  void U8(uint8_t v) { buf_.push_back(static_cast<char>(v)); }
  void U64(uint64_t v);
  void Str(const std::string& s);
  void StrList(const std::vector<std::string>& list);
  const std::string& data() const { return buf_; }
  std::string& data() { return buf_; }

 private:
  std::string buf_;
};

// Reads fields back out. Every method returns false on truncation, and a
// failed read leaves the reader failed, so a caller can check once at the end.
class Reader {
 public:
  explicit Reader(const std::string& data) : data_(data) {}
  bool U8(uint8_t* v);
  bool U64(uint64_t* v);
  bool Str(std::string* s);
  bool StrList(std::vector<std::string>* list);
  bool done() const { return pos_ == data_.size(); }

 private:
  const std::string& data_;
  size_t pos_ = 0;
};

// Frames are capped so a corrupt length cannot make either side try to
// allocate the address space. Large enough for any artifact a single
// compilation produces.
constexpr uint64_t kMaxFrame = 16ull << 30;

// Blocking whole-frame I/O on a stream socket. EINTR is retried.
bool SendFrame(int fd, const std::string& body);
bool RecvFrame(int fd, std::string* body);

// The daemon's private directory: lock, pid file, log, upload journal, and the
// socket when its path fits. Lives inside the disk cache directory, which the
// disk layer never walks outside its 256 two-hex-digit shards.
std::string StateDir(const core::Config& config);

// The socket path for this configuration. An explicit daemon.socket wins.
// Otherwise <state>/sock, unless that is too long for sockaddr_un -- 104 bytes
// on macOS, 108 on Linux, which a deep $HOME reaches easily -- in which case a
// short path under /tmp keyed by the cache directory is used instead.
std::string SocketPath(const core::Config& config);

// What a client and a daemon must agree on for the daemon to serve that client:
// one "name: value" line per setting that decides *where entries live*. It is
// compared line by line, so a refusal can name the setting that differs.
// Credentials appear only as a digest of the access key id, which is enough to
// tell two identities apart without putting a secret on the wire or in a log.
std::string ConfigFingerprint(const core::Config& config);

// Returns the first line of `theirs` that differs from `ours`, as
// "setting: daemon has X, client has Y", or the empty string if they agree.
std::string FingerprintMismatch(const std::string& ours, const std::string& theirs);

// Layer name for a blob served from the upload queue. The client logs it.
// Anything other than "s3" still counts as a disk hit.
inline const char* HeldHitLayerName() { return "memory"; }

}  // namespace vcache::daemon
