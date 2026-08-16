// SPDX-License-Identifier: GPL-3.0-only
#include "storage/s3_storage.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

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
                               const std::string& query,
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

  // The canonical query string must already be sorted by key and encoded; the
  // only caller that passes a non-empty one is the listing, which builds it
  // that way deliberately.
  const std::string canonical_request = method + "\n" + canonical_uri + "\n" +
                                        query + "\n" + canonical_headers +
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

size_t HeaderCallback(char* ptr, size_t size, size_t nitems, void* userdata) {
  const size_t n = size * nitems;
  auto* out = static_cast<std::time_t*>(userdata);
  // Header names are case-insensitive; S3 sends "Last-Modified" but a gateway
  // in front of it need not.
  static constexpr std::string_view kName = "last-modified:";
  std::string line(ptr, n);
  std::string lower = line.substr(0, std::min(n, kName.size()));
  for (char& c : lower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  if (lower == kName) {
    std::string value = line.substr(kName.size());
    // Trim surrounding whitespace and the trailing CRLF.
    const size_t first = value.find_first_not_of(" \t");
    const size_t last = value.find_last_not_of(" \t\r\n");
    if (first != std::string::npos && last != std::string::npos && first <= last) {
      std::time_t parsed = 0;
      if (ParseHttpDate(value.substr(first, last - first + 1), &parsed)) {
        *out = parsed;
      }
    }
  }
  return n;
}

const char* const kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// timegm() is a GNU extension but present everywhere vcache builds; the
// alternative is mktime() plus a timezone dance, which is worse.
std::time_t FromUtcParts(int year, int month, int day, int hour, int minute,
                         int second) {
  struct tm tm_utc = {};
  tm_utc.tm_year = year - 1900;
  tm_utc.tm_mon = month - 1;
  tm_utc.tm_mday = day;
  tm_utc.tm_hour = hour;
  tm_utc.tm_min = minute;
  tm_utc.tm_sec = second;
  return ::timegm(&tm_utc);
}

// Extracts the text between the first <tag> and its closing </tag> at or after
// `pos`. Sufficient for ListObjectsV2, whose fields carry no attributes and no
// nested markup; anything richer would want a real parser.
bool ElementText(const std::string& xml, const std::string& tag, size_t* pos,
                 std::string* out) {
  const std::string open = "<" + tag + ">";
  const std::string close = "</" + tag + ">";
  const size_t start = xml.find(open, *pos);
  if (start == std::string::npos) return false;
  const size_t from = start + open.size();
  const size_t end = xml.find(close, from);
  if (end == std::string::npos) return false;
  *out = xml.substr(from, end - from);
  *pos = end + close.size();
  return true;
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

bool ParseHttpDate(const std::string& value, std::time_t* out) {
  // "Sun, 06 Nov 1994 08:49:37 GMT" -- fixed width, so scan rather than parse.
  if (value.size() < 29) return false;
  char mon[4] = {0};
  int day = 0, year = 0, hour = 0, minute = 0, second = 0;
  if (std::sscanf(value.c_str() + 5, "%2d %3s %4d %2d:%2d:%2d", &day, mon, &year,
                  &hour, &minute, &second) != 6) {
    return false;
  }
  int month = 0;
  for (int i = 0; i < 12; ++i) {
    if (std::strcmp(mon, kMonths[i]) == 0) {
      month = i + 1;
      break;
    }
  }
  if (month == 0) return false;
  *out = FromUtcParts(year, month, day, hour, minute, second);
  return true;
}

bool ParseIso8601(const std::string& value, std::time_t* out) {
  // "1994-11-06T08:49:37.000Z". Fractional seconds are ignored: this is used
  // for eviction ordering, where sub-second resolution buys nothing.
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (std::sscanf(value.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day,
                  &hour, &minute, &second) != 6) {
    return false;
  }
  if (month < 1 || month > 12) return false;
  *out = FromUtcParts(year, month, day, hour, minute, second);
  return true;
}

bool ParseListObjectsV2(const std::string& xml, std::vector<S3Object>* out,
                        std::string* next_token) {
  next_token->clear();
  if (xml.find("<ListBucketResult") == std::string::npos) return false;

  size_t pos = 0;
  while (true) {
    const size_t contents = xml.find("<Contents>", pos);
    if (contents == std::string::npos) break;
    size_t cursor = contents;

    S3Object obj;
    std::string key, size, modified;
    if (!ElementText(xml, "Key", &cursor, &key)) break;
    obj.key = key;
    // Size and LastModified are both optional in the sense that a malformed
    // page should not abort the whole trim; an entry missing either is kept
    // but cannot be evicted on that axis.
    size_t probe = cursor;
    if (ElementText(xml, "LastModified", &probe, &modified)) {
      ParseIso8601(modified, &obj.last_modified);
    }
    probe = cursor;
    if (ElementText(xml, "Size", &probe, &size)) {
      obj.size = std::strtoull(size.c_str(), nullptr, 10);
    }
    out->push_back(std::move(obj));

    const size_t close = xml.find("</Contents>", contents);
    if (close == std::string::npos) break;
    pos = close + 1;
  }

  size_t token_pos = 0;
  std::string truncated;
  if (ElementText(xml, "IsTruncated", &token_pos, &truncated) &&
      truncated == "true") {
    token_pos = 0;
    std::string token;
    if (ElementText(xml, "NextContinuationToken", &token_pos, &token)) {
      *next_token = token;
    }
  }
  return true;
}

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
  // An empty key addresses the bucket itself, which is what a listing needs.
  // Path style then stops at "/<bucket>" with no trailing slash, because a
  // trailing slash is a different canonical URI and would not verify.
  const std::string path_suffix = encoded_key.empty() ? "" : "/" + encoded_key;

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
      *canonical_uri = "/" + config_.bucket + path_suffix;
    } else {
      *host = config_.bucket + "." + *host;
      *canonical_uri = encoded_key.empty() ? "/" : "/" + encoded_key;
      base = base.substr(0, base.find("://") + 3) + *host;
    }
    *url = base + *canonical_uri;
    return;
  }

  if (config_.use_path_style) {
    *host = "s3." + config_.region + ".amazonaws.com";
    *canonical_uri = "/" + config_.bucket + path_suffix;
  } else {
    *host = config_.bucket + ".s3." + config_.region + ".amazonaws.com";
    *canonical_uri = encoded_key.empty() ? "/" : "/" + encoded_key;
  }
  *url = "https://" + *host + *canonical_uri;
}

