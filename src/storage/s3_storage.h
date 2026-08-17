// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// S3 cache layer.
//
// Requests are signed with AWS Signature Version 4 over libcurl, avoiding a
// dependency on the AWS SDK; SHA-256/HMAC come from hash/sha256.h rather than
// OpenSSL. A compile only ever issues GET and PUT of a single object. Trim()
// additionally lists and deletes, which is why it is confined to `vcache
// --trim` and never runs on the compile path.
//
// libcurl is loaded with dlopen() on construction rather than linked, so a
// disk-only build never maps it. Any transport or authentication failure --
// including libcurl being absent -- is reported as a miss: a shared cache that
// is unreachable must never break a build.
#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "core/config.h"
#include "storage/curl_api.h"
#include "storage/storage.h"

namespace vcache::storage {

// One entry as ListObjectsV2 describes it.
struct S3Object {
  std::string key;   // full object key, prefix included
  uint64_t size = 0;
  std::time_t last_modified = 0;
};

class S3Storage : public Storage {
 public:
  explicit S3Storage(core::S3CacheConfig config);
  ~S3Storage() override;

  std::string Name() const override { return "s3"; }
  bool Get(const std::string& key, std::string* value) override;
  bool Put(const std::string& key, const std::string& value) override;
  bool writable() const override { return !read_only_; }

  // Deletes expired entries and, when a byte budget is configured, the oldest
  // entries beyond it. Unlike the disk layer this never runs inline: it costs a
  // bucket listing, and on a bucket shared by a fleet the decision to evict is
  // not one an ordinary compile should be making. `vcache --trim` calls it.
  void Trim() override;

  // Counts from the last Trim(), for reporting. Deleted objects are those this
  // process actually removed, not those a lifecycle rule may also expire.
  struct TrimResult {
    bool ran = false;          // false when nothing was configured to enforce
    bool complete = false;     // false if listing or a delete failed part-way
    uint64_t listed = 0;
    uint64_t expired = 0;      // deleted for age
    uint64_t evicted = 0;      // deleted for the byte budget
    uint64_t bytes_before = 0;
    uint64_t bytes_deleted = 0;
  };
  const TrimResult& last_trim() const { return last_trim_; }

  void set_read_only(bool read_only) { read_only_ = read_only; }

  // False when libcurl could not be loaded, in which case this layer can never
  // serve or store anything and the caller should leave it out of the chain.
  bool available() const { return curl_ != nullptr; }
  const std::string& load_error() const { return load_error_; }

 private:
  // Performs one signed request. `payload` is empty for GET and DELETE.
  // `query` is the already-sorted canonical query string ("a=1&b=2"), empty for
  // ordinary object operations. When `last_modified` is non-null the response's
  // Last-Modified header is parsed into it, leaving it at 0 if absent.
  bool Request(const std::string& method, const std::string& canonical_uri_path,
               const std::string& query, const std::string& payload,
               std::string* response_body, std::time_t* last_modified);

  // Convenience wrapper for the single-object case: builds the object key from
  // a cache key and issues an unqueried request.
  bool ObjectRequest(const std::string& method, const std::string& key,
                     const std::string& payload, std::string* response_body,
                     std::time_t* last_modified);

  // Lists every object under the configured prefix, following continuation
  // tokens. Returns false if any page failed, leaving `out` with what it got.
  bool ListAll(std::vector<S3Object>* out);

  bool DeleteObject(const std::string& object_key);

  // Settles what a 403 on a lookup meant, at most once per process.
  //
  // S3 answers a missing key with 404 when the caller holds ListBucket and 403
  // when it does not, so a 403 is either an ordinary miss on an under-permitted
  // bucket or a real permission failure -- and the two want opposite handling.
  // One max-keys=0 listing tells them apart. A bucket that grants ListBucket
  // never reaches this, because its misses are 404s, so the cost falls only on
  // the configuration this is trying to get fixed.
  void DiagnoseDenial(const std::string& key);

  // Builds the object key, applying the configured prefix and sharding the
  // digest so a bucket listing stays navigable.
  std::string ObjectKey(const std::string& key) const;

  // Resolves host and full URL for `object_key`, honouring path-style access.
  void ResolveEndpoint(const std::string& object_key, std::string* host,
                       std::string* url, std::string* canonical_uri) const;

  core::S3CacheConfig config_;
  const CurlApi* curl_ = nullptr;
  std::string load_error_;
  bool read_only_ = false;
  long last_status_ = 0;
  bool denial_diagnosed_ = false;
  TrimResult last_trim_;
};

// Exposed for unit testing: response parsing.
//
// Parses an RFC 7231 IMF-fixdate ("Sun, 06 Nov 1994 08:49:37 GMT"), which is
// what S3 sends in Last-Modified. Returns false on anything else; the caller
// treats that as "age unknown" rather than as expired, since refusing an entry
// vcache simply failed to read a header for would throw away a good cache.
bool ParseHttpDate(const std::string& value, std::time_t* out);

// Parses an ISO-8601 instant ("1994-11-06T08:49:37.000Z"), which is what
// ListObjectsV2 puts in <LastModified>.
bool ParseIso8601(const std::string& value, std::time_t* out);

// Pulls the <Contents> entries out of a ListObjectsV2 response. Sets
// `next_token` to the continuation token when the listing is truncated, and to
// the empty string otherwise. Returns false if the body is not a listing at
// all, which includes the error documents S3 returns with a 200 in some
// gateway configurations.
bool ParseListObjectsV2(const std::string& xml, std::vector<S3Object>* out,
                        std::string* next_token);

// Exposed for unit testing: AWS SigV4 primitives.
namespace sigv4 {

std::string Sha256Hex(const std::string& data);
std::string HmacSha256(const std::string& key, const std::string& data);
std::string HmacSha256Hex(const std::string& key, const std::string& data);

// Percent-encodes per RFC 3986. When `keep_slash` is set, '/' is left alone,
// which is what S3 canonical URIs require.
std::string UriEncode(const std::string& input, bool keep_slash);

// Builds the Authorization header value for a request.
std::string BuildAuthorization(const std::string& method,
                               const std::string& canonical_uri,
                               const std::string& query,
                               const std::string& host,
                               const std::string& amz_date,
                               const std::string& date_stamp,
                               const std::string& payload_hash,
                               const std::string& session_token,
                               const std::string& region,
                               const std::string& access_key,
                               const std::string& secret_key);

}  // namespace sigv4

}  // namespace vcache::storage
