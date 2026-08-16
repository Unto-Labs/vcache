// SPDX-License-Identifier: GPL-3.0-or-later
// Configuration, layered the way sccache does it: a TOML file supplies the
// defaults and environment variables override individual settings, so a CI job
// can point at a different bucket without rewriting the file.
//
// Search order for the config file:
//   $VCACHE_CONFIG
//   $XDG_CONFIG_HOME/vcache/config.toml  (or ~/.config/vcache/config.toml)
//   /etc/vcache/config.toml
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/roots.h"

namespace vcache::core {

struct DiskCacheConfig {
  bool enabled = true;
  std::string dir;              // default ~/.cache/vcache
  uint64_t max_size = 10ull << 30;  // 10 GiB
};

struct S3CacheConfig {
  bool enabled = false;
  std::string bucket;
  std::string region = "us-east-1";
  std::string prefix;    // key prefix inside the bucket
  std::string endpoint;  // optional; derived from region when empty
  bool use_path_style = false;   // required by MinIO and similar
  bool no_credentials = false;   // anonymous, read-only public buckets
  int timeout_seconds = 30;

  // Resolved from the environment (AWS_*) at load time.
  std::string access_key;
  std::string secret_key;
  std::string session_token;
};

// What to do about -march=native / -mtune=native / -mcpu=native.
enum class NativeTargetPolicy {
  // Ask the compiler which target it resolved to and put that in the key. Two
  // machines with different CPUs then derive different keys instead of sharing
  // an entry that only one of them can use.
  kResolve,
  // Do not cache such compilations at all. Costs the hit but avoids the extra
  // compiler probe, and is the conservative choice for a cache directory shared
  // between machines whose hostnames do not distinguish their CPUs.
  kUncacheable,
};

bool ParseNativeTargetPolicy(std::string_view name, NativeTargetPolicy* out);
const char* NativeTargetPolicyName(NativeTargetPolicy policy);

// What to do about -M/-MM runs, which produce a dependency list and no object.
enum class DepScanPolicy {
  // Cache them against a manifest of the files the scan read. A hit costs one
  // hash per file instead of a preprocessor run.
  kManifest,
  // Run the compiler. Preprocessing to derive an ordinary key would cost more
  // than the run being cached, so this is the same as "do not cache".
  kUncacheable,
};

bool ParseDepScanPolicy(std::string_view name, DepScanPolicy* out);
const char* DepScanPolicyName(DepScanPolicy policy);

struct Config {
  DiskCacheConfig disk;
  S3CacheConfig s3;

  // Root specs in "PATH" or "PATH=TARGET" form, before RootMap resolution.
  std::vector<std::string> root_specs;

  // Bring the current directory under canonicalisation when it is not already
  // inside a root. DW_AT_comp_dir records the cwd, so leaving it unmapped costs
  // every cross-directory hit whenever -g is in play.
  bool map_cwd = true;
  std::string cwd_canonical_name = "cwd";

  // Defaults to kError: vcache owns path rewriting, and silently overriding a
  // prefix map the build system deliberately passed would change that build's
  // output without telling anyone. Overridable from config, environment or the
  // command line, in that order of increasing precedence.
  IncomingMapPolicy incoming_map_policy = IncomingMapPolicy::kError;

  NativeTargetPolicy native_target_policy = NativeTargetPolicy::kResolve;

  DepScanPolicy dep_scan_policy = DepScanPolicy::kManifest;

  bool disabled = false;   // VCACHE_DISABLE: run the compiler, skip the cache
  bool read_only = false;  // look up but never store
  bool recache = false;    // ignore hits, recompile and overwrite

  // Extra environment variable names to mix into the cache key, for builds
  // where a variable affects codegen (SOURCE_DATE_EPOCH, for example).
  std::vector<std::string> extra_env_vars;

  // Path the config was loaded from, for --show-config. Empty if defaults only.
  std::string loaded_from;

  // Non-fatal problems encountered while loading.
  std::vector<std::string> warnings;
};

// Loads config from file plus environment. Never fails outright: on a malformed
// file it records a warning and returns defaults, because breaking the build
// over a config typo is worse than caching nothing.
Config LoadConfig();

// Renders the effective configuration for --show-config.
std::string DescribeConfig(const Config& config);

}  // namespace vcache::core