bool S3Storage::ObjectRequest(const std::string& method, const std::string& key,
                              const std::string& payload,
                              std::string* response_body,
                              std::time_t* last_modified) {
  return Request(method, ObjectKey(key), /*query=*/"", payload, response_body,
                 last_modified);
}

bool S3Storage::Request(const std::string& method,
                        const std::string& canonical_uri_path,
                        const std::string& query, const std::string& payload,
                        std::string* response_body,
                        std::time_t* last_modified) {
  if (curl_ == nullptr) {  // libcurl unavailable; behave as a miss
    SetError("libcurl is not available: " + load_error_);
    return false;
  }

  std::string host, url, canonical_uri;
  ResolveEndpoint(canonical_uri_path, &host, &url, &canonical_uri);
  if (!query.empty()) url += "?" + query;

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
        method, canonical_uri, query, host, amz_date, date_stamp, payload_hash,
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

  if (last_modified != nullptr) {
    *last_modified = 0;
    curl_->easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_->easy_setopt(curl, CURLOPT_HEADERDATA, last_modified);
  }

  if (method == "DELETE") {
    curl_->easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  }

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
    const std::string detail = method + " " + canonical_uri_path + ": " +
                               curl_->easy_strerror(rc);
    SetError(detail);
    VCACHE_LOG("s3: " + detail);
    return false;
  }
  if (status == 404 || status == 403) {
    // A plain miss. 403 is lumped in here because S3 answers a missing key
    // that way on buckets without ListBucket permission, so it cannot be
    // told apart from a genuine permission problem by status alone -- which
    // means a real credential failure reads as a cold cache rather than as a
    // media error. Documented rather than guessed at.
    return false;
  }
  if (status < 200 || status >= 300) {
    const std::string detail = method + " " + canonical_uri_path + ": HTTP " +
                               std::to_string(status) + ": " +
                               response_body->substr(0, 200);
    SetError(detail);
    VCACHE_LOG("s3: " + detail);
    return false;
  }
  return true;
}

bool S3Storage::Get(const std::string& key, std::string* value) {
  ClearError();
  std::string body;
  std::time_t last_modified = 0;
  if (!ObjectRequest("GET", key, "", &body, &last_modified)) return false;

  // Age is checked on read as well as in Trim() so that expiry does not depend
  // on anyone remembering to trim, or on a bucket lifecycle rule being present
  // and correctly scoped. A last_modified of 0 means the header was missing or
  // unparseable, which is treated as "unknown age" and allowed through: losing
  // a good cache to a header quirk would be a worse failure than serving an
  // entry slightly past its window.
  if (config_.ttl_days > 0 && last_modified > 0) {
    const std::time_t age = std::time(nullptr) - last_modified;
    const std::time_t ttl =
        static_cast<std::time_t>(config_.ttl_days) * 24 * 60 * 60;
    if (age > ttl) {
      VCACHE_LOG("s3: " + ObjectKey(key) + " is " + std::to_string(age / 86400) +
                 " days old, past the " + std::to_string(config_.ttl_days) +
                 "-day ttl; treating as a miss");
      return false;
    }
  }

  *value = std::move(body);
  return true;
}

