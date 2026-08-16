// SPDX-License-Identifier: GPL-3.0-only
#include "core/config.h"

#include <cstdlib>
#include <sstream>

#include "util/fs.h"
#include "util/log.h"
#include "util/str.h"

#define TOML_EXCEPTIONS 0
#include "toml.hpp"

namespace vcache::core {
namespace {

std::optional<std::string> Env(const char* name) {
  const char* v = std::getenv(name);
  if (v == nullptr || *v == '\0') return std::nullopt;
  return std::string(v);
}

bool EnvBool(const char* name, bool fallback) {
  auto v = Env(name);
  if (!v) return fallback;
  const std::string lower = util::AsciiLower(*v);
  if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") return true;
  if (lower == "0" || lower == "false" || lower == "no" || lower == "off") return false;
  return fallback;
}

std::string DefaultDiskDir() {
  if (auto v = Env("XDG_CACHE_HOME")) return *v + "/vcache";
  if (auto home = Env("HOME")) return *home + "/.cache/vcache";
  return "/tmp/vcache";
}

std::string FindConfigFile() {
  if (auto v = Env("VCACHE_CONFIG")) return *v;
  if (auto v = Env("XDG_CONFIG_HOME")) {
    std::string p = *v + "/vcache/config.toml";
    if (util::FileExists(p)) return p;
  }
  if (auto home = Env("HOME")) {
    std::string p = *home + "/.config/vcache/config.toml";
    if (util::FileExists(p)) return p;
  }
  if (util::FileExists("/etc/vcache/config.toml")) return "/etc/vcache/config.toml";
  return "";
}

// toml++ node accessors that tolerate a missing or wrongly typed key.
std::optional<std::string> TomlString(const toml::table& t, std::string_view key) {
  if (auto node = t[key].as_string()) return std::string(node->get());
  return std::nullopt;
}

std::optional<bool> TomlBool(const toml::table& t, std::string_view key) {
  if (auto node = t[key].as_boolean()) return node->get();
  return std::nullopt;
}

std::optional<int64_t> TomlInt(const toml::table& t, std::string_view key) {
  if (auto node = t[key].as_integer()) return node->get();
  return std::nullopt;
}

std::vector<std::string> TomlStringArray(const toml::table& t, std::string_view key,
                                         std::vector<std::string>* warnings) {
  std::vector<std::string> out;
  auto arr = t[key].as_array();
  if (arr == nullptr) return out;
  for (const auto& element : *arr) {
    if (auto s = element.as_string()) {
      out.push_back(std::string(s->get()));
    } else if (warnings != nullptr) {
      warnings->push_back(std::string(key) + ": ignoring non-string array entry");
    }
  }
  return out;
}

void ApplyTomlFile(const std::string& path, Config* config) {
  auto text = util::ReadFile(path);
  if (!text) {
    config->warnings.push_back("could not read config file " + path);
    return;
  }
  toml::parse_result parsed = toml::parse(*text, path);
  if (!parsed) {
    std::ostringstream oss;
    oss << "config parse error at " << path << ":"
        << parsed.error().source().begin.line << ": "
        << parsed.error().description();
    config->warnings.push_back(oss.str());
    return;
  }
  config->loaded_from = path;
  const toml::table& root = parsed.table();

  if (auto disk = root["cache"]["disk"].as_table()) {
    if (auto v = TomlBool(*disk, "enabled")) config->disk.enabled = *v;
    if (auto v = TomlString(*disk, "dir")) config->disk.dir = util::ExpandTilde(*v);
    if (auto v = TomlString(*disk, "size")) {
      uint64_t bytes = 0;
      if (util::ParseSize(*v, &bytes)) {
        config->disk.max_size = bytes;
      } else {
        config->warnings.push_back("cache.disk.size: unparseable size '" + *v + "'");
      }
    } else if (auto n = TomlInt(*disk, "size")) {
      if (*n > 0) config->disk.max_size = static_cast<uint64_t>(*n);
    }
  }

  if (auto s3 = root["cache"]["s3"].as_table()) {
    // Presence of a bucket is what turns the S3 layer on.
    if (auto v = TomlString(*s3, "bucket")) {
      config->s3.bucket = *v;
      config->s3.enabled = true;
    }
    if (auto v = TomlBool(*s3, "enabled")) config->s3.enabled = *v;
    if (auto v = TomlString(*s3, "region")) config->s3.region = *v;
    if (auto v = TomlString(*s3, "prefix")) config->s3.prefix = *v;
    if (auto v = TomlString(*s3, "endpoint")) config->s3.endpoint = *v;
    if (auto v = TomlBool(*s3, "path_style")) config->s3.use_path_style = *v;
    if (auto v = TomlBool(*s3, "no_credentials")) config->s3.no_credentials = *v;
    if (auto v = TomlInt(*s3, "timeout")) {
      if (*v > 0) config->s3.timeout_seconds = static_cast<int>(*v);
    }
  }

  if (auto vc = root["vcache"].as_table()) {
    for (std::string& spec : TomlStringArray(*vc, "roots", &config->warnings)) {
      config->root_specs.push_back(std::move(spec));
    }
    if (auto v = TomlBool(*vc, "map_cwd")) config->map_cwd = *v;
    if (auto v = TomlString(*vc, "cwd_name")) config->cwd_canonical_name = *v;
    if (auto v = TomlBool(*vc, "read_only")) config->read_only = *v;
    for (std::string& name :
         TomlStringArray(*vc, "hash_env_vars", &config->warnings)) {
      config->extra_env_vars.push_back(std::move(name));
    }
    if (auto v = TomlString(*vc, "incoming_prefix_maps")) {
      if (!ParseIncomingMapPolicy(*v, &config->incoming_map_policy)) {
        config->warnings.push_back(
            "vcache.incoming_prefix_maps: unknown policy '" + *v +
            "' (expected error, strip or keep)");
      }
    }
    if (auto v = TomlString(*vc, "native_target")) {
      if (!ParseNativeTargetPolicy(*v, &config->native_target_policy)) {
        config->warnings.push_back("vcache.native_target: unknown policy '" + *v +
                                   "' (expected resolve or uncacheable)");
      }
    }
    if (auto v = TomlString(*vc, "dep_scan")) {
      if (!ParseDepScanPolicy(*v, &config->dep_scan_policy)) {
        config->warnings.push_back("vcache.dep_scan: unknown policy '" + *v +
                                   "' (expected manifest or uncacheable)");
      }
    }
  }
}

void ApplyEnvironment(Config* config) {
  if (auto v = Env("VCACHE_DIR")) config->disk.dir = util::ExpandTilde(*v);
  if (auto v = Env("VCACHE_CACHE_SIZE")) {
    uint64_t bytes = 0;
    if (util::ParseSize(*v, &bytes)) {
      config->disk.max_size = bytes;
    } else {
      config->warnings.push_back("VCACHE_CACHE_SIZE: unparseable size '" + *v + "'");
    }
  }
  config->disk.enabled = EnvBool("VCACHE_DISK", config->disk.enabled);

  if (auto v = Env("VCACHE_S3_BUCKET")) {
    config->s3.bucket = *v;
    config->s3.enabled = true;
  }
  if (auto v = Env("VCACHE_S3_REGION")) config->s3.region = *v;
  if (auto v = Env("VCACHE_S3_PREFIX")) config->s3.prefix = *v;
  if (auto v = Env("VCACHE_S3_ENDPOINT")) config->s3.endpoint = *v;
  config->s3.use_path_style = EnvBool("VCACHE_S3_PATH_STYLE", config->s3.use_path_style);
  config->s3.no_credentials =
      EnvBool("VCACHE_S3_NO_CREDENTIALS", config->s3.no_credentials);

  // Standard AWS variables, so existing credential setups work unchanged.
  if (auto v = Env("AWS_ACCESS_KEY_ID")) config->s3.access_key = *v;
  if (auto v = Env("AWS_SECRET_ACCESS_KEY")) config->s3.secret_key = *v;
  if (auto v = Env("AWS_SESSION_TOKEN")) config->s3.session_token = *v;
  if (auto v = Env("AWS_DEFAULT_REGION")) config->s3.region = *v;
  if (auto v = Env("AWS_REGION")) config->s3.region = *v;

  // VCACHE_ROOTS is colon-separated, like PATH. Entries may carry an "=TARGET".
  if (auto v = Env("VCACHE_ROOTS")) {
    for (std::string& spec : util::Split(*v, ':', /*skip_empty=*/true)) {
      config->root_specs.push_back(std::move(spec));
    }
  }
  config->map_cwd = EnvBool("VCACHE_MAP_CWD", config->map_cwd);
  if (auto v = Env("VCACHE_CWD_NAME")) config->cwd_canonical_name = *v;

  if (auto v = Env("VCACHE_HASH_ENV_VARS")) {
    for (std::string& name : util::Split(*v, ',', /*skip_empty=*/true)) {
      config->extra_env_vars.push_back(util::TrimWhitespace(name));
    }
  }

  config->disabled = EnvBool("VCACHE_DISABLE", config->disabled);
  config->read_only = EnvBool("VCACHE_READONLY", config->read_only);
  config->recache = EnvBool("VCACHE_RECACHE", config->recache);

  if (auto v = Env("VCACHE_INCOMING_PREFIX_MAPS")) {
    if (!ParseIncomingMapPolicy(*v, &config->incoming_map_policy)) {
      config->warnings.push_back(
          "VCACHE_INCOMING_PREFIX_MAPS: unknown policy '" + *v +
          "' (expected error, strip or keep)");
    }
  }

  if (auto v = Env("VCACHE_NATIVE_TARGET")) {
    if (!ParseNativeTargetPolicy(*v, &config->native_target_policy)) {
      config->warnings.push_back("VCACHE_NATIVE_TARGET: unknown policy '" + *v +
                                 "' (expected resolve or uncacheable)");
    }
  }

  if (auto v = Env("VCACHE_DEP_SCAN")) {
    if (!ParseDepScanPolicy(*v, &config->dep_scan_policy)) {
      config->warnings.push_back("VCACHE_DEP_SCAN: unknown policy '" + *v +
                                 "' (expected manifest or uncacheable)");
    }
  }
}

}  // namespace

bool ParseNativeTargetPolicy(std::string_view name, NativeTargetPolicy* out) {
  if (name == "resolve") {
    *out = NativeTargetPolicy::kResolve;
    return true;
  }
  if (name == "uncacheable") {
    *out = NativeTargetPolicy::kUncacheable;
    return true;
  }
  return false;
}

const char* NativeTargetPolicyName(NativeTargetPolicy policy) {
  switch (policy) {
    case NativeTargetPolicy::kResolve: return "resolve";
    case NativeTargetPolicy::kUncacheable: return "uncacheable";
  }
  return "resolve";
}

bool ParseDepScanPolicy(std::string_view name, DepScanPolicy* out) {
  if (name == "manifest") {
    *out = DepScanPolicy::kManifest;
    return true;
  }
  if (name == "uncacheable" || name == "off") {
    *out = DepScanPolicy::kUncacheable;
    return true;
  }
  return false;
}

const char* DepScanPolicyName(DepScanPolicy policy) {
  switch (policy) {
    case DepScanPolicy::kManifest: return "manifest";
    case DepScanPolicy::kUncacheable: return "uncacheable";
  }
  return "manifest";
}

Config LoadConfig() {
  Config config;
  config.disk.dir = DefaultDiskDir();

  const std::string path = FindConfigFile();
  if (!path.empty()) ApplyTomlFile(path, &config);
  ApplyEnvironment(&config);

  if (config.disk.dir.empty()) config.disk.dir = DefaultDiskDir();
  if (config.s3.enabled && config.s3.bucket.empty()) {
    config.warnings.push_back("s3 cache enabled but no bucket configured; disabling");
    config.s3.enabled = false;
  }
  if (config.s3.enabled && !config.s3.no_credentials &&
      (config.s3.access_key.empty() || config.s3.secret_key.empty())) {
    config.warnings.push_back(
        "s3 cache enabled but AWS credentials are not set; disabling");
    config.s3.enabled = false;
  }
  return config;
}

std::string DescribeConfig(const Config& config) {
  std::ostringstream out;
  out << "config file:      " << (config.loaded_from.empty() ? "(none)" : config.loaded_from) << "\n";
  out << "disk cache:       " << (config.disk.enabled ? "enabled" : "disabled") << "\n";
  out << "  dir:            " << config.disk.dir << "\n";
  out << "  max size:       " << config.disk.max_size << " bytes\n";
  out << "s3 cache:         " << (config.s3.enabled ? "enabled" : "disabled") << "\n";
  if (config.s3.enabled) {
    out << "  bucket:         " << config.s3.bucket << "\n";
    out << "  region:         " << config.s3.region << "\n";
    out << "  prefix:         " << config.s3.prefix << "\n";
    if (!config.s3.endpoint.empty()) out << "  endpoint:       " << config.s3.endpoint << "\n";
    out << "  path style:     " << (config.s3.use_path_style ? "yes" : "no") << "\n";
    out << "  credentials:    "
        << (config.s3.no_credentials ? "anonymous" : "from environment") << "\n";
  }
  out << "roots:            "
      << (config.root_specs.empty() ? "(none)" : util::Join(config.root_specs, ", ")) << "\n";
  out << "map cwd:          " << (config.map_cwd ? "yes" : "no") << "\n";
  out << "incoming maps:    " << IncomingMapPolicyName(config.incoming_map_policy)
      << "\n";
  out << "native target:    " << NativeTargetPolicyName(config.native_target_policy)
      << "\n";
  out << "dependency scans: " << DepScanPolicyName(config.dep_scan_policy) << "\n";
  out << "read only:        " << (config.read_only ? "yes" : "no") << "\n";
  out << "disabled:         " << (config.disabled ? "yes" : "no") << "\n";
  if (!config.extra_env_vars.empty()) {
    out << "hashed env vars:  " << util::Join(config.extra_env_vars, ", ") << "\n";
  }
  for (const std::string& w : config.warnings) out << "warning: " << w << "\n";
  return out.str();
}

}  // namespace vcache::core
