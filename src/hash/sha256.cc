// SPDX-License-Identifier: GPL-3.0-only
#include "hash/sha256.h"

#include <algorithm>
#include <cstring>

#include "util/str.h"

namespace vcache::hash {
namespace {

// First 32 bits of the fractional parts of the cube roots of the first 64
// primes (FIPS 180-4, section 4.2.2).
constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t RotateRight(uint32_t value, int bits) {
  return (value >> bits) | (value << (32 - bits));
}

inline uint32_t BigEndian32(const unsigned char* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline void StoreBigEndian32(unsigned char* p, uint32_t value) {
  p[0] = static_cast<unsigned char>(value >> 24);
  p[1] = static_cast<unsigned char>(value >> 16);
  p[2] = static_cast<unsigned char>(value >> 8);
  p[3] = static_cast<unsigned char>(value);
}

}  // namespace

Sha256::Sha256() { Reset(); }

void Sha256::Reset() {
  // First 32 bits of the fractional parts of the square roots of the first
  // eight primes.
  state_[0] = 0x6a09e667;
  state_[1] = 0xbb67ae85;
  state_[2] = 0x3c6ef372;
  state_[3] = 0xa54ff53a;
  state_[4] = 0x510e527f;
  state_[5] = 0x9b05688c;
  state_[6] = 0x1f83d9ab;
  state_[7] = 0x5be0cd19;
  bit_length_ = 0;
  buffered_ = 0;
  std::memset(buffer_, 0, sizeof(buffer_));
}

void Sha256::Compress(const unsigned char block[kSha256BlockLen]) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) w[i] = BigEndian32(block + 4 * i);
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 =
        RotateRight(w[i - 15], 7) ^ RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 =
        RotateRight(w[i - 2], 17) ^ RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

  for (int i = 0; i < 64; ++i) {
    const uint32_t S1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t temp1 = h + S1 + ch + kRoundConstants[i] + w[i];
    const uint32_t S0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = S0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::Update(const void* data, size_t len) {
  const auto* p = static_cast<const unsigned char*>(data);
  bit_length_ += static_cast<uint64_t>(len) * 8;

  // Top up a partially filled block first.
  if (buffered_ != 0) {
    const size_t take = std::min(kSha256BlockLen - buffered_, len);
    std::memcpy(buffer_ + buffered_, p, take);
    buffered_ += take;
    p += take;
    len -= take;
    if (buffered_ == kSha256BlockLen) {
      Compress(buffer_);
      buffered_ = 0;
    }
  }

  while (len >= kSha256BlockLen) {
    Compress(p);
    p += kSha256BlockLen;
    len -= kSha256BlockLen;
  }

  if (len != 0) {
    std::memcpy(buffer_, p, len);
    buffered_ = len;
  }
}

void Sha256::Final(unsigned char out[kSha256DigestLen]) {
  // Padding is written straight into the buffer rather than through Update(),
  // which would add it to the message length.
  const uint64_t bits = bit_length_;

  // Always at least one padding byte: 0x80. buffered_ is < 64 here, because
  // Update() compresses as soon as a block fills.
  buffer_[buffered_++] = 0x80;

  // The 8-byte length must fit in this block; if it does not, flush a full
  // block of padding first.
  if (buffered_ > kSha256BlockLen - 8) {
    std::memset(buffer_ + buffered_, 0, kSha256BlockLen - buffered_);
    Compress(buffer_);
    buffered_ = 0;
  }
  std::memset(buffer_ + buffered_, 0, (kSha256BlockLen - 8) - buffered_);

  for (int i = 0; i < 8; ++i) {
    buffer_[kSha256BlockLen - 8 + i] =
        static_cast<unsigned char>(bits >> (56 - 8 * i));
  }
  Compress(buffer_);
  buffered_ = 0;

  for (int i = 0; i < 8; ++i) StoreBigEndian32(out + 4 * i, state_[i]);
}

void Sha256Raw(std::string_view data, unsigned char out[kSha256DigestLen]) {
  Sha256 ctx;
  ctx.Update(data);
  ctx.Final(out);
}

std::string Sha256Hex(std::string_view data) {
  unsigned char digest[kSha256DigestLen];
  Sha256Raw(data, digest);
  return util::HexEncode(digest, kSha256DigestLen);
}

std::string HmacSha256(std::string_view key, std::string_view data) {
  unsigned char key_block[kSha256BlockLen];
  std::memset(key_block, 0, sizeof(key_block));

  // RFC 2104: a key longer than the block size is replaced by its hash.
  if (key.size() > kSha256BlockLen) {
    unsigned char hashed[kSha256DigestLen];
    Sha256Raw(key, hashed);
    std::memcpy(key_block, hashed, kSha256DigestLen);
  } else {
    std::memcpy(key_block, key.data(), key.size());
  }

  unsigned char inner_pad[kSha256BlockLen];
  unsigned char outer_pad[kSha256BlockLen];
  for (size_t i = 0; i < kSha256BlockLen; ++i) {
    inner_pad[i] = static_cast<unsigned char>(key_block[i] ^ 0x36);
    outer_pad[i] = static_cast<unsigned char>(key_block[i] ^ 0x5c);
  }

  unsigned char inner_digest[kSha256DigestLen];
  Sha256 inner;
  inner.Update(inner_pad, sizeof(inner_pad));
  inner.Update(data);
  inner.Final(inner_digest);

  unsigned char outer_digest[kSha256DigestLen];
  Sha256 outer;
  outer.Update(outer_pad, sizeof(outer_pad));
  outer.Update(inner_digest, sizeof(inner_digest));
  outer.Final(outer_digest);

  return std::string(reinterpret_cast<char*>(outer_digest), kSha256DigestLen);
}

std::string HmacSha256Hex(std::string_view key, std::string_view data) {
  const std::string raw = HmacSha256(key, data);
  return util::HexEncode(reinterpret_cast<const unsigned char*>(raw.data()),
                         raw.size());
}

}  // namespace vcache::hash
