// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "rust/rust_compile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "args/rustc_args.h"
#include "core/compile.h"
#include "core/depfile.h"
#include "core/stats.h"
#include "hash/hasher.h"
#include "storage/storage.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/str.h"
#include "util/subprocess.h"

namespace fs = std::filesystem;

namespace vcache::rust {
namespace {

using core::Counter;
using core::MapDirection;
using core::RootMap;

constexpr std::string_view kCacheKeyVersion = "vcache-rust-key-v1";

int RunPassthrough(const std::vector<std::string>& argv) {
  VCACHE_LOG("rust passthrough: " + util::Join(argv, " "));
  util::ProcResult result = util::Run(argv);
  if (result.exit_code < 0) {
    ::fprintf(stderr, "vcache: failed to execute %s\n", argv[0].c_str());
    return 127;
  }
  return result.exit_code;
}

// Identity of the rustc toolchain. `rustc -vV` reports version, commit hash and
// host triple, all machine-independent, so entries stay shareable through S3.
std::string ResolveRustcFingerprint(const std::string& rustc,
                                    const std::string& cache_dir) {
  const std::string real = util::RealPath(rustc).value_or(rustc);
  uint64_t size = util::FileSize(real).value_or(0);

  hash::Hasher memo_key;
  memo_key.UpdateDelimited("rustc-version-memo-v1");
  memo_key.UpdateDelimited(real);
  memo_key.UpdateU64(size);
  const std::string memo_path = cache_dir + "/compilers/" + memo_key.Hex();

  if (auto cached = util::ReadFile(memo_path)) return hash::HashString(*cached);

  util::ProcResult probe =
      util::Run({rustc, "-vV"}, {.capture_stdout = true, .capture_stderr = true});
  const std::string banner = probe.stdout_data + probe.stderr_data;
  if (probe.exit_code != 0 || banner.empty()) return hash::HashString(real);

  util::WriteFileAtomic(memo_path, banner);
  return hash::HashString(banner);
}

// Asks rustc which files this crate reads. Returns the source paths, or an
// empty vector on failure.
std::vector<std::string> CollectSourceFiles(const args::RustcArgs& parsed,
                                            const RootMap& roots,
                                            const std::string& temp_dir) {
  const std::string dep_dir = temp_dir + "/depinfo";
  if (!util::MakeDirs(dep_dir)) return {};

  std::vector<std::string> cmd;
  cmd.push_back(parsed.compiler);
  for (const std::string& arg : parsed.base_args) cmd.push_back(arg);
  cmd.push_back("--emit=dep-info");
  cmd.push_back("--out-dir");
  cmd.push_back(dep_dir);
  cmd.push_back(parsed.source);

  VCACHE_LOG("rust dep-info: " + util::Join(cmd, " "));
  util::ProcResult result = util::Run(cmd, {.capture_stderr = true});
  if (result.exit_code != 0) {
    VCACHE_LOG("rust dep-info failed: " + result.stderr_data.substr(0, 512));
    return {};
  }

  std::vector<std::string> sources;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dep_dir, ec)) {
    if (ec) break;
    if (!entry.is_regular_file()) continue;
    auto text = util::ReadFile(entry.path().string());
    if (!text) continue;
    auto dep = core::ParseDepFile(*text);
    if (!dep) {
      VCACHE_LOG("rust dep-info did not parse");
      return {};
    }
    for (const core::DepRule& rule : dep->rules) {
      for (const std::string& prereq : rule.prerequisites) sources.push_back(prereq);
    }
  }

  // rustc repeats each source across several rules; one hash per file is
  // enough, and a stable order keeps the key deterministic.
  std::sort(sources.begin(), sources.end());
  sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
  return sources;
}

