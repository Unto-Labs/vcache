// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// BLAKE3-based hashing. Cache keys must be collision-resistant against
// accidental collisions across millions of entries; BLAKE3 gives that at
// several GB/s, which matters because preprocessor output is the bulk input.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vcache::hash {

// Length of the hex digest produced by Hasher::Hex().
inline constexpr size_t kDigestHexLen = 64;

class Hasher {
 public:
  Hasher();
  ~Hasher();

  Hasher(const Hasher&) = delete;
  Hasher& operator=(const Hasher&) = delete;

  void Update(std::string_view data);
  void Update(const void* data, size_t len);

  // Feeds `data` preceded by its length. Use this for every variable-length
  // field so that ("ab","c") and ("a","bc") cannot hash the same.
  void UpdateDelimited(std::string_view data);

  void UpdateU64(uint64_t value);

  // Streams a file's contents. Returns false if it could not be read, in which
  // case the caller must treat the compilation as uncacheable.
  bool UpdateFile(const std::string& path);

  // Finalises and returns the 64-character lowercase hex digest.
  std::string Hex() const;

 private:
  struct Impl;
  Impl* impl_;
};

// One-shot convenience wrapper.
std::string HashString(std::string_view data);

// Hashes a file's contents, or nullopt if unreadable.
std::optional<std::string> HashFile(const std::string& path);

}  // namespace vcache::hash
