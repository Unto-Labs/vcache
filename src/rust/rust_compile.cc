// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "rust/rust_compile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>

#include "args/rustc_args.h"
#include "core/compile.h"
#include "core/cost.h"
#include "core/depfile.h"
#include "core/stats.h"
#include "hash/hasher.h"
#include "rust/rust_manifest.h"
#include "storage/storage.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/str.h"
#include "util/subprocess.h"

namespace fs = std::filesystem;

namespace vcache::rust {

// A relink after a source change often keeps the same byte count. The memo
// key therefore includes mtime, or the new binary is answered from the
// previous banner and every later crate hits the old toolchain. mtime stays
// out of the banner hash: that hash is what other machines share.
std::string ResolveRustcFingerprint(const std::string& rustc,
                                    const std::string& cache_dir) {
  const std::string real = util::RealPath(rustc).value_or(rustc);
  const uint64_t size = util::FileSize(real).value_or(0);
  const int64_t mtime = util::FileMtime(real).value_or(0);

  hash::Hasher memo_key;
  memo_key.UpdateDelimited("rustc-version-memo-v2");
  memo_key.UpdateDelimited(real);
  memo_key.UpdateU64(size);
  memo_key.UpdateU64(static_cast<uint64_t>(mtime));
  const std::string memo_path = cache_dir + "/compilers/" + memo_key.Hex();

  if (auto cached = util::ReadFile(memo_path)) return hash::HashString(*cached);

  util::ProcResult probe =
      util::Run({rustc, "-vV"}, {.capture_stdout = true, .capture_stderr = true});
  const std::string banner = probe.stdout_data + probe.stderr_data;
  if (probe.exit_code != 0 || banner.empty()) return hash::HashString(real);

  util::WriteFileAtomic(memo_path, banner);
  return hash::HashString(banner);
}

namespace {

using core::Counter;
using core::MapDirection;
using core::Reason;
using core::RootMap;

constexpr std::string_view kCacheKeyVersion = "vcache-rust-key-v3";
constexpr std::string_view kManifestKeyVersion = "vcache-rust-manifest-v1";

int RunPassthrough(const std::vector<std::string>& argv) {
  VCACHE_LOG("rust passthrough: " + util::Join(argv, " "));
  util::ProcResult result = util::Run(argv);
  if (result.exit_code < 0) {
    ::fprintf(stderr, "vcache: failed to execute %s\n", argv[0].c_str());
    return 127;
  }
  return result.exit_code;
}

// What rustc's dep-info says the crate reads.
struct CrateInputs {
  std::vector<std::string> sources;
  std::vector<core::DepEnv> env_deps;
};

// Asks rustc which files and environment variables this crate reads. Sources
// are empty on failure.
CrateInputs CollectCrateInputs(const args::RustcArgs& parsed,
                               const std::string& temp_dir) {
  const std::string dep_dir = temp_dir + "/depinfo";
  if (!util::MakeDirs(dep_dir)) return {};

  std::vector<std::string> cmd;
  cmd.push_back(parsed.compiler);
  for (const std::string& arg : parsed.dep_info_args) cmd.push_back(arg);
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

  CrateInputs inputs;
  std::vector<std::string>& sources = inputs.sources;
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
    for (core::DepEnv& env : dep->env_deps) {
      VCACHE_LOG("rust env-dep " + env.name +
                 (env.value ? "=" + *env.value : std::string(" (unset)")));
      inputs.env_deps.push_back(std::move(env));
    }
  }