std::string ComputeKey(const args::RustcArgs& parsed, const RootMap& roots,
                       const std::string& rustc_fingerprint,
                       const core::Config& config,
                       const std::vector<std::string>& sources) {
  hash::Hasher hasher;
  hasher.UpdateDelimited(kCacheKeyVersion);
  hasher.UpdateDelimited(rustc_fingerprint);
  hasher.UpdateDelimited(roots.Fingerprint());

  for (const std::string& arg : parsed.key_args) {
    hasher.UpdateDelimited(roots.Canonicalize(arg));
  }
  // --emit decides which artifacts land in the output directory, so it is part
  // of the key even though vcache passes it separately.
  std::vector<std::string> emit = parsed.emit_kinds;
  std::sort(emit.begin(), emit.end());
  for (const std::string& kind : emit) hasher.UpdateDelimited(kind);

  // Every reachable source: canonical path plus contents.
  for (const std::string& path : sources) {
    hasher.UpdateDelimited(roots.Canonicalize(path));
    if (!hasher.UpdateFile(path)) {
      VCACHE_LOG("rust: could not read source " + path);
      return "";
    }
  }

  // Dependencies by content rather than by path, so a differently located
  // target directory still hits.
  std::vector<args::ExternCrate> externs = parsed.externs;
  std::sort(externs.begin(), externs.end(),
            [](const args::ExternCrate& a, const args::ExternCrate& b) {
              return a.name < b.name;
            });
  for (const args::ExternCrate& ext : externs) {
    hasher.UpdateDelimited(ext.name);
    if (ext.path.empty()) continue;
    if (!hasher.UpdateFile(ext.path)) {
      VCACHE_LOG("rust: could not read extern " + ext.path);
      return "";
    }
  }

  for (const std::string& name : config.extra_env_vars) {
    const char* value = std::getenv(name.c_str());
    hasher.UpdateDelimited(name);
    hasher.UpdateDelimited(value != nullptr ? value : "");
  }
  return hasher.Hex();
}

// Stands in for the output directory inside stored dep-info. rustc records the
// artifact paths it wrote, which during a miss are inside vcache's staging
// directory; substituting a placeholder keeps the entry directory-independent.
constexpr std::string_view kOutDirPlaceholder = "/vcache-outdir";

// Rewrites `from` to `to` at the start of every target and prerequisite.
void SubstituteDir(core::DepFile* dep, const std::string& from,
                   const std::string& to) {
  auto fix = [&](std::string& path) {
    if (path.size() >= from.size() && path.compare(0, from.size(), from) == 0) {
      path = to + path.substr(from.size());
    }
  };
  for (core::DepRule& rule : dep->rules) {
    for (std::string& t : rule.targets) fix(t);
    for (std::string& p : rule.prerequisites) fix(p);
  }
}

// Collects every file produced under `dir` as a blob file set, canonicalising
// any dependency-info file on the way.
bool CaptureOutputs(const std::string& dir, const RootMap& roots,
                    std::vector<storage::BlobFile>* files) {
  std::error_code ec;
  for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
    if (ec) return false;
    if (!entry.is_regular_file()) continue;
    const std::string path = entry.path().string();
    auto contents = util::ReadFile(path);
    if (!contents) return false;

    storage::BlobFile file;
    file.name = fs::relative(entry.path(), fs::path(dir), ec).string();
    if (ec) return false;

    // cargo runs build scripts and binary crates directly out of the output
    // directory, so an entry that restores the bytes but not the execute bit
    // fails the build with EACCES.
    file.executable =
        (entry.status().permissions() & fs::perms::owner_exec) != fs::perms::none;

    // rustc does not apply --remap-path-prefix to dep-info output, exactly as
    // gcc does not to .d files, so it has to be rewritten explicitly.
    if (util::EndsWith(file.name, ".d")) {
      if (auto dep = core::ParseDepFile(*contents)) {
        core::RemapDepFile(&*dep, roots, MapDirection::kCanonicalize);
        SubstituteDir(&*dep, dir, std::string(kOutDirPlaceholder));
        file.contents = core::RenderDepFile(*dep);
      } else {
        file.contents = std::move(*contents);
      }
    } else {
      file.contents = std::move(*contents);
    }
    files->push_back(std::move(file));
  }
  return !files->empty();
}

