// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: GPL-3.0-only
// Lazily loaded libcurl bindings.
//
// vcache runs once per compilation, so every DT_NEEDED entry is a library the
// dynamic loader maps and relocates on every invocation. Linking libcurl
// directly pulls in around thirty shared objects -- TLS, HTTP/2, Kerberos,
// LDAP, SSH, compression, IDN -- which cost roughly 2.4 ms of process startup
// each time, whether or not the S3 layer is configured. Most builds are
// disk-only and never make a request.
//
// So libcurl is opened with dlopen() the first time an S3 layer is actually
// constructed. Disk-only builds never touch it, and `ldd bin/vcache` lists only
// libc, libm and the loader.
//
// The header is still used at compile time for the CURL types and CURLOPT_
// constants; only the link-time dependency is removed.
#pragma once

#include <string>

// curl's typecheck-gcc.h redefines curl_easy_setopt and curl_easy_getinfo as
// macros, which would rewrite calls made through our function pointers.
#define CURL_DISABLE_TYPECHECK 1
#include <curl/curl.h>

namespace vcache::storage {

// The subset of libcurl vcache calls. All members are non-null when
// CurlApi::Load() succeeds.
struct CurlApi {
  CURLcode (*global_init)(long flags) = nullptr;
  CURL* (*easy_init)() = nullptr;
  CURLcode (*easy_setopt)(CURL* handle, CURLoption option, ...) = nullptr;
  CURLcode (*easy_perform)(CURL* handle) = nullptr;
  CURLcode (*easy_getinfo)(CURL* handle, CURLINFO info, ...) = nullptr;
  void (*easy_cleanup)(CURL* handle) = nullptr;
  const char* (*easy_strerror)(CURLcode code) = nullptr;
  curl_slist* (*slist_append)(curl_slist* list, const char* value) = nullptr;
  void (*slist_free_all)(curl_slist* list) = nullptr;

  // Loads libcurl if it is not already loaded and returns the resolved API, or
  // nullptr on failure with a human-readable reason in `error`. The result is
  // memoised, including failure, so a broken installation is diagnosed once.
  // Thread-safe. curl_global_init is called exactly once on first success.
  static const CurlApi* Load(std::string* error);

  // True if a previous Load() succeeded. Does not attempt to load.
  static bool loaded();
};

}  // namespace vcache::storage
