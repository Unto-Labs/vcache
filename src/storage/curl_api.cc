// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/curl_api.h"

#include <dlfcn.h>

#include <mutex>

#include "util/log.h"

namespace vcache::storage {
namespace {

// Tried in order. Debian and Ubuntu ship a GnuTLS-linked build under a
// different soname, and some distributions only install the unversioned symlink
// with the -dev package.
constexpr const char* kCandidates[] = {
    "libcurl.so.4",
    "libcurl-gnutls.so.4",
    "libcurl-nss.so.4",
    "libcurl.so",
};

std::once_flag g_once;
CurlApi* g_api = nullptr;
std::string* g_error = nullptr;

// Resolves `name` into `slot`. Records the first failure in `error`.
template <typename Fn>
bool Resolve(void* handle, const char* name, Fn* slot, std::string* error) {
  // The cast through void* is the documented way to use dlsym for functions;
  // POSIX guarantees it works even though C++ has no direct conversion.
  void* symbol = ::dlsym(handle, name);
  if (symbol == nullptr) {
    if (error->empty()) {
      *error = std::string("libcurl is missing symbol ") + name;
    }
    return false;
  }
  *slot = reinterpret_cast<Fn>(symbol);
  return true;
}

void DoLoad() {
  g_error = new std::string();

  void* handle = nullptr;
  std::string attempts;
  for (const char* candidate : kCandidates) {
    // RTLD_GLOBAL so libcurl's own dependencies resolve normally; RTLD_NOW so a
    // missing symbol surfaces here rather than mid-request.
    handle = ::dlopen(candidate, RTLD_NOW | RTLD_GLOBAL);
    if (handle != nullptr) {
      VCACHE_LOG(std::string("loaded ") + candidate);
      break;
    }
    if (!attempts.empty()) attempts += ", ";
    attempts += candidate;
  }
  if (handle == nullptr) {
    *g_error = "could not load libcurl (tried " + attempts +
               "); install libcurl4 or equivalent to use the S3 cache";
    return;
  }

  auto* api = new CurlApi();
  bool ok = true;
  ok &= Resolve(handle, "curl_global_init", &api->global_init, g_error);
  ok &= Resolve(handle, "curl_easy_init", &api->easy_init, g_error);
  ok &= Resolve(handle, "curl_easy_setopt", &api->easy_setopt, g_error);
  ok &= Resolve(handle, "curl_easy_perform", &api->easy_perform, g_error);
  ok &= Resolve(handle, "curl_easy_getinfo", &api->easy_getinfo, g_error);
  ok &= Resolve(handle, "curl_easy_cleanup", &api->easy_cleanup, g_error);
  ok &= Resolve(handle, "curl_easy_strerror", &api->easy_strerror, g_error);
  ok &= Resolve(handle, "curl_slist_append", &api->slist_append, g_error);
  ok &= Resolve(handle, "curl_slist_free_all", &api->slist_free_all, g_error);

  if (!ok) {
    delete api;
    // The handle is deliberately not closed: unloading after a partial resolve
    // buys nothing and risks running libcurl's destructors in a bad state.
    return;
  }

  api->global_init(CURL_GLOBAL_DEFAULT);
  g_api = api;
}

}  // namespace

const CurlApi* CurlApi::Load(std::string* error) {
  std::call_once(g_once, DoLoad);
  if (g_api == nullptr && error != nullptr) {
    *error = (g_error != nullptr) ? *g_error : "libcurl could not be loaded";
  }
  return g_api;
}

bool CurlApi::loaded() { return g_api != nullptr; }

}  // namespace vcache::storage