// Writes a captured file set into the real output directory.
bool RestoreOutputs(const std::vector<storage::BlobFile>& files,
                    const std::string& out_dir, const RootMap& roots) {
  for (const storage::BlobFile& file : files) {
    const std::string target = out_dir + "/" + file.name;
    std::string contents = file.contents;
    if (util::EndsWith(file.name, ".d")) {
      if (auto dep = core::ParseDepFile(contents)) {
        core::RemapDepFile(&*dep, roots, MapDirection::kLocalize);
        SubstituteDir(&*dep, std::string(kOutDirPlaceholder), out_dir);
        contents = core::RenderDepFile(*dep);
      }
    }
    if (!util::WriteFileAtomic(target, contents)) {
      VCACHE_LOG("rust: could not write " + target);
      return false;
    }
    if (file.executable) {
      std::error_code perm_ec;
      fs::permissions(target,
                      fs::perms::owner_exec | fs::perms::group_exec |
                          fs::perms::others_exec,
                      fs::perm_options::add, perm_ec);
      if (perm_ec) {
        VCACHE_LOG("rust: could not make " + target + " executable");
        return false;
      }
    }
  }
  return true;
}

}  // namespace

int RunRustCompile(const std::vector<std::string>& argv,
                   const core::Config& config, const RootMap& roots,
                   storage::CacheChain* cache) {
  const std::string& cache_dir = config.disk.dir;

  args::RustcArgs parsed = args::ParseRustc(argv);
  if (!parsed.cacheable()) {
    VCACHE_LOG("rust uncacheable: " + *parsed.uncacheable);
    core::RecordCounter(cache_dir, Counter::kUncacheable);
    return RunPassthrough(argv);
  }

  if (!parsed.incoming_prefix_maps.empty()) {
    switch (config.incoming_map_policy) {
      case core::IncomingMapPolicy::kError:
        ::fprintf(stderr,
                  "vcache: refusing to run: the command line contains %s\n"
                  "vcache manages path prefix mapping itself. Pass "
                  "--vcache-allow-prefix-maps to drop them,\nor set "
                  "VCACHE_INCOMING_PREFIX_MAPS=keep to pass them through "
                  "(disables caching).\n",
                  parsed.incoming_prefix_maps.front().c_str());
        return 1;
      case core::IncomingMapPolicy::kKeep:
        core::RecordCounter(cache_dir, Counter::kUncacheable);
        return RunPassthrough(argv);
      case core::IncomingMapPolicy::kStrip:
        VCACHE_LOG("rust: stripped incoming remap flags");
        break;
    }
  }

  auto temp_dir = util::MakeTempDir("vcache-rs-");
  if (!temp_dir) return RunPassthrough(argv);
  struct TempDirGuard {
    std::string path;
    ~TempDirGuard() { util::RemoveRecursive(path); }
  } guard{*temp_dir};

  const std::string rustc_fingerprint =
      ResolveRustcFingerprint(parsed.compiler, cache_dir);

  const std::vector<std::string> sources =
      CollectSourceFiles(parsed, roots, *temp_dir);
  if (sources.empty()) {
    VCACHE_LOG("rust: no dependency information; falling back");
    core::RecordCounter(cache_dir, Counter::kPreprocessFailed);
    return RunPassthrough(argv);
  }

  const std::string key =
      ComputeKey(parsed, roots, rustc_fingerprint, config, sources);
  if (key.empty()) return RunPassthrough(argv);
  VCACHE_LOG("rust key " + key + " for " + parsed.source);

  if (!util::MakeDirs(parsed.out_dir)) {
    VCACHE_LOG("rust: cannot create out-dir " + parsed.out_dir);
    return RunPassthrough(argv);
  }

  // Tracks whether any layer was broken, as opposed to cold, for this run.
  bool media_failed = false;

  if (!config.recache && cache != nullptr) {
    storage::GetResult got = cache->Get(key);
    media_failed |= core::ReportCacheMediaErrors(got.errors, cache_dir);
    if (got.hit) {
      storage::Blob blob;
      if (storage::DeserializeBlob(got.value, &blob) && !blob.files.empty() &&
          RestoreOutputs(blob.files, parsed.out_dir, roots)) {
        if (!blob.stderr_text.empty()) {
          const std::string text = roots.LocalizeText(blob.stderr_text);
          ::fwrite(text.data(), 1, text.size(), stderr);
        }
        VCACHE_LOG("rust hit on " + got.layer);
        core::RecordCounter(cache_dir, got.layer == "s3" ? Counter::kHitS3
                                                         : Counter::kHitDisk);
        return 0;
      }
      VCACHE_LOG("rust: unusable cache entry; recompiling");
    }
  }
  core::RecordCounter(cache_dir, Counter::kMiss);

  // ---- miss: compile into a staging directory -----------------------------

  const std::string stage_dir = *temp_dir + "/out";
  if (!util::MakeDirs(stage_dir)) return RunPassthrough(argv);

  std::vector<std::string> cmd;
  cmd.push_back(parsed.compiler);
  for (const std::string& arg : parsed.base_args) cmd.push_back(arg);
  for (const std::string& arg : roots.PrefixMapArgs(core::PrefixMapStyle::kRust)) {
    cmd.push_back(arg);
  }
  cmd.push_back("--emit=" + util::Join(parsed.emit_kinds, ","));
  cmd.push_back("--out-dir");
  cmd.push_back(stage_dir);
  cmd.push_back(parsed.source);

  VCACHE_LOG("rust compile: " + util::Join(cmd, " "));
  util::ProcResult compiled = util::Run(cmd, {.capture_stderr = true});

  if (compiled.exit_code != 0) {
    if (!compiled.stderr_data.empty()) {
      ::fwrite(compiled.stderr_data.data(), 1, compiled.stderr_data.size(), stderr);
    }
    core::RecordCounter(cache_dir, Counter::kCompileFailed);
    return compiled.exit_code;
  }

  std::vector<storage::BlobFile> files;
  if (!CaptureOutputs(stage_dir, roots, &files)) {
    VCACHE_LOG("rust: could not capture outputs; rerunning directly");
    return RunPassthrough(argv);
  }
  if (!RestoreOutputs(files, parsed.out_dir, roots)) return RunPassthrough(argv);

  // Only now, with the artifacts in place. rustc announces each one on stderr
  // as a JSON "artifact" message, and cargo uses those to start a dependent
  // crate the moment its .rmeta appears -- so forwarding them any earlier
  // points cargo at a file vcache has not written yet, and the dependent fails
  // with "extern location for <crate> does not exist".
  if (!compiled.stderr_data.empty()) {
    ::fwrite(compiled.stderr_data.data(), 1, compiled.stderr_data.size(), stderr);
    ::fflush(stderr);
  }

  if (config.read_only || cache == nullptr) {
    if (media_failed && config.error_on_cache_media_failure &&
        compiled.exit_code == 0) {
      return core::kCacheMediaFailureExit;
    }
    return compiled.exit_code;
  }

  storage::Blob blob;
  blob.files = std::move(files);
  blob.stderr_text = roots.CanonicalizeText(compiled.stderr_data);
  blob.meta = "rustc: " + rustc_fingerprint + "\ncrate: " + parsed.crate_name +
              "\nroots:\n" + roots.DebugString();

  const storage::PutResult put = cache->Put(key, storage::SerializeBlob(blob));
  media_failed |= core::ReportCacheMediaErrors(put.errors, cache_dir);
  if (put.stored) {
    core::RecordCounter(cache_dir, Counter::kStored);
  } else {
    core::RecordCounter(cache_dir, Counter::kStoreFailed);
  }
  if (media_failed && config.error_on_cache_media_failure &&
      compiled.exit_code == 0) {
    return core::kCacheMediaFailureExit;
  }
  return compiled.exit_code;
}

}  // namespace vcache::rust
