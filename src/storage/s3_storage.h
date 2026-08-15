// S3 cache layer.
//
// Requests are signed with AWS Signature Version 4 over libcurl. Only GET and
// PUT of a single object are needed, so this avoids a dependency on the AWS
// SDK, and SHA-256/HMAC come from hash/sha256.h rather than OpenSSL.
//
// libcurl is loaded with dlopen() on construction rather than linked, so a
// disk-only build never maps it. Any transport or authentication failure --
// including libcurl being absent -- is reported as a miss: a shared cache that
// is unreachable must never break a build.
#pragma once

#include <string>

#include "core/config.h"
#include "storage/curl_api.h"
#include "storage/storage.h"

namespace vcache::storage {

class S3Storage : public Storage {
 public:
  explicit S3Storage(core::S3CacheConfig config);
  ~S3Storage() override;

  std::string Name() const override { return "s3"; }
  bool Get(const std::string& key, std::string* value) override;
  bool Put(const std::string& key, const std::string& value) override;
  bool writable() const override { return !read_only_; }

  void set_read_only(bool read_only) { read_only_ = read_only; }

  // False when libcurl could not be loaded, in which case this layer can never
  // serve or store anything and the caller should leave it out of the chain.
  bool available() const { return curl_ != nullptr; }
  const std::string& load_error() const { return load_error_; }

 private:
  // Performs one signed request. `payload` is empty for GET.
  bool Request(const std::string& method, const std::string& key,
               const std::string& payload, std::string* response_body);

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
};

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
