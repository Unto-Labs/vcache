// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/s3_storage.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <mutex>
#include <utility>

#include "hash/sha256.h"
#include "storage/curl_api.h"
#include "util/log.h"
#include "util/str.h"

namespace vcache::storage {
namespace sigv4 {

// Thin forwarders to the vendored implementations. SigV4 is the only thing in
// vcache that needs SHA-256; cache keys use BLAKE3.
std::string Sha256Hex(const std::string& data) { return hash::Sha256Hex(data); }

std::string HmacSha256(const std::string& key, const std::string& data) {
  return hash::HmacSha256(key, data);
}

std::string HmacSha256Hex(const std::string& key, const std::string& data) {
  return hash::HmacSha256Hex(key, data);
}

std::string UriEncode(const std::string& input, bool keep_slash) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(input.size() * 3);
  for (unsigned char c : input) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                            c == '.' || c == '~';
    if (unreserved || (keep_slash && c == '/')) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0f]);
    }
  }
  return out;
}

std::string BuildAuthorization(const std::string& method,
                               const std::string& canonical_uri,
                               const std::string& host,
                               const std::string& amz_date,
                               const std::string& date_stamp,
                               const std::string& payload_hash,
                               const std::string& session_token,
                               const std::string& region,
                               const std::string& access_key,
                               const std::string& secret_key) {
  // Canonical headers must be sorted by lowercase name; the set below is
  // already in order (host, x-amz-content-sha256, x-amz-date, x-amz-security-token).
  std::string canonical_headers = "host:" + host + "\n" +
                                  "x-amz-content-sha256:" + payload_hash + "\n" +
                                  "x-amz-date:" + amz_date + "\n";
  std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";
  if (!session_token.empty()) {
    canonical_headers += "x-amz-security-token:" + session_token + "\n";
    signed_headers += ";x-amz-security-token";
  }

  const std::string canonical_request = method + "\n" + canonical_uri + "\n" +
                                        /*query=*/"" + "\n" + canonical_headers +
                                        "\n" + signed_headers + "\n" + payload_hash;

  const std::string scope = date_stamp + "/" + region + "/s3/aws4_request";
  const std::string string_to_sign = "AWS4-HMAC-SHA256\n" + amz_date + "\n" +
                                     scope + "\n" + Sha256Hex(canonical_request);

  const std::string k_date = HmacSha256("AWS4" + secret_key, date_stamp);
  const std::string k_region = HmacSha256(k_date, region);
  const std::string k_service = HmacSha256(k_region, "s3");
  const std::string k_signing = HmacSha256(k_service, "aws4_request");
  const std::string signature = HmacSha256Hex(k_signing, string_to_sign);

  return "AWS4-HMAC-SHA256 Credential=" + access_key + "/" + scope +
         ", SignedHeaders=" + signed_headers + ", Signature=" + signature;
}

}  // namespace sigv4

namespace {

size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* sink = static_cast<std::string*>(userdata);
  sink->append(ptr, size * nmemb);
  return size * nmemb;
}

struct ReadContext {
  const std::string* data;
  size_t offset;
};

size_t ReadCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
  auto* ctx = static_cast<ReadContext*>(userdata);
  const size_t remaining = ctx->data->size() - ctx->offset;
  const size_t n = std::min(remaining, size * nitems);
  if (n > 0) {
    std::memcpy(buffer, ctx->data->data() + ctx->offset, n);
    ctx->offset += n;
  }
  return n;
}

void FormatTimes(std::string* amz_date, std::string* date_stamp) {
  const std::time_t now = std::time(nullptr);
  struct tm tm_utc;
  ::gmtime_r(&now, &tm_utc);
  char buf[32];
  ::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm_utc);
  *amz_date = buf;
  ::strftime(buf, sizeof(buf), "%Y%m%d", &tm_utc);
  *date_stamp = buf;
}

}  // namespace

S3Storage::S3Storage(core::S3CacheConfig config) : config_(std::move(config)) {
  // Opening libcurl here rather than at link time keeps roughly thirty shared
  // objects out of every disk-only compilation. See storage/curl_api.h.
  curl_ = CurlApi::Load(&load_error_);
  read_only_ = config_.no_credentials;
}

S3Storage::~S3Storage() = default;

std::string S3Storage::ObjectKey(const std::string& key) const {
  std::string prefix = config_.prefix;
  if (!prefix.empty() && prefix.back() != '/') prefix.push_back('/');
  // Two-character shard keeps `aws s3 ls` output usable on large caches.
  return prefix + key.substr(0, 2) + "/" + key.substr(2);
}

