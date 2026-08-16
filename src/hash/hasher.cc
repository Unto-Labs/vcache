// SPDX-License-Identifier: GPL-3.0-or-later
#include "hash/hasher.h"

#include <cstdio>
#include <vector>

#include "util/str.h"

extern "C" {
#include "blake3.h"
}

namespace vcache::hash {

struct Hasher::Impl {
  blake3_hasher state;
};

Hasher::Hasher() : impl_(new Impl) { blake3_hasher_init(&impl_->state); }

Hasher::~Hasher() { delete impl_; }

void Hasher::Update(std::string_view data) {
  blake3_hasher_update(&impl_->state, data.data(), data.size());
}

void Hasher::Update(const void* data, size_t len) {
  blake3_hasher_update(&impl_->state, data, len);
}

void Hasher::UpdateDelimited(std::string_view data) {
  UpdateU64(data.size());
  Update(data);
}

void Hasher::UpdateU64(uint64_t value) {
  // Little-endian fixed width keeps digests stable regardless of host order.
  unsigned char buf[8];
  for (int i = 0; i < 8; ++i) buf[i] = static_cast<unsigned char>(value >> (8 * i));
  Update(buf, sizeof(buf));
}

bool Hasher::UpdateFile(const std::string& path) {
  FILE* f = ::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  std::vector<char> buf(1 << 20);
  bool ok = true;
  while (true) {
    size_t n = ::fread(buf.data(), 1, buf.size(), f);
    if (n > 0) Update(buf.data(), n);
    if (n < buf.size()) {
      if (::ferror(f) != 0) ok = false;
      break;
    }
  }
  ::fclose(f);
  return ok;
}

std::string Hasher::Hex() const {
  // Finalise on a copy: callers may keep updating after reading a digest.
  blake3_hasher copy = impl_->state;
  unsigned char out[BLAKE3_OUT_LEN];
  blake3_hasher_finalize(&copy, out, BLAKE3_OUT_LEN);
  return util::HexEncode(out, BLAKE3_OUT_LEN);
}

std::string HashString(std::string_view data) {
  Hasher h;
  h.Update(data);
  return h.Hex();
}

std::optional<std::string> HashFile(const std::string& path) {
  Hasher h;
  if (!h.UpdateFile(path)) return std::nullopt;
  return h.Hex();
}

}  // namespace vcache::hash