  // rustc repeats each source across several rules; one hash per file is
  // enough, and a stable order keeps the key deterministic.
  std::sort(sources.begin(), sources.end());
  sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
  return inputs;
}

// Digests every --extern, sorted by name. nullopt if a dependency cannot be
// read, since then nothing pins what would be linked.
std::optional<std::vector<RustExtern>> HashExterns(const args::RustcArgs& parsed) {
  std::vector<RustExtern> externs;
  for (const args::ExternCrate& ext : parsed.externs) {
    RustExtern hashed{ext.name, ""};
    if (!ext.path.empty()) {
      auto digest = hash::HashFile(ext.path);
      if (!digest) {
        VCACHE_LOG("rust: could not read extern " + ext.path);
        return std::nullopt;
      }
      hashed.digest = std::move(*digest);
    }
    externs.push_back(std::move(hashed));
  }
  std::sort(externs.begin(), externs.end(), [](const RustExtern& a, const RustExtern& b) {
    return a.name != b.name ? a.name < b.name : a.digest < b.digest;
  });
  return externs;
}

// Canonical path and digest of every source. Hashed once, so the key and the
// manifest state recorded beside it cannot see two different contents.
std::optional<std::vector<core::ManifestFile>> HashSources(
    const std::vector<std::string>& sources, const RootMap& roots) {
  std::vector<core::ManifestFile> files;
  for (const std::string& path : sources) {
    auto digest = hash::HashFile(path);
    if (!digest) {
      VCACHE_LOG("rust: could not read source " + path);
      return std::nullopt;
    }
    files.emplace_back(roots.Canonicalize(path), std::move(*digest));
  }
  return files;
}

// The part of both keys that comes from the invocation itself.
void HashInvocation(const args::RustcArgs& parsed, const RootMap& roots,
                    const std::string& rustc_fingerprint, hash::Hasher* hasher) {
  hasher->UpdateDelimited(rustc_fingerprint);
  hasher->UpdateDelimited(roots.Fingerprint());
  for (const std::string& arg : parsed.key_args) {
    hasher->UpdateDelimited(roots.Canonicalize(arg));
  }
  // --emit decides which artifacts land in the output directory, so it is part
  // of the key even though vcache passes it separately.
  std::vector<std::string> emit = parsed.emit_kinds;
  std::sort(emit.begin(), emit.end());
  for (const std::string& kind : emit) hasher->UpdateDelimited(kind);
}

void HashExtraEnv(const core::Config& config, hash::Hasher* hasher) {
  for (const std::string& name : config.extra_env_vars) {
    const char* value = std::getenv(name.c_str());
    hasher->UpdateDelimited(name);
    hasher->UpdateDelimited(value != nullptr ? value : "");
  }
}

std::string ComputeKey(const args::RustcArgs& parsed, const RootMap& roots,
                       const std::string& rustc_fingerprint,
                       const core::Config& config, const RustManifestState& inputs) {
  hash::Hasher hasher;
  hasher.UpdateDelimited(kCacheKeyVersion);
  HashInvocation(parsed, roots, rustc_fingerprint, &hasher);

  for (const auto& [canonical_path, digest] : inputs.files) {
    hasher.UpdateDelimited(canonical_path);
    hasher.UpdateDelimited(digest);
  }

  // Raw unless named in rust_path_env_vars: rustc does not remap env values,
  // so a path-valued variable such as OUT_DIR may be baked into the artifact.
  // A canonicalised one is checked for that before its entry is stored.
  for (const core::DepEnv& env : inputs.env_deps) {
    hasher.UpdateDelimited(env.name);
    hasher.UpdateDelimited(env.value ? "=" + *env.value : std::string("unset"));
  }

  // Dependencies by content rather than by path, so a differently located
  // target directory still hits.
  for (const RustExtern& ext : inputs.externs) {
    hasher.UpdateDelimited(ext.name);
    if (!ext.digest.empty()) hasher.UpdateDelimited(ext.digest);
  }

  HashExtraEnv(config, &hasher);
  return hasher.Hex();
}

// Everything known before rustc runs. The crate root's canonical path is in it
// because the recorded module paths are relative to that root: two crates with
// identical roots in different places must not share states.
std::string ComputeManifestKey(const args::RustcArgs& parsed, const RootMap& roots,
                               const std::string& rustc_fingerprint,
                               const core::Config& config,
                               const std::string& source_digest,
                               const std::vector<RustExtern>& externs) {
  hash::Hasher hasher;
  hasher.UpdateDelimited(kManifestKeyVersion);
  HashInvocation(parsed, roots, rustc_fingerprint, &hasher);
  hasher.UpdateDelimited(roots.Canonicalize(parsed.source));
  hasher.UpdateDelimited(source_digest);
  for (const RustExtern& ext : externs) hasher.UpdateDelimited(ext.name);
  HashExtraEnv(config, &hasher);
  return hasher.Hex();
}

std::vector<RustManifestState> LoadManifest(storage::CacheChain* cache,
                                            const std::string& manifest_key,
                                            const std::string& cache_dir,
                                            bool* media_failed) {
  std::vector<RustManifestState> states;
  storage::GetResult got = cache->Get(manifest_key);
  *media_failed |= core::ReportCacheMediaErrors(got.errors, cache_dir);
  storage::Blob blob;
  if (!got.hit || !storage::DeserializeBlob(got.value, &blob) || !blob.has_dep_manifest) {
    VCACHE_LOG("rust manifest: none stored");
    return states;
  }
  if (!ParseRustManifest(blob.dep_manifest, &states)) {
    VCACHE_LOG("rust manifest: did not parse; starting a new one");
    states.clear();
  }
  return states;
}

void StoreManifest(storage::CacheChain* cache, const std::string& manifest_key,
                   RustManifestState fresh, const std::vector<RustManifestState>& loaded,
                   const std::string& cache_dir, bool* media_failed) {
  core::PauseBeforeManifestPut();
  const std::vector<RustManifestState> reread =
      LoadManifest(cache, manifest_key, cache_dir, media_failed);
  const std::vector<RustManifestState> states = core::MergeManifestStates(
      std::move(fresh), reread, loaded, &RustManifestState::key);
  storage::Blob blob;
  blob.dep_manifest = RenderRustManifest(states);
  blob.has_dep_manifest = true;
  blob.meta = "rust dep-info manifest\nstates: " + std::to_string(states.size()) + "\n";
  const storage::PutResult put = cache->Put(manifest_key, storage::SerializeBlob(blob));
  *media_failed |= core::ReportCacheMediaErrors(put.errors, cache_dir);
  VCACHE_LOG(put.stored ? "rust manifest: stored " + std::to_string(states.size()) + " states"
                        : std::string("rust manifest: could not store"));
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

}  // namespace

// A blob name is joined onto the output directory. `..` or an absolute name
// would write outside it, including a name captured from a symlink whose
// target is not in the directory.
bool IsSafeOutputName(std::string_view name) {
  if (name.empty() || name.find('\0') != std::string_view::npos) return false;
  if (name.front() == '/') return false;
  std::size_t begin = 0;
  while (begin < name.size()) {
    const std::size_t slash = name.find('/', begin);
    const std::size_t end = slash == std::string_view::npos ? name.size() : slash;
    const std::string_view component = name.substr(begin, end - begin);
    if (component == "..") return false;
    if (slash == std::string_view::npos) return true;
    begin = slash + 1;
  }
  return true;
}

// Collects every file produced under `dir` as a blob file set, canonicalising
// any dependency-info file on the way.
bool CaptureOutputs(const std::string& dir, const RootMap& roots,
                    const std::vector<std::string>& path_env_vars,
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
    if (!IsSafeOutputName(file.name)) return false;

    // cargo runs build scripts and binary crates directly out of the output
    // directory, so an entry that restores the bytes but not the execute bit
    // fails the build with EACCES.
    file.executable =
        (entry.status().permissions() & fs::perms::owner_exec) != fs::perms::none;

    // rustc does not apply --remap-path-prefix to dep-info output, exactly as
    // gcc does not to .d files, so it has to be rewritten explicitly.
    if (util::EndsWith(file.name, ".d")) {
      if (auto dep = core::ParseDepFile(*contents)) {
        core::RemapDepFile(&*dep, roots, MapDirection::kCanonicalize, path_env_vars);
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
                    const std::string& out_dir, const RootMap& roots,
                    const std::vector<std::string>& path_env_vars) {
  for (const storage::BlobFile& file : files) {
    if (!IsSafeOutputName(file.name)) {
      VCACHE_LOG("rust: refusing stored file name '" + file.name + "'");
      return false;
    }
  }
  for (const storage::BlobFile& file : files) {
    const std::string target = out_dir + "/" + file.name;
    // A dep-info file is rewritten into this checkout. Every other file is
    // written from the bytes already in the blob.
    const std::string* bytes = &file.contents;
    std::string rewritten;
    if (util::EndsWith(file.name, ".d")) {
      if (auto dep = core::ParseDepFile(file.contents)) {
        core::RemapDepFile(&*dep, roots, MapDirection::kLocalize, path_env_vars);
        SubstituteDir(&*dep, std::string(kOutDirPlaceholder), out_dir);
        rewritten = core::RenderDepFile(*dep);
        bytes = &rewritten;
      }
    }
    if (!util::WriteFileAtomic(target, *bytes)) {
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

namespace {

// Names the first path-valued env dep whose local value, which the key left out,
// still appears in the entry: another checkout must not be served this path.
std::optional<std::string> FindEnvPathInOutput(const std::vector<core::DepEnv>& env_deps,
                                               const std::vector<std::string>& path_env_vars,
                                               const RootMap& roots, const storage::Blob& blob) {
  for (const core::DepEnv& env : env_deps) {
    if (!env.value || std::find(path_env_vars.begin(), path_env_vars.end(), env.name) ==
                          path_env_vars.end()) {
      continue;
    }
    const char* raw = std::getenv(env.name.c_str());
    if (raw == nullptr || *raw == '\0' || roots.Canonicalize(raw) == raw) continue;
    const std::string_view local(raw);
    if (blob.stderr_text.find(local) != std::string::npos) return env.name;
    for (const storage::BlobFile& file : blob.files) {
      if (file.contents.find(local) != std::string::npos) return env.name + " in " + file.name;
    }
  }
  return std::nullopt;
}

// Restores a cached entry and replays its diagnostics. False if the entry is
// unusable, in which case the caller recompiles.
bool ServeHit(const storage::GetResult& got, const args::RustcArgs& parsed,
              const RootMap& roots, const std::vector<std::string>& path_env_vars) {
  storage::Blob blob;
  if (!storage::DeserializeBlob(got.value, &blob) || blob.files.empty() ||
      !RestoreOutputs(blob.files, parsed.out_dir, roots, path_env_vars)) {
    return false;
  }
  if (!blob.stderr_text.empty()) {
    const std::string text = roots.LocalizeText(blob.stderr_text);
    ::fwrite(text.data(), 1, text.size(), stderr);
  }
  return true;
}

Counter HitCounter(const storage::GetResult& got) {
  return got.layer == "s3" ? Counter::kHitS3 : Counter::kHitDisk;
}

}  // namespace

int RunRustCompile(const std::vector<std::string>& argv,
                   const core::Config& config, const RootMap& roots,
                   storage::CacheChain* cache) {
  const std::string& cache_dir = config.disk.dir;

  args::RustcArgs parsed = args::ParseRustc(argv);
  if (!parsed.cacheable()) {
    core::RecordDecision(cache_dir, *parsed.uncacheable);
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
        core::RecordDecision(cache_dir, Reason::kKeptPrefixMaps);
        return RunPassthrough(argv);
      case core::IncomingMapPolicy::kStrip:
        VCACHE_LOG("rust: stripped incoming remap flags");
        break;
    }
  }

  auto temp_dir = util::MakeTempDir("vcache-rs-");
  if (!temp_dir) {
    core::RecordDecision(cache_dir, Reason::kNoTempDir);
    return RunPassthrough(argv);
  }
  struct TempDirGuard {
    std::string path;
    ~TempDirGuard() { util::RemoveRecursive(path); }
  } guard{*temp_dir};

  const std::string rustc_fingerprint =
      ResolveRustcFingerprint(parsed.compiler, cache_dir);

  const std::optional<std::vector<RustExtern>> externs = HashExterns(parsed);
  if (!externs) {
    core::RecordDecision(cache_dir, Reason::kNoCacheKey);
    return RunPassthrough(argv);
  }

  if (!util::MakeDirs(parsed.out_dir)) {
    core::RecordDecision(cache_dir, {Reason::kOutDirUnwritable, parsed.out_dir});
    return RunPassthrough(argv);
  }

  // Tracks whether any layer was broken, as opposed to cold, for this run.
  bool media_failed = false;

  // ---- manifest: answer from a remembered dep-info run --------------------

  std::string manifest_key;
  std::vector<RustManifestState> states;
  if (config.rust_dep_info_policy == core::RustDepInfoPolicy::kManifest &&
      cache != nullptr) {
    if (auto source_digest = hash::HashFile(parsed.source)) {
      manifest_key = ComputeManifestKey(parsed, roots, rustc_fingerprint, config,
                                        *source_digest, *externs);
      VCACHE_LOG("rust manifest key " + manifest_key + " for " + parsed.source);
      states = LoadManifest(cache, manifest_key, cache_dir, &media_failed);
    }
  }

  if (!config.recache && !manifest_key.empty()) {
    for (size_t i = 0; i < states.size(); ++i) {
      const std::string which =
          "state " + std::to_string(i + 1) + " of " + std::to_string(states.size());
      if (auto mismatch = FindRustStateMismatch(states[i], *externs, roots,
                                                config.rust_path_env_vars)) {
        VCACHE_LOG("rust manifest: " + which + " rejected: " + *mismatch);
        continue;
      }
      storage::GetResult got = cache->Get(states[i].key);
      media_failed |= core::ReportCacheMediaErrors(got.errors, cache_dir);
      if (!got.hit || !ServeHit(got, parsed, roots, config.rust_path_env_vars)) {
        VCACHE_LOG("rust manifest: " + which + " matched but its entry is " +
                   (got.hit ? "unusable" : "gone"));
        continue;
      }
      VCACHE_LOG("rust key " + states[i].key + " for " + parsed.source);
      VCACHE_LOG("rust manifest hit: " + which + ", on " + got.layer);
      core::RecordCounter(cache_dir, HitCounter(got));
      if (i > 0 && !config.read_only) {
        StoreManifest(cache, manifest_key, states[i], states, cache_dir, &media_failed);
      }
      return 0;
    }
  }

  // ---- dep-info: ask rustc what the crate reads ---------------------------

  const CrateInputs inputs = CollectCrateInputs(parsed, *temp_dir);
  if (inputs.sources.empty()) {
    core::RecordDecision(cache_dir, Reason::kNoRustDepInfo);
    return RunPassthrough(argv);
  }

  auto source_files = HashSources(inputs.sources, roots);
  if (!source_files) {
    core::RecordDecision(cache_dir, Reason::kNoCacheKey);
    return RunPassthrough(argv);
  }

  RustManifestState fresh;
  fresh.files = std::move(*source_files);
  for (const core::DepEnv& env : inputs.env_deps) {
    fresh.env_deps.push_back(
        {env.name, KeyedEnvDepValue(env.name, env.value, config.rust_path_env_vars, roots)});
  }
  fresh.externs = *externs;
  fresh.key = ComputeKey(parsed, roots, rustc_fingerprint, config, fresh);
  const std::string& key = fresh.key;
  VCACHE_LOG("rust key " + key + " for " + parsed.source);

  // Records what this dep-info run found, once its entry is known to exist.
  const auto record_state = [&]() {
    if (manifest_key.empty() || config.read_only) return;
    StoreManifest(cache, manifest_key, fresh, states, cache_dir, &media_failed);
  };

  if (!config.recache && cache != nullptr) {
    storage::GetResult got = cache->Get(key);
    media_failed |= core::ReportCacheMediaErrors(got.errors, cache_dir);
    if (got.hit) {
      if (ServeHit(got, parsed, roots, config.rust_path_env_vars)) {
        VCACHE_LOG("rust hit on " + got.layer);
        core::RecordCounter(cache_dir, HitCounter(got));
        record_state();
        return 0;
      }
      VCACHE_LOG("rust: unusable cache entry; recompiling");
    }
  }
  core::RecordCounter(cache_dir, Counter::kMiss);

  // ---- miss: compile into a staging directory -----------------------------

  const std::string stage_dir = *temp_dir + "/out";
  if (!util::MakeDirs(stage_dir)) {
    core::RecordDecision(cache_dir, Reason::kNoTempDir);
    return RunPassthrough(argv);
  }

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
  core::RecordCompileCost(cache_dir, "rustc", parsed.source, "rust", parsed.key_args, roots,
                          compiled);

  if (compiled.exit_code != 0) {
    if (!compiled.stderr_data.empty()) {
      ::fwrite(compiled.stderr_data.data(), 1, compiled.stderr_data.size(), stderr);
    }
    core::RecordCounter(cache_dir, Counter::kCompileFailed);
    return compiled.exit_code;
  }

  std::vector<storage::BlobFile> files;
  if (!CaptureOutputs(stage_dir, roots, config.rust_path_env_vars, &files)) {
    core::RecordDecision(cache_dir, Reason::kCaptureFailed);
    return RunPassthrough(argv);
  }
  if (!RestoreOutputs(files, parsed.out_dir, roots, config.rust_path_env_vars)) {
    core::RecordDecision(cache_dir, {Reason::kOutputUnplaceable, parsed.out_dir});
    return RunPassthrough(argv);
  }

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

  if (auto leaked = FindEnvPathInOutput(inputs.env_deps, config.rust_path_env_vars, roots,
                                        blob)) {
    core::RecordDecision(cache_dir, {Reason::kEnvPathInOutput, *leaked});
    return compiled.exit_code;
  }
  blob.meta = "rustc: " + rustc_fingerprint + "\ncrate: " + parsed.crate_name +
              "\nroots:\n" + roots.DebugString();
  core::AppendCostMeta(&blob.meta, compiled.max_rss_kb, compiled.wall_ms);

  const storage::PutResult put = cache->Put(key, storage::SerializeBlob(blob));
  media_failed |= core::ReportCacheMediaErrors(put.errors, cache_dir);
  if (put.stored) {
    core::RecordCounter(cache_dir, Counter::kStored);
    record_state();
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