void S3Storage::ResolveEndpoint(const std::string& object_key, std::string* host,
                                std::string* url,
                                std::string* canonical_uri) const {
  const std::string encoded_key = sigv4::UriEncode(object_key, /*keep_slash=*/true);

  if (!config_.endpoint.empty()) {
    // Custom endpoint (MinIO, Ceph, an S3 gateway). Path style unless the
    // config explicitly asks otherwise.
    std::string base = config_.endpoint;
    while (!base.empty() && base.back() == '/') base.pop_back();

    std::string scheme_stripped = base;
    for (std::string_view scheme : {"https://", "http://"}) {
      if (util::StartsWith(scheme_stripped, scheme)) {
        scheme_stripped = scheme_stripped.substr(scheme.size());
        break;
      }
    }
    // Host header excludes any path component of the endpoint.
    const size_t slash = scheme_stripped.find('/');
    *host = (slash == std::string::npos) ? scheme_stripped
                                         : scheme_stripped.substr(0, slash);

    if (config_.use_path_style) {
      *canonical_uri = "/" + config_.bucket + "/" + encoded_key;
    } else {
      *host = config_.bucket + "." + *host;
      *canonical_uri = "/" + encoded_key;
      base = base.substr(0, base.find("://") + 3) + *host;
    }
    *url = base + *canonical_uri;
    return;
  }

  if (config_.use_path_style) {
    *host = "s3." + config_.region + ".amazonaws.com";
    *canonical_uri = "/" + config_.bucket + "/" + encoded_key;
  } else {
    *host = config_.bucket + ".s3." + config_.region + ".amazonaws.com";
    *canonical_uri = "/" + encoded_key;
  }
  *url = "https://" + *host + *canonical_uri;
}

bool S3Storage::Request(const std::string& method, const std::string& key,
                        const std::string& payload, std::string* response_body) {
  if (curl_ == nullptr) return false;  // libcurl unavailable; behave as a miss

  const std::string object_key = ObjectKey(key);
  std::string host, url, canonical_uri;
  ResolveEndpoint(object_key, &host, &url, &canonical_uri);

  std::string amz_date, date_stamp;
  FormatTimes(&amz_date, &date_stamp);
  const std::string payload_hash = sigv4::Sha256Hex(payload);

  CURL* curl = curl_->easy_init();
  if (curl == nullptr) return false;

  struct curl_slist* headers = nullptr;
  headers = curl_->slist_append(headers, ("x-amz-date: " + amz_date).c_str());
  headers = curl_->slist_append(
      headers, ("x-amz-content-sha256: " + payload_hash).c_str());
  if (!config_.session_token.empty()) {
    headers = curl_->slist_append(
        headers, ("x-amz-security-token: " + config_.session_token).c_str());
  }
  if (!config_.no_credentials) {
    const std::string authorization = sigv4::BuildAuthorization(
        method, canonical_uri, host, amz_date, date_stamp, payload_hash,
        config_.session_token, config_.region, config_.access_key,
        config_.secret_key);
    headers = curl_->slist_append(headers, ("Authorization: " + authorization).c_str());
  }
  // S3 rejects the Expect: 100-continue that curl adds for larger PUTs.
  headers = curl_->slist_append(headers, "Expect:");

  curl_->easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_->easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_->easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_->easy_setopt(curl, CURLOPT_WRITEDATA, response_body);
  curl_->easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(config_.timeout_seconds));
  curl_->easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_->easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_->easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  ReadContext read_ctx{&payload, 0};
  if (method == "PUT") {
    curl_->easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_->easy_setopt(curl, CURLOPT_READFUNCTION, ReadCallback);
    curl_->easy_setopt(curl, CURLOPT_READDATA, &read_ctx);
    curl_->easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                       static_cast<curl_off_t>(payload.size()));
  }

  const CURLcode rc = curl_->easy_perform(curl);
  long status = 0;
  curl_->easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_->slist_free_all(headers);
  curl_->easy_cleanup(curl);

  if (rc != CURLE_OK) {
    VCACHE_LOG("s3: " + method + " " + object_key + " transport error: " +
               curl_->easy_strerror(rc));
    return false;
  }
  if (status == 404 || status == 403) {
    // 403 shows up instead of 404 on buckets without ListBucket permission.
    return false;
  }
  if (status < 200 || status >= 300) {
    VCACHE_LOG("s3: " + method + " " + object_key + " HTTP " +
               std::to_string(status) + ": " + response_body->substr(0, 512));
    return false;
  }
  return true;
}

bool S3Storage::Get(const std::string& key, std::string* value) {
  std::string body;
  if (!Request("GET", key, "", &body)) return false;
  *value = std::move(body);
  return true;
}

bool S3Storage::Put(const std::string& key, const std::string& value) {
  if (read_only_) return false;
  std::string response;
  return Request("PUT", key, value, &response);
}

}  // namespace vcache::storage