bool S3Storage::Put(const std::string& key, const std::string& value) {
  ClearError();
  if (read_only_) return false;
  std::string response;
  return ObjectRequest("PUT", key, value, &response, nullptr);
}

bool S3Storage::DeleteObject(const std::string& object_key) {
  std::string response;
  return Request("DELETE", object_key, /*query=*/"", /*payload=*/"", &response,
                 nullptr);
}

bool S3Storage::ListAll(std::vector<S3Object>* out) {
  std::string token;
  // A bucket shared with anything else could be enormous; the bound stops a
  // misconfigured prefix from turning a trim into an unbounded walk.
  constexpr int kMaxPages = 10000;
  for (int page = 0; page < kMaxPages; ++page) {
    // Canonical query strings must be sorted by key name, so build in that
    // order: continuation-token, list-type, prefix.
    std::string query;
    if (!token.empty()) {
      query += "continuation-token=" + sigv4::UriEncode(token, /*keep_slash=*/false);
      query += "&";
    }
    query += "list-type=2";
    if (!config_.prefix.empty()) {
      query += "&prefix=" + sigv4::UriEncode(config_.prefix, /*keep_slash=*/false);
    }

    // The listing is a bucket-level operation, so the path is the bucket root
    // rather than an object.
    std::string body;
    if (!Request("GET", /*canonical_uri_path=*/"", query, /*payload=*/"", &body,
                 nullptr)) {
      VCACHE_LOG("s3: listing failed; trim is incomplete");
      return false;
    }
    if (!ParseListObjectsV2(body, out, &token)) {
      VCACHE_LOG("s3: listing response was not a ListBucketResult");
      return false;
    }
    if (token.empty()) return true;
  }
  VCACHE_LOG("s3: listing exceeded the page bound; trim is incomplete");
  return false;
}

void S3Storage::Trim() {
  last_trim_ = TrimResult{};
  if (curl_ == nullptr || read_only_) return;
  if (config_.ttl_days <= 0 && config_.max_size == 0) return;  // nothing to enforce
  last_trim_.ran = true;

  std::vector<S3Object> objects;
  const bool listed_all = ListAll(&objects);
  last_trim_.listed = objects.size();
  for (const S3Object& o : objects) last_trim_.bytes_before += o.size;

  const std::time_t now = std::time(nullptr);
  const std::time_t ttl =
      static_cast<std::time_t>(config_.ttl_days) * 24 * 60 * 60;

  // Oldest first: expiry walks the front of this order and the byte budget
  // evicts from it, so one sort serves both.
  std::sort(objects.begin(), objects.end(),
            [](const S3Object& a, const S3Object& b) {
              return a.last_modified < b.last_modified;
            });

  bool all_deletes_ok = true;
  uint64_t remaining = last_trim_.bytes_before;
  std::vector<const S3Object*> survivors;
  survivors.reserve(objects.size());

  for (const S3Object& o : objects) {
    const bool expired = config_.ttl_days > 0 && o.last_modified > 0 &&
                         (now - o.last_modified) > ttl;
    if (!expired) {
      survivors.push_back(&o);
      continue;
    }
    if (DeleteObject(o.key)) {
      ++last_trim_.expired;
      last_trim_.bytes_deleted += o.size;
      remaining -= o.size;
    } else {
      all_deletes_ok = false;
      survivors.push_back(&o);
    }
  }

  // Evict down to 80% of the budget rather than exactly to it, so the next
  // trim is not triggered by a single store. Same reasoning as the disk layer.
  if (config_.max_size > 0 && remaining > config_.max_size) {
    const auto target = static_cast<uint64_t>(config_.max_size * 0.8);
    for (const S3Object* o : survivors) {
      if (remaining <= target) break;
      if (DeleteObject(o->key)) {
        ++last_trim_.evicted;
        last_trim_.bytes_deleted += o->size;
        remaining -= o->size;
      } else {
        all_deletes_ok = false;
      }
    }
  }

  last_trim_.complete = listed_all && all_deletes_ok;
  VCACHE_LOG("s3: trim listed " + std::to_string(last_trim_.listed) +
             " objects, deleted " + std::to_string(last_trim_.expired) +
             " expired and " + std::to_string(last_trim_.evicted) + " over budget");
}

}  // namespace vcache::storage
