// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "core/compile.h"

#include <sys/utsname.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "args/compiler_args.h"
#include "core/depfile.h"
#include "core/preprocessed.h"
#include "core/stats.h"
#include "hash/hasher.h"
#include "storage/storage.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/str.h"
#include "util/subprocess.h"

namespace vcache::core {
namespace {

// Bumped whenever the key derivation or blob layout changes, so old entries are
// ignored rather than misinterpreted.
constexpr std::string_view kCacheKeyVersion = "vcache-key-v2";

// Runs the compiler unchanged and returns its exit code. The fallback path for
// everything vcache cannot or should not cache.
int RunPassthrough(const std::vector<std::string>& argv) {
  VCACHE_LOG("passthrough: " + util::Join(argv, " "));
  util::ProcResult result = util::Run(argv);
  if (result.exit_code < 0) {
    ::fprintf(stderr, "vcache: failed to execute %s\n", argv[0].c_str());
    return 127;
  }
  return result.exit_code;
}

std::string EnvOr(const char* name, const std::string& fallback) {
  const char* v = std::getenv(name);
  return (v != nullptr && *v != '\0') ? std::string(v) : fallback;
}

// Assembles the preprocessing command: the original flags, plus the root
// prefix maps, minus anything that would write files.
//
// -fno-working-directory is essential here. gcc emits a `# 1 "<cwd>//"`
// linemarker whenever debug info is enabled, and -ffile-prefix-map does *not*
// rewrite it, so without this flag the preprocessed text differs between build
// directories and every cross-directory lookup misses.
std::vector<std::string> BuildPreprocessCommand(const args::CompilerArgs& parsed,
                                                const RootMap& roots) {
  std::vector<std::string> cmd;
  cmd.push_back(parsed.compiler);
  // base_args deliberately excludes dependency flags: gcc rejects -MP or -MF
  // when neither -M nor -MM is present, so passing them through here would make
  // every preprocessing run fail.
  for (const std::string& arg : parsed.base_args) cmd.push_back(arg);
  for (const std::string& arg : roots.PrefixMapArgs(PrefixMapStyle::kC)) {
    cmd.push_back(arg);
  }
  cmd.push_back("-fno-working-directory");
  cmd.push_back("-E");
  cmd.push_back(parsed.source);
  return cmd;
}

// Assembles the real compile command with prefix maps injected.
std::vector<std::string> BuildCompileCommand(const args::CompilerArgs& parsed,
                                             const RootMap& roots,
                                             const std::string& output,
                                             const std::string& depfile) {
  std::vector<std::string> cmd;
  cmd.push_back(parsed.compiler);
  for (const std::string& arg : parsed.base_args) cmd.push_back(arg);
  for (const std::string& arg : roots.PrefixMapArgs(PrefixMapStyle::kC)) {
    cmd.push_back(arg);
  }
  // Dependency flags only belong on the real compile.
  if (!depfile.empty()) {
    for (const std::string& arg : parsed.dep_args) cmd.push_back(arg);
  }
  if (parsed.compile_only) cmd.push_back("-c");
  if (parsed.assemble_only) cmd.push_back("-S");
  if (!depfile.empty()) {
    cmd.push_back("-MF");
    cmd.push_back(depfile);
    // The real compile writes the object to a temporary path. Without an
    // explicit -MT gcc would name that temporary as the dependency-rule target,
    // and make would never match the rule.
    if (!parsed.dep_target_explicit) {
      cmd.push_back("-MT");
      cmd.push_back(parsed.output);
    }
  }
  cmd.push_back("-o");
  cmd.push_back(output);
  cmd.push_back(parsed.source);
  return cmd;
}

// The driver's -x value to probe a native target with. The predefined macro set
// differs slightly between C and C++, so probe in the language being compiled.
const char* NativeProbeLanguage(args::Language lang) {
  switch (lang) {
    case args::Language::kCxx: return "c++";
    case args::Language::kObjC: return "objective-c";
    case args::Language::kObjCxx: return "objective-c++";
    default: return "c";
  }
}

// Identity of the machine the probe result belongs to. The memo below lives in
// the disk cache directory, which is normally local; this keeps the answer
// correct anyway if that directory is shared between hosts, since a different
// host derives a different memo path rather than reading a stale one.
std::string HostIdentity() {
  std::string id;
  char name[256] = {0};
  if (::gethostname(name, sizeof(name) - 1) == 0) id.assign(name);
  struct ::utsname uts;
  if (::uname(&uts) == 0) {
    id += "|";
    id += uts.machine;
  }
  return id;
}

// Turns -march=native (and friends) into something the key can actually use.
//
// The flag text is identical on every machine while the code it produces is
// not, so hashing the text alone would let two hosts with different CPUs share
// an entry. Asking the compiler for its predefined macros with the same flags
// applied gives a concrete answer: the dump names every enabled ISA extension
// and the selected tuning model (__AVX512F__, __tune_znver4__, and so on), so
// hosts that would generate different code derive different keys.
//
// Returns the empty string if the probe fails, which the caller treats as
// uncacheable -- guessing here would be worse than not caching.
//
// The probe costs a process (~7 ms), which is significant against a cache hit,
// so the answer is memoised in the cache directory.
std::string ResolveNativeTarget(const args::CompilerArgs& parsed,
                                const CompilerId& compiler_id,
                                const std::string& cache_dir) {
  const char* probe_lang = NativeProbeLanguage(parsed.language);

  hash::Hasher memo_key;
  memo_key.UpdateDelimited("native-target-memo-v1");
  memo_key.UpdateDelimited(compiler_id.fingerprint);
  memo_key.UpdateDelimited(HostIdentity());
  memo_key.UpdateDelimited(probe_lang);
  for (const std::string& flag : parsed.native_flags) memo_key.UpdateDelimited(flag);
  const std::string memo_path = cache_dir + "/native/" + memo_key.Hex();

  if (auto cached = util::ReadFile(memo_path)) {
    if (cached->size() == hash::kDigestHexLen) return *cached;
  }

  std::vector<std::string> cmd{parsed.compiler};
  for (const std::string& flag : parsed.native_flags) cmd.push_back(flag);
  cmd.push_back("-E");
  cmd.push_back("-dM");
  cmd.push_back("-x");
  cmd.push_back(probe_lang);
  cmd.push_back("/dev/null");

  util::ProcResult probe =
      util::Run(cmd, {.capture_stdout = true, .capture_stderr = true});
  if (probe.exit_code != 0 || probe.stdout_data.empty()) {
    VCACHE_LOG("native target probe failed: " + util::Join(cmd, " "));
    return "";
  }

  // The dump is tens of kilobytes; only its digest is needed.
  const std::string digest = hash::HashString(probe.stdout_data);
  util::WriteFileAtomic(memo_path, digest);
  return digest;
}

std::string ComputeKey(const args::CompilerArgs& parsed, const RootMap& roots,
                       const CompilerId& compiler_id, const Config& config,
                       const std::string& native_target,
                       const std::string& preprocessed_path) {
  hash::Hasher hasher;
  hasher.UpdateDelimited(kCacheKeyVersion);
  hasher.UpdateDelimited(compiler_id.fingerprint);
  hasher.UpdateDelimited(native_target);

  // Root mapping participates in the key: the same source canonicalised under
  // a different mapping produces a different object.
  hasher.UpdateDelimited(roots.Fingerprint());

  // Only codegen-affecting flags, with any embedded paths canonicalised so that
  // -I-style differences between checkouts do not change the key.
  for (const std::string& arg : parsed.key_args) {
    hasher.UpdateDelimited(roots.Canonicalize(arg));
  }

  for (const std::string& name : config.extra_env_vars) {
    const char* value = std::getenv(name.c_str());
    hasher.UpdateDelimited(name);
    hasher.UpdateDelimited(value != nullptr ? value : "");
  }

  // The preprocessed text carries the full effect of every -I/-D/-include flag
  // and of the source itself. Its linemarkers still hold raw absolute paths --
  // gcc does not apply -ffile-prefix-map to them -- so they are canonicalised
  // on the way into the hash.
  if (!HashNormalizedPreprocessedOutput(preprocessed_path, roots, &hasher)) {
    return "";
  }
  return hasher.Hex();
}

// Writes the cached artifacts into the working tree.
bool MaterializeHit(const storage::Blob& blob, const args::CompilerArgs& parsed,
                    const RootMap& roots) {
  if (!util::WriteFileAtomic(parsed.output, blob.object)) {
    VCACHE_LOG("hit: failed to write object " + parsed.output);
    return false;
  }

  if (parsed.generates_deps && !parsed.depfile.empty() && blob.has_depfile) {
    // The stored .d holds canonical paths; make needs local ones.
    auto dep = ParseDepFile(blob.depfile);
    if (!dep) {
      VCACHE_LOG("hit: cached dependency file did not parse; recompiling");
      return false;
    }
    RemapDepFile(&*dep, roots, MapDirection::kLocalize);

    // The output path is deliberately not part of the cache key, so the stored
    // entry names whichever object the original build produced. Point the rule
    // at this build's object instead. Rules without prerequisites are the
    // phony targets -MP emits and must be left alone.
    if (!parsed.dep_target_explicit) {
      for (DepRule& rule : dep->rules) {
        if (!rule.prerequisites.empty()) rule.targets = {parsed.output};
      }
    }

    if (!util::WriteFileAtomic(parsed.depfile, RenderDepFile(*dep))) {
      VCACHE_LOG("hit: failed to write dependency file " + parsed.depfile);
      return false;
    }
  }

  if (!blob.stderr_text.empty()) {
    const std::string text = roots.LocalizeText(blob.stderr_text);
    ::fwrite(text.data(), 1, text.size(), stderr);
  }
  return true;
}

}  // namespace

CompilerId ResolveCompilerId(const std::string& compiler,
                             const std::string& check_mode,
                             const std::string& cache_dir) {
  CompilerId id;
  const std::string real =
      util::RealPath(compiler).value_or(compiler);

  struct stat_info {
    uint64_t size = 0;
    int64_t mtime = 0;
  } info;
  if (auto size = util::FileSize(real)) info.size = *size;

  if (check_mode == "mtime") {
    hash::Hasher hasher;
    hasher.UpdateDelimited(real);
    hasher.UpdateU64(info.size);
    id.fingerprint = hasher.Hex();
    id.description = real + " (mtime check)";
    return id;
  }

  if (check_mode == "content") {
    hash::Hasher hasher;
    if (hasher.UpdateFile(real)) {
      id.fingerprint = hasher.Hex();
      id.description = real + " (content hash)";
      return id;
    }
  }

  // Default: hash `<compiler> -v`, which reports version, target triple and
  // configure flags. Machine-independent, so entries are shareable via S3.
  //
  // Spawning a process per compilation would be wasteful, so the answer is
  // memoised in the cache directory keyed by the binary's identity.
  hash::Hasher memo_key;
  memo_key.UpdateDelimited("compiler-version-memo-v1");
  memo_key.UpdateDelimited(real);
  memo_key.UpdateU64(info.size);
  const std::string memo_path = cache_dir + "/compilers/" + memo_key.Hex();

  if (auto cached = util::ReadFile(memo_path)) {
    id.fingerprint = hash::HashString(*cached);
    id.description = util::Split(*cached, '\n', /*skip_empty=*/true).empty()
                         ? real
                         : util::Split(*cached, '\n', true).back();
    return id;
  }

  util::ProcResult probe = util::Run({compiler, "-v"},
                                     {.capture_stdout = true, .capture_stderr = true});
  // gcc and clang both print the version banner on stderr.
  std::string banner = probe.stderr_data + probe.stdout_data;
  if (probe.exit_code != 0 || banner.empty()) {
    // Fall back to identity by content rather than giving up on caching.
    hash::Hasher hasher;
    if (hasher.UpdateFile(real)) {
      id.fingerprint = hasher.Hex();
      id.description = real + " (content hash fallback)";
      return id;
    }
    id.fingerprint = hash::HashString(real);
    id.description = real + " (path only)";
    return id;
  }

  util::WriteFileAtomic(memo_path, banner);
  id.fingerprint = hash::HashString(banner);
  const auto lines = util::Split(banner, '\n', /*skip_empty=*/true);
  id.description = lines.empty() ? real : lines.back();
  return id;
}

namespace {

// Bumped independently of the compile key: the two never share entries.
constexpr std::string_view kDepScanKeyVersion = "vcache-depscan-v1";
constexpr std::string_view kManifestHeader = "vcache-depmanifest-2";

// How many header states one manifest remembers. A manifest maps a command line
// to several possible answers, one per set of header contents seen, because the
// key cannot include what it has not read yet. Without that, alternating
// between two branches would miss every time: the entry for the state you just
// left is the one that gets overwritten. Eight covers branch-switching; the
// cost of a stale tail entry is one wasted hash pass.
constexpr size_t kMaxManifestEntries = 8;

// One remembered state: the files a scan read, and where its answer is stored.
struct DepManifestEntry {
  std::string result_key;
  std::vector<std::pair<std::string, std::string>> files;  // canonical path, digest
};

// Everything on the command line that can change which files a dependency scan
// reads or what it writes about them. Unlike a compile there is no preprocessed
// text to stand in for -I/-D, so those go in verbatim -- canonicalised, so two
// checkouts still agree.
//
// Left out: the compiler itself (its identity is hashed separately) and the
// flags naming where the answer goes, since the answer's content does not
// depend on them.
std::vector<std::string> DepScanKeyArgs(const args::CompilerArgs& parsed,
                                        const RootMap& roots) {
  std::vector<std::string> out;
  for (size_t i = 1; i < parsed.argv.size(); ++i) {
    const std::string& arg = parsed.argv[i];
    if (arg == "-o" || arg == "-MF") {
      ++i;  // and its value
      continue;
    }
    if (util::StartsWith(arg, "-MF") && arg.size() > 3) continue;
    if (arg == parsed.source) {
      // The path is irrelevant once canonicalised; the content is hashed below.
      out.push_back("--vcache-source");
      continue;
    }
    out.push_back(roots.Canonicalize(arg));
  }
  return out;
}

// A header line, then per entry an "entry <key>" line followed by one
// "<hex digest> <canonical path>" line per file. The digest comes first so a
// path containing spaces still parses by taking the rest of the line.
std::string RenderManifest(const std::vector<DepManifestEntry>& entries) {
  std::string out(kManifestHeader);
  out.push_back('\n');
  for (const DepManifestEntry& entry : entries) {
    out += "entry ";
    out += entry.result_key;
    out.push_back('\n');
    for (const auto& [path, digest] : entry.files) {
      out += digest;
      out.push_back(' ');
      out += path;
      out.push_back('\n');
    }
  }
  return out;
}

bool ParseManifest(const std::string& text, std::vector<DepManifestEntry>* entries) {
  const auto lines = util::Split(text, '\n', /*skip_empty=*/true);
  if (lines.empty() || lines.front() != kManifestHeader) return false;
  for (size_t i = 1; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    if (util::StartsWith(line, "entry ")) {
      DepManifestEntry entry;
      entry.result_key = line.substr(6);
      if (entry.result_key.size() != hash::kDigestHexLen) return false;
      entries->push_back(std::move(entry));
      continue;
    }
    if (entries->empty()) return false;
    const size_t space = line.find(' ');
    if (space != hash::kDigestHexLen) return false;
    entries->back().files.emplace_back(line.substr(space + 1), line.substr(0, space));
  }
  return true;
}

// True when every recorded file still hashes to what the manifest says. A file
// that has been deleted, changed or replaced makes this false, and the entry is
// then not used -- which is the whole guarantee behind this mode.
bool ManifestStillHolds(const std::vector<std::pair<std::string, std::string>>& files,
                        const RootMap& roots) {
  for (const auto& [canonical_path, digest] : files) {
    const std::string local = roots.Localize(canonical_path);
    auto actual = hash::HashFile(local);
    if (!actual) {
      VCACHE_LOG("dep manifest: " + local + " is gone");
      return false;
    }
    if (*actual != digest) {
      VCACHE_LOG("dep manifest: " + local + " changed");
      return false;
    }
  }
  return true;
}

// Writes the dependency text where the caller asked for it: the named file, or
// stdout when neither -o nor -MF was given.
bool EmitDepOutput(const std::string& path, const std::string& text) {
  if (path.empty()) {
    ::fwrite(text.data(), 1, text.size(), stdout);
    ::fflush(stdout);
    return true;
  }
  return util::WriteFileAtomic(path, text);
}

}  // namespace

int RunDepScan(const std::vector<std::string>& argv, const Config& config,
               const RootMap& roots, storage::CacheChain* cache) {
  const std::string& cache_dir = config.disk.dir;
  const args::CompilerArgs parsed = args::Parse(argv);

  if (config.dep_scan_policy == DepScanPolicy::kUncacheable) {
    VCACHE_LOG("dep scan: policy is 'uncacheable'");
    RecordCounter(cache_dir, Counter::kUncacheable);
    return RunPassthrough(argv);
  }

  const std::string check_mode = EnvOr("VCACHE_COMPILER_CHECK", "version");
  const CompilerId compiler_id =
      ResolveCompilerId(parsed.compiler, check_mode, cache_dir);

  std::string native_target;
  if (!parsed.native_flags.empty()) {
    if (config.native_target_policy == NativeTargetPolicy::kUncacheable) {
      RecordCounter(cache_dir, Counter::kUncacheable);
      return RunPassthrough(argv);
    }
    native_target = ResolveNativeTarget(parsed, compiler_id, cache_dir);
    if (native_target.empty()) {
      RecordCounter(cache_dir, Counter::kUncacheable);
      return RunPassthrough(argv);
    }
  }

  // The source is hashed into the key rather than verified through the
  // manifest, so that editing a file back and forth does not make two versions
  // fight over one entry.
  auto source_digest = hash::HashFile(parsed.source);
  if (!source_digest) {
    VCACHE_LOG("dep scan: cannot read " + parsed.source);
    return RunPassthrough(argv);
  }

  hash::Hasher hasher;
  hasher.UpdateDelimited(kDepScanKeyVersion);
  hasher.UpdateDelimited(compiler_id.fingerprint);
  hasher.UpdateDelimited(native_target);
  hasher.UpdateDelimited(roots.Fingerprint());
  hasher.UpdateDelimited(*source_digest);
  for (const std::string& arg : DepScanKeyArgs(parsed, roots)) {
    hasher.UpdateDelimited(arg);
  }
  for (const std::string& name : config.extra_env_vars) {
    const char* value = std::getenv(name.c_str());
    hasher.UpdateDelimited(name);
    hasher.UpdateDelimited(value != nullptr ? value : "");
  }
  const std::string key = hasher.Hex();
  VCACHE_LOG("dep scan key " + key + " for " + parsed.source);

  // ---- lookup -------------------------------------------------------------
  //
  // Two steps: fetch the manifest for this command line, then find the entry
  // whose recorded file contents still match what is on disk.

  bool media_failed = false;
  const auto media_fail_exit = [&]() {
    return (media_failed && config.error_on_cache_media_failure)
               ? kCacheMediaFailureExit
               : 0;
  };

  std::vector<DepManifestEntry> entries;
  if (cache != nullptr) {
    storage::GetResult got = cache->Get(key);
    media_failed |= ReportCacheMediaErrors(got.errors, cache_dir);
    storage::Blob manifest_blob;
    if (got.hit && storage::DeserializeBlob(got.value, &manifest_blob) &&
        manifest_blob.has_dep_manifest) {
      if (!ParseManifest(manifest_blob.dep_manifest, &entries)) {
        VCACHE_LOG("dep scan: manifest did not parse; starting a new one");
        entries.clear();
      }
    }
  }

  if (!config.recache && cache != nullptr) {
    for (const DepManifestEntry& entry : entries) {
      if (!ManifestStillHolds(entry.files, roots)) continue;
      storage::GetResult result = cache->Get(entry.result_key);
      media_failed |= ReportCacheMediaErrors(result.errors, cache_dir);
      storage::Blob blob;
      if (!result.hit || !storage::DeserializeBlob(result.value, &blob) ||
          !blob.has_depfile) {
        VCACHE_LOG("dep scan: manifest entry matched but its result is gone");
        continue;
      }
      auto dep = ParseDepFile(blob.depfile);
      if (!dep) continue;
      RemapDepFile(&*dep, roots, MapDirection::kLocalize);
      if (!EmitDepOutput(parsed.depfile, RenderDepFile(*dep))) break;
      VCACHE_LOG("dep scan hit on " + result.layer);
      RecordCounter(cache_dir, result.layer == "s3" ? Counter::kHitS3
                                                    : Counter::kHitDisk);
      return media_fail_exit();
    }
  }
  RecordCounter(cache_dir, Counter::kMiss);

  // ---- miss: run the scan for real ----------------------------------------

  auto temp_dir = util::MakeTempDir("vcache-dep-");
  if (!temp_dir) return RunPassthrough(argv);
  struct TempDirGuard {
    std::string path;
    ~TempDirGuard() { util::RemoveRecursive(path); }
  } guard{*temp_dir};

  // Run the caller's command unchanged apart from where the answer lands, so
  // there is no chance of altering what the scan does.
  const std::string tmp_out = *temp_dir + "/deps.d";
  std::vector<std::string> cmd;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "-o" || argv[i] == "-MF") {
      ++i;
      continue;
    }
    if (util::StartsWith(argv[i], "-MF") && argv[i].size() > 3) continue;
    cmd.push_back(argv[i]);
  }
  cmd.push_back("-MF");
  cmd.push_back(tmp_out);
  VCACHE_LOG("dep scan: " + util::Join(cmd, " "));

  util::ProcResult scanned = util::Run(cmd, {.capture_stderr = true});
  if (!scanned.stderr_data.empty()) {
    ::fwrite(scanned.stderr_data.data(), 1, scanned.stderr_data.size(), stderr);
  }
  if (scanned.exit_code != 0) {
    RecordCounter(cache_dir, Counter::kCompileFailed);
    return scanned.exit_code;
  }

  auto text = util::ReadFile(tmp_out);
  if (!text) {
    VCACHE_LOG("dep scan: produced no output; rerunning directly");
    return RunPassthrough(argv);
  }

  auto dep = ParseDepFile(*text);
  if (!dep) {
    // Emit what the compiler said rather than nothing, and do not store an
    // entry vcache cannot replay faithfully.
    VCACHE_LOG("dep scan: output did not parse; passing it through unstored");
    if (!EmitDepOutput(parsed.depfile, *text)) return RunPassthrough(argv);
    RecordCounter(cache_dir, Counter::kStoreFailed);
    return media_fail_exit();
  }

  // Emit the re-rendered form, not the compiler's, so that what lands on disk
  // does not depend on whether this run was a hit. The two say the same thing;
  // only gcc's line wrapping differs, and the unwrapped form is the one cargo
  // can also read.
  if (!EmitDepOutput(parsed.depfile, RenderDepFile(*dep))) {
    VCACHE_LOG("dep scan: could not write " + parsed.depfile);
    return RunPassthrough(argv);
  }

  // ---- store --------------------------------------------------------------

  if (config.read_only || cache == nullptr) return media_fail_exit();

  // Every prerequisite becomes a manifest entry. -MP's phony rules carry no
  // prerequisites and contribute nothing, which is correct: they name the same
  // headers the first rule already lists.
  std::vector<std::pair<std::string, std::string>> files;
  for (const DepRule& rule : dep->rules) {
    for (const std::string& prereq : rule.prerequisites) {
      auto digest = hash::HashFile(prereq);
      if (!digest) {
        VCACHE_LOG("dep scan: cannot hash prerequisite " + prereq + "; not storing");
        RecordCounter(cache_dir, Counter::kStoreFailed);
        return media_fail_exit();
      }
      files.emplace_back(roots.Canonicalize(prereq), *digest);
    }
  }

  RemapDepFile(&*dep, roots, MapDirection::kCanonicalize);

  // The result key names this exact set of file contents, so re-scanning an
  // already-recorded state overwrites its own entry instead of adding another.
  hash::Hasher result_hasher;
  result_hasher.UpdateDelimited("vcache-depscan-result-v1");
  result_hasher.UpdateDelimited(key);
  for (const auto& [path, digest] : files) {
    result_hasher.UpdateDelimited(path);
    result_hasher.UpdateDelimited(digest);
  }
  const std::string result_key = result_hasher.Hex();

  storage::Blob result;
  result.depfile = RenderDepFile(*dep);
  result.has_depfile = true;
  result.stderr_text = roots.CanonicalizeText(scanned.stderr_data);
  result.meta = "dependency scan\ncompiler: " + compiler_id.description + "\n" +
                "files: " + std::to_string(files.size()) + "\n";

  const storage::PutResult result_put =
      cache->Put(result_key, storage::SerializeBlob(result));
  media_failed |= ReportCacheMediaErrors(result_put.errors, cache_dir);
  if (!result_put.stored) {
    RecordCounter(cache_dir, Counter::kStoreFailed);
    return media_fail_exit();
  }

  // Newest first, so the states in active use stay ahead of the tail that gets
  // dropped, and so lookup checks the likely match before hashing for others.
  DepManifestEntry fresh;
  fresh.result_key = result_key;
  fresh.files = std::move(files);

  std::vector<DepManifestEntry> updated;
  updated.push_back(std::move(fresh));
  for (DepManifestEntry& entry : entries) {
    if (entry.result_key == result_key) continue;  // superseded
    if (updated.size() >= kMaxManifestEntries) break;
    updated.push_back(std::move(entry));
  }

  storage::Blob manifest_blob;
  manifest_blob.dep_manifest = RenderManifest(updated);
  manifest_blob.has_dep_manifest = true;
  manifest_blob.meta = "dependency-scan manifest\nstates: " +
                       std::to_string(updated.size()) + "\n";

  const storage::PutResult manifest_put =
      cache->Put(key, storage::SerializeBlob(manifest_blob));
  media_failed |= ReportCacheMediaErrors(manifest_put.errors, cache_dir);
  if (manifest_put.stored) {
    RecordCounter(cache_dir, Counter::kStored);
  } else {
    RecordCounter(cache_dir, Counter::kStoreFailed);
  }
  return media_fail_exit();
}

