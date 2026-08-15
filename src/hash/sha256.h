// SHA-256 and HMAC-SHA256 (FIPS 180-4, RFC 2104).
//
// vcache needs these for exactly one thing: AWS SigV4 request signing. Linking
// OpenSSL for three functions costs a shared-library dependency that is loaded
// on every compiler invocation, so they are implemented here instead.
//
// This is not a general-purpose crypto library and makes no attempt at
// constant-time behaviour -- it signs outbound requests with a key the process
// already holds, so there is no secret to leak through timing to an attacker who
// is not already inside the process.
//
// BLAKE3 remains the hash used for cache keys; SHA-256 is here only because AWS
// specifies it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace vcache::hash {

inline constexpr size_t kSha256DigestLen = 32;
inline constexpr size_t kSha256BlockLen = 64;

class Sha256 {
 public:
  Sha256();

  void Update(const void* data, size_t len);
  void Update(std::string_view data) { Update(data.data(), data.size()); }

  // Writes the digest and leaves the object unusable until Reset().
  void Final(unsigned char out[kSha256DigestLen]);

  void Reset();

 private:
  void Compress(const unsigned char block[kSha256BlockLen]);

  uint32_t state_[8];
  uint64_t bit_length_;
  unsigned char buffer_[kSha256BlockLen];
  size_t buffered_;
};

// One-shot helpers.
void Sha256Raw(std::string_view data, unsigned char out[kSha256DigestLen]);
std::string Sha256Hex(std::string_view data);

// HMAC-SHA256. Returns the raw 32-byte MAC, since SigV4 chains one HMAC's
// output into the next as a key.
std::string HmacSha256(std::string_view key, std::string_view data);
std::string HmacSha256Hex(std::string_view key, std::string_view data);

}  // namespace vcache::hash
