// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "storage/chain.h"

#include "hash/hasher.h"
#include "util/log.h"
#include "util/str.h"

namespace vcache::storage {
namespace {

// Bumped when the section layout changes. A mismatch reads as a miss, which is
// what should happen to an entry this build cannot interpret. '02' added the
// per-file flags byte that carries the execute bit.
constexpr char kMagic[8] = {'V', 'C', 'A', 'C', 'H', 'E', '0', '2'};

void AppendU64(std::string* out, uint64_t value) {
  for (int i = 0; i < 8; ++i) out->push_back(static_cast<char>(value >> (8 * i)));
}

bool ReadU64(const std::string& data, size_t* pos, uint64_t* out) {
  if (*pos + 8 > data.size()) return false;
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(static_cast<unsigned char>(data[*pos + i])) << (8 * i);
  }
  *pos += 8;
  *out = value;
  return true;
}

// Per-file flags in a kFile section. Only the execute bit is carried: the rest
// of the mode is the caller's business, and rustc emits nothing else unusual.
constexpr char kFileFlagExecutable = 0x01;

void AppendSection(std::string* out, SectionKind kind, const std::string& payload) {
  out->push_back(static_cast<char>(kind));
  AppendU64(out, payload.size());
  out->append(payload);
}

}  // namespace

std::string SerializeBlob(const Blob& blob) {
  std::string body;
  AppendSection(&body, SectionKind::kObject, blob.object);
  if (blob.has_depfile) AppendSection(&body, SectionKind::kDepFile, blob.depfile);
  if (!blob.stderr_text.empty()) {
    AppendSection(&body, SectionKind::kStderr, blob.stderr_text);
  }
  if (!blob.meta.empty()) AppendSection(&body, SectionKind::kMeta, blob.meta);
  if (blob.has_dep_manifest) {
    AppendSection(&body, SectionKind::kDepManifest, blob.dep_manifest);
  }
  for (const BlobFile& file : blob.files) {
    // Length-prefixed name so a name containing any byte stays unambiguous,
    // then a flags byte, then the contents.
    std::string payload;
    AppendU64(&payload, file.name.size());
    payload.append(file.name);
    payload.push_back(file.executable ? kFileFlagExecutable : 0);
    payload.append(file.contents);
    AppendSection(&body, SectionKind::kFile, payload);
  }

  // A checksum over the body catches truncated uploads and bit rot; a corrupt
  // object silently linked into a binary would be far worse than a miss.
  const std::string digest = hash::HashString(body);

  std::string out;
  out.reserve(body.size() + 96);
  out.append(kMagic, sizeof(kMagic));
  out.append(digest);  // 64 hex characters
  AppendU64(&out, body.size());
  out.append(body);
  return out;
}

bool DeserializeBlob(const std::string& data, Blob* blob) {
  size_t pos = 0;
  if (data.size() < sizeof(kMagic) + hash::kDigestHexLen + 8) return false;
  if (data.compare(0, sizeof(kMagic), kMagic, sizeof(kMagic)) != 0) return false;
  pos += sizeof(kMagic);

  const std::string digest = data.substr(pos, hash::kDigestHexLen);
  pos += hash::kDigestHexLen;

  uint64_t body_len = 0;
  if (!ReadU64(data, &pos, &body_len)) return false;
  if (pos + body_len != data.size()) return false;

  const std::string body = data.substr(pos, body_len);
  if (hash::HashString(body) != digest) {
    VCACHE_LOG("cache entry failed checksum verification");
    return false;
  }

  size_t bpos = 0;
  while (bpos < body.size()) {
    if (bpos + 1 > body.size()) return false;
    const auto kind = static_cast<SectionKind>(static_cast<unsigned char>(body[bpos]));
    bpos += 1;
    uint64_t len = 0;
    if (!ReadU64(body, &bpos, &len)) return false;
    if (bpos + len > body.size()) return false;
    std::string payload = body.substr(bpos, len);
    bpos += len;

    switch (kind) {
      case SectionKind::kObject: blob->object = std::move(payload); break;
      case SectionKind::kDepFile:
        blob->depfile = std::move(payload);
        blob->has_depfile = true;
        break;
      case SectionKind::kStderr: blob->stderr_text = std::move(payload); break;
      case SectionKind::kMeta: blob->meta = std::move(payload); break;
      case SectionKind::kDepManifest:
        blob->dep_manifest = std::move(payload);
        blob->has_dep_manifest = true;
        break;
      case SectionKind::kFile: {
        size_t fpos = 0;
        uint64_t name_len = 0;
        if (!ReadU64(payload, &fpos, &name_len)) return false;
        if (fpos + name_len + 1 > payload.size()) return false;
        BlobFile file;
        file.name = payload.substr(fpos, name_len);
        const auto flags =
            static_cast<unsigned char>(payload[fpos + name_len]);
        file.executable = (flags & kFileFlagExecutable) != 0;
        file.contents = payload.substr(fpos + name_len + 1);
        blob->files.push_back(std::move(file));
        break;
      }
      default:
        // Unknown section from a newer vcache: skip it rather than fail.
        break;
    }
  }
  return true;
}

void CacheChain::AddLayer(std::unique_ptr<Storage> layer) {
  if (layer != nullptr) layers_.push_back(std::move(layer));
}

GetResult CacheChain::Get(const std::string& key) {
  GetResult result;
  for (size_t i = 0; i < layers_.size(); ++i) {
    std::string value;
    if (!layers_[i]->Get(key, &value)) {
      // A layer that is broken rather than merely cold still lets the lookup
      // fall through to the next one; the build must not stop for it here.
      if (layers_[i]->failed()) {
        result.errors.push_back(layers_[i]->Name() + ": " + layers_[i]->last_error());
      }
      continue;
    }

    result.hit = true;
    result.value = std::move(value);
    result.layer = layers_[i]->Name();

    // Backfill the faster layers this lookup passed through.
    for (size_t j = 0; j < i; ++j) {
      if (!layers_[j]->writable()) continue;
      if (layers_[j]->Put(key, result.value)) {
        VCACHE_LOG("backfilled " + layers_[j]->Name() + " from " + result.layer);
      } else if (layers_[j]->failed()) {
        result.errors.push_back(layers_[j]->Name() + ": " + layers_[j]->last_error());
      }
    }
    return result;
  }
  return result;
}

PutResult CacheChain::Put(const std::string& key, const std::string& value) {
  PutResult result;
  for (auto& layer : layers_) {
    if (!layer->writable()) continue;
    if (layer->Put(key, value)) {
      result.stored = true;
    } else {
      VCACHE_LOG("store failed on layer " + layer->Name());
      if (layer->failed()) {
        result.errors.push_back(layer->Name() + ": " + layer->last_error());
      }
    }
  }
  return result;
}

std::vector<std::string> CacheChain::LayerNames() const {
  std::vector<std::string> names;
  names.reserve(layers_.size());
  for (const auto& layer : layers_) names.push_back(layer->Name());
  return names;
}

void CacheChain::Trim() {
  for (auto& layer : layers_) layer->Trim();
}

}  // namespace vcache::storage