bool ReportCacheMediaErrors(const std::vector<std::string>& errors,
                            const std::string& cache_dir) {
  for (const std::string& e : errors) {
    ::fprintf(stderr, "vcache: warning: cache layer failed: %s\n", e.c_str());
    RecordCounter(cache_dir, Counter::kCacheMediaError);
  }
  return !errors.empty();
}

RootMap BuildRootMap(const Config& config, std::vector<std::string>* warnings) {
  RootMap roots = RootMap::FromSpecs(config.root_specs, warnings);
  if (config.map_cwd) {
    const std::string cwd = util::CurrentDir();
    // DW_AT_comp_dir records the build directory. If it sits outside every
    // configured root it stays absolute in the object file, and two machines
    // building the same tree from different build directories will never share
    // an entry.
    if (!cwd.empty() && roots.AddIfUncovered(cwd, config.cwd_canonical_name)) {
      VCACHE_LOG("added cwd as root: " + cwd);
    }
  }
  return roots;
}

int RunCompile(const std::vector<std::string>& argv, const Config& config,
               const RootMap& roots, storage::CacheChain* cache) {
  const std::string& cache_dir = config.disk.dir;

  args::CompilerArgs parsed = args::Parse(argv);

  if (!parsed.cacheable()) {
    VCACHE_LOG("uncacheable: " + *parsed.uncacheable);
    RecordCounter(cache_dir, Counter::kUncacheable);
    return RunPassthrough(argv);
  }

  // -M/-MM without -c produces a dependency list and no object. It is cached,
  // but not by preprocessing -- see RunDepScan.
  if (parsed.dep_only) return RunDepScan(argv, config, roots, cache);

  // vcache owns path rewriting; a prefix map arriving from the build system
  // would interact with ours under gcc's last-match-wins rule.
  if (!parsed.incoming_prefix_maps.empty()) {
    switch (config.incoming_map_policy) {
      case IncomingMapPolicy::kError:
        ::fprintf(stderr,
                  "vcache: refusing to run: the command line contains %s\n"
                  "vcache manages path prefix mapping itself, and combining the "
                  "two would silently change\nwhich paths end up in your "
                  "output. Choose one:\n"
                  "  --vcache-allow-prefix-maps        drop them; vcache's "
                  "mapping wins\n"
                  "  --vcache-incoming-prefix-maps=keep pass them through "
                  "(disables caching)\n"
                  "or set vcache.incoming_prefix_maps / "
                  "VCACHE_INCOMING_PREFIX_MAPS.\n",
                  parsed.incoming_prefix_maps.front().c_str());
        return 1;
      case IncomingMapPolicy::kKeep:
        VCACHE_LOG("incoming prefix maps kept; not caching");
        RecordCounter(cache_dir, Counter::kUncacheable);
        return RunPassthrough(argv);
      case IncomingMapPolicy::kStrip:
        VCACHE_LOG("stripped incoming prefix maps: " +
                   util::Join(parsed.incoming_prefix_maps, " "));
        break;
    }
  }

  const std::string check_mode = EnvOr("VCACHE_COMPILER_CHECK", "version");
  const CompilerId compiler_id =
      ResolveCompilerId(parsed.compiler, check_mode, cache_dir);

  // -march=native and friends: resolve them to a concrete target fingerprint,
  // or decline to cache if the caller asked for that or the probe failed.
  std::string native_target;
  if (!parsed.native_flags.empty()) {
    if (config.native_target_policy == NativeTargetPolicy::kUncacheable) {
      VCACHE_LOG("uncacheable: native target flags (" +
                 util::Join(parsed.native_flags, " ") + ") and policy is 'uncacheable'");
      RecordCounter(cache_dir, Counter::kUncacheable);
      return RunPassthrough(argv);
    }
    native_target = ResolveNativeTarget(parsed, compiler_id, cache_dir);
    if (native_target.empty()) {
      VCACHE_LOG("uncacheable: could not resolve " +
                 util::Join(parsed.native_flags, " "));
      RecordCounter(cache_dir, Counter::kUncacheable);
      return RunPassthrough(argv);
    }
    VCACHE_LOG("native target " + native_target.substr(0, 16) + " for " +
               util::Join(parsed.native_flags, " "));
  }

  auto temp_dir = util::MakeTempDir("vcache-");
  if (!temp_dir) {
    VCACHE_LOG("could not create temp dir; falling back");
    return RunPassthrough(argv);
  }
  struct TempDirGuard {
    std::string path;
    ~TempDirGuard() { util::RemoveRecursive(path); }
  } guard{*temp_dir};

  // ---- preprocess ---------------------------------------------------------

  const std::string preprocessed = *temp_dir + "/pp.i";
  const std::vector<std::string> pp_cmd = BuildPreprocessCommand(parsed, roots);
  VCACHE_LOG("preprocess: " + util::Join(pp_cmd, " "));

  util::ProcResult pp = util::Run(
      pp_cmd, {.capture_stdout = true, .capture_stderr = true, .stdout_file = preprocessed});
  if (pp.exit_code != 0) {
    // The compilation itself will fail too; let the compiler produce the real
    // diagnostics rather than reporting a vcache-shaped error.
    VCACHE_LOG("preprocessing failed with exit code " + std::to_string(pp.exit_code) +
               ": " + pp.stderr_data.substr(0, 512));
    RecordCounter(cache_dir, Counter::kPreprocessFailed);
    return RunPassthrough(argv);
  }

  const std::string key =
      ComputeKey(parsed, roots, compiler_id, config, native_target, preprocessed);
  if (key.empty()) {
    VCACHE_LOG("could not compute cache key; falling back");
    return RunPassthrough(argv);
  }
  VCACHE_LOG("key " + key + " for " + parsed.source + " -> " + parsed.output);

  // ---- lookup -------------------------------------------------------------

  // Tracks whether any layer was broken, as opposed to cold, across this whole
  // invocation. A hit does not clear it: the media still failed.
  bool media_failed = false;

  if (!config.recache && cache != nullptr) {
    storage::GetResult got = cache->Get(key);
    media_failed |= ReportCacheMediaErrors(got.errors, cache_dir);
    if (got.hit) {
      storage::Blob blob;
      if (storage::DeserializeBlob(got.value, &blob) &&
          MaterializeHit(blob, parsed, roots)) {
        VCACHE_LOG("hit on " + got.layer);
        RecordCounter(cache_dir, got.layer == "s3" ? Counter::kHitS3
                                                   : Counter::kHitDisk);
        if (media_failed && config.error_on_cache_media_failure) {
          return kCacheMediaFailureExit;
        }
        return 0;
      }
      VCACHE_LOG("hit on " + got.layer + " but entry was unusable; recompiling");
    }
  }
  RecordCounter(cache_dir, Counter::kMiss);

  // ---- miss: compile for real ---------------------------------------------

  // Compile to a temporary and move into place only on success, so a failed
  // build never leaves a half-written object where make expects a good one.
  const std::string tmp_output = *temp_dir + "/out" ;
  const std::string tmp_depfile =
      parsed.generates_deps && !parsed.depfile.empty() ? *temp_dir + "/out.d" : "";

  const std::vector<std::string> compile_cmd =
      BuildCompileCommand(parsed, roots, tmp_output, tmp_depfile);
  VCACHE_LOG("compile: " + util::Join(compile_cmd, " "));

  util::ProcResult compiled = util::Run(compile_cmd, {.capture_stderr = true});

  if (!compiled.stderr_data.empty()) {
    ::fwrite(compiled.stderr_data.data(), 1, compiled.stderr_data.size(), stderr);
  }
  if (compiled.exit_code != 0) {
    VCACHE_LOG("compile failed with exit code " + std::to_string(compiled.exit_code));
    RecordCounter(cache_dir, Counter::kCompileFailed);
    return compiled.exit_code;
  }

  if (!util::LinkOrCopy(tmp_output, parsed.output)) {
    VCACHE_LOG("could not place object at " + parsed.output + "; rerunning directly");
    return RunPassthrough(argv);
  }

  std::string canonical_depfile;
  bool have_depfile = false;
  if (!tmp_depfile.empty()) {
    auto text = util::ReadFile(tmp_depfile);
    if (text) {
      if (!util::WriteFileAtomic(parsed.depfile, *text)) {
        VCACHE_LOG("could not write dependency file " + parsed.depfile);
      }
      // Store it canonicalised so the entry is directory-independent; gcc does
      // not remap dependency output itself.
      if (auto dep = ParseDepFile(*text)) {
        RemapDepFile(&*dep, roots, MapDirection::kCanonicalize);
        canonical_depfile = RenderDepFile(*dep);
        have_depfile = true;
      } else {
        VCACHE_LOG("dependency file did not parse; storing entry without it");
      }
    }
  }

  // ---- store --------------------------------------------------------------

  if (config.read_only || cache == nullptr) {
    if (media_failed && config.error_on_cache_media_failure &&
        compiled.exit_code == 0) {
      return kCacheMediaFailureExit;
    }
    return compiled.exit_code;
  }

  auto object_bytes = util::ReadFile(tmp_output);
  if (!object_bytes) {
    RecordCounter(cache_dir, Counter::kStoreFailed);
    return compiled.exit_code;
  }

  // A depfile that failed to parse means the entry cannot be replayed
  // faithfully, so skip storing rather than serve an incomplete result.
  if (parsed.generates_deps && !parsed.depfile.empty() && !have_depfile) {
    RecordCounter(cache_dir, Counter::kStoreFailed);
    return compiled.exit_code;
  }

  storage::Blob blob;
  blob.object = std::move(*object_bytes);
  blob.depfile = std::move(canonical_depfile);
  blob.has_depfile = have_depfile;
  blob.stderr_text = roots.CanonicalizeText(compiled.stderr_data);
  blob.meta = "compiler: " + compiler_id.description + "\n" +
              "language: " + args::LanguageName(parsed.language) + "\n" +
              "roots:\n" + roots.DebugString();

  const storage::PutResult put = cache->Put(key, storage::SerializeBlob(blob));
  media_failed |= ReportCacheMediaErrors(put.errors, cache_dir);
  if (put.stored) {
    RecordCounter(cache_dir, Counter::kStored);
  } else {
    RecordCounter(cache_dir, Counter::kStoreFailed);
  }
  // Only override a successful compile. A real compiler failure is the more
  // useful exit code to propagate, and the object is already in place either
  // way, so this reports the cache fault without hiding the build result.
  if (media_failed && config.error_on_cache_media_failure &&
      compiled.exit_code == 0) {
    return kCacheMediaFailureExit;
  }
  return compiled.exit_code;
}

}  // namespace vcache::core
