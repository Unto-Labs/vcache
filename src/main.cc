// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: GPL-3.0-only
// vcache entry point.
//
// Two invocation styles, matching ccache and sccache:
//   vcache g++ -c foo.cc -o foo.o     explicit prefix
//   ln -s vcache g++; g++ -c foo.cc   masquerade via a symlink on $PATH

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "args/compiler_args.h"
#include "args/rustc_args.h"
#include "core/compile.h"
#include "core/config.h"
#include "core/roots.h"
#include "core/stats.h"
#include "rust/rust_compile.h"
#include "storage/chain.h"
#include "storage/disk_storage.h"
#include "storage/s3_storage.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/str.h"
#include "util/subprocess.h"

namespace {

constexpr const char* kVersion = "vcache 0.4.3";

void PrintUsage() {
  std::printf(
      "%s -- compilation cache for C, C++ and Rust\n"
      "\n"
      "Usage:\n"
      "  vcache <compiler> [args...]      cache one compilation\n"
      "  vcache <option>\n"
      "\n"
      "Options:\n"
      "  -h, --help            show this help\n"
      "  -V, --version         show the version\n"
      "  -s, --show-stats      show cache statistics\n"
      "  -z, --zero-stats      reset statistics counters\n"
      "  -C, --clear           delete all cached entries\n"
      "      --show-config     show the effective configuration\n"
      "      --show-roots      show the root mapping for the current directory\n"
      "      --error-on-cache-media-failure\n"
      "                        exit non-zero if a cache layer is broken (as\n"
      "                        opposed to merely cold); off by default\n"
      "      --trim            evict entries until the cache is under its limit\n"
      "                        (disk; also expires and caps s3 when configured)\n"
      "\n"
      "Root mapping (the point of vcache):\n"
      "  --vcache-root=PATH[=TARGET]   repeatable; may also be given as\n"
      "                                VCACHE_ROOTS=path1:path2\n"
      "\n"
      "  --vcache-allow-prefix-maps    accept a caller-supplied\n"
      "                                -ffile-prefix-map/--remap-path-prefix\n"
      "                                and drop it in favour of vcache's own\n"
      "  --vcache-incoming-prefix-maps=error|strip|keep\n"
      "                                what to do with such flags; the default\n"
      "                                is error, because vcache owns path\n"
      "                                rewriting and silently overriding the\n"
      "                                caller's mapping would change the build\n"
      "\n"
      "  Each root is rewritten to a stable canonical prefix via\n"
      "  -ffile-prefix-map (or --remap-path-prefix for rustc), which makes the\n"
      "  compiler's output byte-identical regardless of where the tree lives.\n"
      "  Without this, absolute paths leak into linemarkers, __FILE__ and debug\n"
      "  info, and no two checkouts ever share a cache entry.\n"
      "\n"
      "Environment:\n"
      "  VCACHE_DIR, VCACHE_CACHE_SIZE, VCACHE_ROOTS, VCACHE_MAP_CWD,\n"
      "  VCACHE_DISABLE, VCACHE_READONLY, VCACHE_RECACHE, VCACHE_LOG,\n"
      "  VCACHE_ERROR_ON_CACHE_MEDIA_FAILURE,\n"
      "  VCACHE_COMPILER_CHECK, VCACHE_S3_BUCKET, VCACHE_S3_REGION,\n"
      "  VCACHE_S3_PREFIX, VCACHE_S3_ENDPOINT, VCACHE_S3_TTL_DAYS,\n"
      "  VCACHE_S3_CACHE_SIZE, AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY\n"
      "\n"
      "Config file: $VCACHE_CONFIG, ~/.config/vcache/config.toml, or\n"
      "             /etc/vcache/config.toml\n",
      kVersion);
}

// Builds the cache chain: local disk first, then S3, so a remote hit is
// promoted into the local layer for the rest of the build.
std::unique_ptr<vcache::storage::CacheChain> BuildChain(
    const vcache::core::Config& config) {
  auto chain = std::make_unique<vcache::storage::CacheChain>();
  if (config.disk.enabled) {
    chain->AddLayer(std::make_unique<vcache::storage::DiskStorage>(
        config.disk.dir, config.disk.max_size, config.read_only));
  }
  if (config.s3.enabled) {
    auto s3 = std::make_unique<vcache::storage::S3Storage>(config.s3);
    if (s3->available()) {
      if (config.read_only) s3->set_read_only(true);
      chain->AddLayer(std::move(s3));
    } else {
      // The S3 layer was asked for and cannot be provided. Say so once rather
      // than silently degrading to a disk-only cache.
      ::fprintf(stderr, "vcache: warning: s3 cache disabled: %s\n",
                s3->load_error().c_str());
    }
  }
  return chain;
}

void ReportWarnings(const std::vector<std::string>& warnings) {
  for (const std::string& w : warnings) {
    ::fprintf(stderr, "vcache: warning: %s\n", w.c_str());
  }
}

// Pulls --vcache-* options off the front of the command line. Everything from
// the first non-vcache argument onward is the compiler command.
struct FrontOptions {
  std::vector<std::string> root_specs;
  // Command-line override for the incoming-prefix-map policy; takes precedence
  // over both the config file and the environment.
  std::optional<vcache::core::IncomingMapPolicy> incoming_map_policy;
  // Command line only ever turns this on; leaving it off is what the config
  // file and environment are for, so there is no --no- form to reason about.
  bool error_on_cache_media_failure = false;
  std::vector<std::string> errors;
  size_t consumed = 1;  // index into argv where the compiler command starts
};

FrontOptions ParseFrontOptions(int argc, char** argv) {
  FrontOptions opts;
  size_t i = 1;

  auto set_policy = [&opts](const std::string& value) {
    vcache::core::IncomingMapPolicy policy;
    if (vcache::core::ParseIncomingMapPolicy(value, &policy)) {
      opts.incoming_map_policy = policy;
    } else {
      opts.errors.push_back("unknown incoming-prefix-map policy '" + value +
                            "' (expected error, strip or keep)");
    }
  };

  for (; i < static_cast<size_t>(argc); ++i) {
    const std::string arg = argv[i];
    if (vcache::util::StartsWith(arg, "--vcache-root=")) {
      opts.root_specs.push_back(arg.substr(std::strlen("--vcache-root=")));
      continue;
    }
    if (arg == "--vcache-root" && i + 1 < static_cast<size_t>(argc)) {
      opts.root_specs.push_back(argv[++i]);
      continue;
    }
    if (vcache::util::StartsWith(arg, "--vcache-incoming-prefix-maps=")) {
      set_policy(arg.substr(std::strlen("--vcache-incoming-prefix-maps=")));
      continue;
    }
    if (arg == "--vcache-incoming-prefix-maps" && i + 1 < static_cast<size_t>(argc)) {
      set_policy(argv[++i]);
      continue;
    }
    // Shorthand for the common override: accept the caller's flags and let
    // vcache replace them.
    if (arg == "--vcache-allow-prefix-maps") {
      opts.incoming_map_policy = vcache::core::IncomingMapPolicy::kStrip;
      continue;
    }
    if (arg == "--error-on-cache-media-failure") {
      opts.error_on_cache_media_failure = true;
      continue;
    }
    break;
  }
  opts.consumed = i;
  return opts;
}

int ShowStats(const vcache::core::Config& config) {
  vcache::storage::DiskStorage disk(config.disk.dir, config.disk.max_size,
                                    /*read_only=*/true);
  const vcache::core::Stats stats = vcache::core::ReadStats(config.disk.dir);
  std::string text = vcache::core::FormatStats(stats, config.disk.dir,
                                               disk.TotalSize(), config.disk.max_size);
  std::fputs(text.c_str(), stdout);
  if (config.s3.enabled) {
    std::printf("s3 backend         %s (region %s)\n", config.s3.bucket.c_str(),
                config.s3.region.c_str());
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  vcache::util::InitLogging();

  if (argc < 2) {
    PrintUsage();
    return 1;
  }

  // Masquerade mode: invoked through a symlink named after the compiler.
  const std::string self_base = vcache::util::BaseName(argv[0]);
  const bool masquerading = (self_base != "vcache");

  std::vector<std::string> command;
  vcache::core::Config config = vcache::core::LoadConfig();

  if (masquerading) {
    // Resolve the real compiler on $PATH, skipping this binary so the symlink
    // cannot invoke itself.
    const std::string self_real =
        vcache::util::RealPath("/proc/self/exe").value_or("");
    auto real = vcache::util::FindInPath(self_base, self_real);
    if (!real) {
      ::fprintf(stderr, "vcache: cannot find a real %s on PATH\n", self_base.c_str());
      return 127;
    }
    command.push_back(*real);
    for (int i = 1; i < argc; ++i) command.push_back(argv[i]);
  } else {
    const std::string first = argv[1];

    if (first == "-h" || first == "--help") { PrintUsage(); return 0; }
    if (first == "-V" || first == "--version") {
      // The notice GPLv3's "How to Apply These Terms" asks for, in the shape
      // GNU tools use it.
      std::printf(
          "%s\n"
          "Copyright (C) 2026 Vlad Petric\n"
          "License GPLv3: GNU GPL version 3 "
          "<https://gnu.org/licenses/gpl-3.0.html>.\n"
          "This is free software: you are free to change and redistribute it.\n"
          "There is NO WARRANTY, to the extent permitted by law.\n"
          "\n"
          "Vendored components under third-party/ keep their own licences:\n"
          "BLAKE3 (Apache 2.0 with LLVM exception), toml++ (MIT), Boost "
          "(BSL-1.0),\n"
          "gperftools (BSD 3-clause).\n",
          kVersion);
      return 0;
    }
    if (first == "-s" || first == "--show-stats") return ShowStats(config);
    if (first == "-z" || first == "--zero-stats") {
      return vcache::core::ZeroStats(config.disk.dir) ? 0 : 1;
    }
    if (first == "-C" || first == "--clear") {
      vcache::storage::DiskStorage disk(config.disk.dir, config.disk.max_size, false);
      const bool ok = disk.Clear();
      vcache::core::ZeroStats(config.disk.dir);
      std::printf("%s cache at %s\n", ok ? "cleared" : "failed to clear",
                  config.disk.dir.c_str());
      return ok ? 0 : 1;
    }
    if (first == "--show-config") {
      std::fputs(vcache::core::DescribeConfig(config).c_str(), stdout);
      return 0;
    }
    if (first == "--trim") {
      vcache::storage::DiskStorage disk(config.disk.dir, config.disk.max_size, false);
      disk.Trim();
      std::printf("trimmed cache at %s\n", config.disk.dir.c_str());

      // The S3 layer is trimmed only here, never during a compile: it costs a
      // bucket listing, and on a shared bucket evicting another machine's
      // entries is not a decision an ordinary build should be taking.
      if (config.s3.enabled) {
        auto s3 = std::make_unique<vcache::storage::S3Storage>(config.s3);
        if (!s3->available()) {
          ::fprintf(stderr, "vcache: warning: s3 not trimmed: %s\n",
                    s3->load_error().c_str());
          return 1;
        }
        if (config.read_only) s3->set_read_only(true);
        s3->Trim();
        const auto& t = s3->last_trim();
        if (!t.ran) {
          std::printf("s3: nothing to enforce (no ttl, no size cap)\n");
        } else {
          std::printf(
              "s3: listed %llu objects (%llu bytes), deleted %llu expired and "
              "%llu over budget (%llu bytes)%s\n",
              static_cast<unsigned long long>(t.listed),
              static_cast<unsigned long long>(t.bytes_before),
              static_cast<unsigned long long>(t.expired),
              static_cast<unsigned long long>(t.evicted),
              static_cast<unsigned long long>(t.bytes_deleted),
              t.complete ? "" : " -- INCOMPLETE, see log");
          if (!t.complete) return 1;
        }
      }
      return 0;
    }

    FrontOptions front = ParseFrontOptions(argc, argv);
    if (!front.errors.empty()) {
      for (const std::string& e : front.errors) {
        ::fprintf(stderr, "vcache: %s\n", e.c_str());
      }
      return 1;
    }
    for (std::string& spec : front.root_specs) {
      config.root_specs.push_back(std::move(spec));
    }
    if (front.error_on_cache_media_failure) {
      config.error_on_cache_media_failure = true;
    }
    if (front.incoming_map_policy.has_value()) {
      config.incoming_map_policy = *front.incoming_map_policy;
    }

    if (front.consumed < static_cast<size_t>(argc) &&
        std::string(argv[front.consumed]) == "--show-roots") {
      std::vector<std::string> warnings;
      vcache::core::RootMap roots = vcache::core::BuildRootMap(config, &warnings);
      ReportWarnings(warnings);
      for (const auto& root : roots.roots()) {
        std::printf("%s -> %s\n", root.path.c_str(), root.canonical.c_str());
      }
      return 0;
    }

    for (size_t i = front.consumed; i < static_cast<size_t>(argc); ++i) {
      command.push_back(argv[i]);
    }
  }

  if (command.empty()) {
    PrintUsage();
    return 1;
  }

  ReportWarnings(config.warnings);

  if (config.disabled) {
    VCACHE_LOG("disabled via configuration; running compiler directly");
    vcache::util::ProcResult result = vcache::util::Run(command);
    return result.exit_code < 0 ? 127 : result.exit_code;
  }

  std::vector<std::string> warnings;
  const vcache::core::RootMap roots = vcache::core::BuildRootMap(config, &warnings);
  ReportWarnings(warnings);

  if (roots.empty()) {
    // Without a mapping vcache still caches correctly, it just cannot share
    // entries between directories, which is the whole point of the tool.
    VCACHE_LOG("no roots configured; cross-directory hits will not occur");
  }

  std::unique_ptr<vcache::storage::CacheChain> chain = BuildChain(config);
  if (chain->empty()) {
    VCACHE_LOG("no cache layers enabled; running compiler directly");
    vcache::util::ProcResult result = vcache::util::Run(command);
    return result.exit_code < 0 ? 127 : result.exit_code;
  }

  if (vcache::args::LooksLikeRustc(command[0])) {
    return vcache::rust::RunRustCompile(command, config, roots, chain.get());
  }
  return vcache::core::RunCompile(command, config, roots, chain.get());
}
