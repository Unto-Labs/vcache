// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "core/compile.h"

#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "args/compiler_args.h"
#include "core/cost.h"
#include "core/cxx_manifest.h"
#include "core/depfile.h"
#include "core/link_trace.h"
#include "core/manifest.h"
#include "core/preprocessed.h"
#include "core/stats.h"
#include "daemon/client.h"
#include "hash/file_memo.h"
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
constexpr std::string_view kCacheKeyVersion = "vcache-key-v4";
constexpr std::string_view kPchIdentityVersion = "vcache-pch-identity-v1";

// Runs the compiler unchanged and returns its exit code. The fallback path for
// everything vcache cannot or should not cache.
}  // namespace

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

// The patch prints its token after the version, on the line that reports it.
// Elsewhere in the banner the same text can be part of a configure flag or a
// path, and says nothing about the driver.
int AbsentDepsVersion(std::string_view banner) {
  static constexpr std::string_view kVersionWord = "version ";
  static constexpr std::string_view kMark = "absent-deps-";
  for (const std::string& line : util::Split(banner, '\n', /*skip_empty=*/true)) {
    if (util::StartsWith(line, "Configured with:")) continue;
    const size_t version = line.find(kVersionWord);
    const size_t number = version + kVersionWord.size();
    if (version == std::string::npos || number == line.size() ||
        !std::isdigit(static_cast<unsigned char>(line[number]))) {
      continue;
    }
    size_t at = number;
    while ((at = line.find(kMark, at)) != std::string::npos) {
      const size_t digits = at + kMark.size();
      size_t end = digits;
      while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end]))) ++end;
      const bool bounded = std::isspace(static_cast<unsigned char>(line[at - 1])) &&
                           (end == line.size() ||
                            std::isspace(static_cast<unsigned char>(line[end])));
      int version_number = 0;
      if (end != digits && bounded &&
          std::from_chars(line.data() + digits, line.data() + end, version_number).ec ==
              std::errc()) {
        return version_number;
      }
      at = digits;
    }
    return 0;
  }
  return 0;
}

namespace {

// Which driver this is. The version banner settles it whenever there is one;
// the compiler-check modes that skip the probe fall back to the name, which is
// what a cross-compiler prefix or a versioned suffix leaves intact.
bool LooksLikeClang(const std::string& banner, const std::string& path) {
  if (!banner.empty()) return banner.find("clang version") != std::string::npos;
  return util::BaseName(path).find("clang") != std::string::npos;
}

// Assembles the preprocessing command: the original flags, plus the root
// prefix maps, minus anything that would write files.
//
// -fno-working-directory is essential under gcc. It emits a `# 1 "<cwd>//"`
// linemarker whenever debug info is enabled, and -ffile-prefix-map does *not*
// rewrite it, so without this flag the preprocessed text differs between build
// directories and every cross-directory lookup misses.
//
// clang emits no such linemarker and rejects the flag as unused -- fatally so
// under -Werror, which would turn every clang compile into a failed preprocess
// and silently disable the cache. So it goes only to the driver that needs it.
std::vector<std::string> BuildPreprocessCommand(const args::CompilerArgs& parsed,
                                                const RootMap& roots,
                                                const CompilerId& compiler_id) {
  std::vector<std::string> cmd;
  cmd.push_back(parsed.compiler);
  // base_args deliberately excludes dependency flags: gcc rejects -MP or -MF
  // when neither -M nor -MM is present, so passing them through here would make
  // every preprocessing run fail.
  for (const std::string& arg : parsed.base_args) cmd.push_back(arg);
  for (const std::string& arg : roots.PrefixMapArgs(PrefixMapStyle::kC)) {
    cmd.push_back(arg);
  }
  if (!compiler_id.is_clang) cmd.push_back("-fno-working-directory");
  // A PCH carries the header's macro definitions to every compile that loads
  // it, and plain -E drops them: a header that only changes `#define FOO 1` to
  // `#define FOO 2` would preprocess identically, and clang's size check would
  // accept the old PCH too. -dD keeps the definitions in the text.
  if (parsed.produces_pch) cmd.push_back("-dD");
  cmd.push_back("-E");
  cmd.push_back(parsed.source);
  return cmd;
}

// Assembles the real compile command with prefix maps injected.
//
// `add_md` asks for a depfile the caller did not: the C/C++ manifest records
// the files a compile read from it. -MD writes the file beside the object and
// changes nothing in the object itself. A non-empty `absent_list_path` asks a
// compiler with the absent-deps patch to write there the paths it looked for
// and did not find, which the manifest records too.
std::vector<std::string> BuildCompileCommand(const args::CompilerArgs& parsed,
                                             const RootMap& roots,
                                             const std::string& output,
                                             const std::string& depfile,
                                             bool add_md = false,
                                             const std::string& absent_list_path = "") {
  std::vector<std::string> cmd;
  cmd.push_back(parsed.compiler);
  for (const std::string& arg : parsed.base_args) cmd.push_back(arg);
  if (parsed.xclang_emit_pch) {
    cmd.push_back("-Xclang");
    cmd.push_back("-emit-pch");
  }
  for (const std::string& arg : roots.PrefixMapArgs(PrefixMapStyle::kC)) {
    cmd.push_back(arg);
  }
  // Dependency flags only belong on the real compile.
  if (!depfile.empty()) {
    for (const std::string& arg : parsed.dep_args) cmd.push_back(arg);
    if (add_md) cmd.push_back("-MD");
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
  if (!absent_list_path.empty()) cmd.push_back("-fabsent-deps=" + absent_list_path);
  cmd.push_back("-o");
  cmd.push_back(output);
  cmd.push_back(parsed.source);
  return cmd;
}

// The driver's -x value to probe a native target with. The predefined macro set
// differs slightly between C and C++, so probe in the language being compiled.
const char* NativeProbeLanguage(args::Language lang) {
  switch (lang) {
    case args::Language::kCxx:
    case args::Language::kCxxHeader: return "c++";
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

// The meta line that carries a PCH build's identity, so a hit can write the
// key file without the preprocessed text.
constexpr std::string_view kPchIdentityMetaKey = "pch-identity: ";

std::string PchIdentityFromMeta(const std::string& meta) {
  size_t pos = 0;
  while (pos < meta.size()) {
    const size_t end = meta.find('\n', pos);
    const std::string_view line(meta.data() + pos, (end == std::string::npos ? meta.size() : end) - pos);
    if (util::StartsWith(line, kPchIdentityMetaKey)) {
      return std::string(line.substr(kPchIdentityMetaKey.size()));
    }
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  return "";
}

std::string ComputeKey(const args::CompilerArgs& parsed, const RootMap& roots,
                       const CompilerId& compiler_id, const Config& config,
                       const std::string& native_target,
                       const std::string& preprocessed_path, bool* saw_incbin,
                       std::string* pch_identity) {
  hash::Hasher hasher;
  // A PCH build's identity takes every input of its key except the paths as
  // typed, so it is the same in every worktree; consumers' manifests name the
  // PCH by it where the build writes no .sum (see PchIdentity).
  hash::Hasher identity;
  const bool want_identity = parsed.produces_pch && pch_identity != nullptr;
  if (want_identity) identity.UpdateDelimited(kPchIdentityVersion);
  const auto feed = [&](std::string_view text) {
    hasher.UpdateDelimited(text);
    if (want_identity) identity.UpdateDelimited(text);
  };
  feed(kCacheKeyVersion);
  feed(compiler_id.fingerprint);
  feed(native_target);
  feed(parsed.assemble_only ? "assembly" : "object");

  // The two key inputs that are neither the source nor the flags, logged
  // because a miss caused by either is otherwise invisible: the command line
  // and the source can be identical in two checkouts while one of these
  // quietly differs.
  VCACHE_LOG("key input compiler=" + compiler_id.fingerprint + " (" +
             compiler_id.description + ") roots=" + roots.Fingerprint() +
             " native=" + native_target);

  // Root mapping participates in the key: the same source canonicalised under
  // a different mapping produces a different object.
  feed(roots.Fingerprint());

  // Only codegen-affecting flags, with any embedded paths canonicalised so that
  // -I-style differences between checkouts do not change the key.
  for (const std::string& arg : parsed.key_args) {
    feed(roots.Canonicalize(arg));
  }

  // A PCH records the absolute path of every input, even one named relatively,
  // and clang refuses to load it once one of them is missing, so it is reusable
  // only at the paths it was built at. The directory and the command line go in
  // as typed, not canonicalised, so two trees at different paths never share a
  // PCH entry; a rebuild of the same tree, or containers that mount every tree
  // at one path, still hit.
  if (parsed.produces_pch) {
    hasher.UpdateDelimited("pch-built-at");
    hasher.UpdateDelimited(util::CurrentDir());
    for (const std::string& arg : parsed.argv) hasher.UpdateDelimited(arg);
  }

  // Linker-only flags change nothing about the object under -c, and gcc ignores
  // them in silence, so they stay out of the key: a -L or -Wl,-rpath carrying a
  // machine-specific path no longer forks the entry across machines. clang
  // instead names the flag in an "unused" warning, which vcache stores and
  // replays on a hit, so there the key has to keep them or the replayed text
  // would report a flag the caller never passed.
  if (compiler_id.is_clang) {
    for (const std::string& arg : parsed.link_args) {
      feed(roots.Canonicalize(arg));
    }
  }

  // Files the compiler reads after preprocessing -- sanitizer ignore lists,
  // sample profiles, plugins. Their contents never reach the preprocessed text,
  // so they are hashed here. Hashing to a digest first keeps the framing fixed
  // width, so two adjacent files cannot run together. One that cannot be read
  // means no key at all: running the compiler beats guessing at its input.
  for (const std::string& path : parsed.key_files) {
    auto digest = hash::HashFile(path);
    if (!digest) {
      VCACHE_LOG("cannot hash keyed input file " + path);
      return "";
    }
    feed(*digest);
  }

  for (const std::string& name : config.extra_env_vars) {
    const char* value = std::getenv(name.c_str());
    feed(name);
    feed(value != nullptr ? value : "");
  }

  // The preprocessed text carries the full effect of every -I/-D/-include flag
  // and of the source itself. Its linemarkers still hold raw absolute paths --
  // gcc does not apply -ffile-prefix-map to them -- so they are canonicalised
  // on the way into the hash.
  if (!HashNormalizedPreprocessedOutput(preprocessed_path, roots, &hasher, saw_incbin,
                                        want_identity ? &identity : nullptr)) {
    return "";
  }
  if (want_identity) *pch_identity = identity.Hex();
  return hasher.Hex();
}

// Writes the cached artifacts into the working tree. `lookup_started` is when
// the entry was asked for, so the log can show the whole hit, not only the writes.
bool MaterializeHit(const storage::Blob& blob, const args::CompilerArgs& parsed,
                    const RootMap& roots,
                    std::chrono::steady_clock::time_point lookup_started) {
  const auto restore_started = std::chrono::steady_clock::now();
  size_t restored_files = 1;
  uint64_t restored_bytes = blob.object.size();
  if (!util::WriteFileAtomic(parsed.output, blob.object, /*durable=*/false)) {
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

    const std::string depfile_text = RenderDepFile(*dep);
    if (!util::WriteFileAtomic(parsed.depfile, depfile_text, /*durable=*/false)) {
      VCACHE_LOG("hit: failed to write dependency file " + parsed.depfile);
      return false;
    }
    ++restored_files;
    restored_bytes += depfile_text.size();
  }
  VCACHE_LOG("hit: restored " + std::to_string(restored_files) + " files, " +
             std::to_string(restored_bytes) + " bytes in " +
             util::MillisecondsSince(restore_started) + " ms");
  VCACHE_LOG("hit total " + util::MillisecondsSince(lookup_started) + " ms");

  if (!blob.stderr_text.empty()) {
    const std::string text = roots.LocalizeText(blob.stderr_text);
    ::fwrite(text.data(), 1, text.size(), stderr);
  }
  return true;
}

// `<compiler> -v` as printed, or nullopt when the probe fails.
//
// Spawning a process per compilation would be wasteful, so the answer is
// memoised in the cache directory keyed by the binary's identity.
// The memo key must change whenever the binary does, or a compiler rebuilt in
// place is answered from the previous one's banner. Size alone does not catch
// that: relinking a compiler after a source change routinely lands on the
// same byte count, and the stale banner then decides the cache key for every
// compilation the new binary performs. mtime is safe to use here even though
// it is not machine-independent, because the memo is a purely local record of
// a local probe -- what travels between machines is the banner it holds.
std::optional<std::string> CompilerBanner(const std::string& compiler,
                                          const std::string& real, uint64_t size,
                                          int64_t mtime, const std::string& cache_dir) {
  hash::Hasher memo_key;
  memo_key.UpdateDelimited("compiler-version-memo-v2");
  memo_key.UpdateDelimited(real);
  memo_key.UpdateU64(size);
  memo_key.UpdateU64(static_cast<uint64_t>(mtime));
  const std::string memo_path = cache_dir + "/compilers/" + memo_key.Hex();
  if (auto cached = util::ReadFile(memo_path)) return cached;

  util::ProcResult probe = util::Run({compiler, "-v"},
                                     {.capture_stdout = true, .capture_stderr = true});
  // gcc and clang both print the version banner on stderr.
  std::string banner = probe.stderr_data + probe.stdout_data;
  if (probe.exit_code != 0 || banner.empty()) return std::nullopt;
  util::WriteFileAtomic(memo_path, banner);
  return banner;
}

}  // namespace

CompilerId ResolveCompilerId(const std::string& compiler,
                             const std::string& check_mode,
                             const std::string& cache_dir,
                             const RootMap& roots) {
  CompilerId id;
  const std::string real =
      util::RealPath(compiler).value_or(compiler);

  struct stat_info {
    uint64_t size = 0;
    int64_t mtime = 0;
  } info;
  if (auto size = util::FileSize(real)) info.size = *size;
  if (auto mtime = util::FileMtime(real)) info.mtime = *mtime;

  // Set on every path below, including the ones that never read a banner.
  id.is_clang = LooksLikeClang("", real);

  // Every mode reads the version, which decides what the compiler can be asked
  // to do; only `version` hashes the banner.
  const std::optional<std::string> banner =
      CompilerBanner(compiler, real, info.size, info.mtime, cache_dir);
  if (banner) id.absent_deps_version = AbsentDepsVersion(*banner);

  if (check_mode == "mtime") {
    hash::Hasher hasher;
    hasher.UpdateDelimited(roots.Canonicalize(real));
    hasher.UpdateU64(info.size);
    hasher.UpdateU64(static_cast<uint64_t>(info.mtime));
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
  if (!banner) {
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

  id.is_clang = LooksLikeClang(*banner, real);
  // The memo is keyed by the local path, so it holds the banner as printed.
  // Canonicalise on use rather than on store, so the same memo stays correct
  // if the root mapping changes.
  id.fingerprint = hash::HashString(roots.CanonicalizeText(*banner));
  const auto lines = util::Split(*banner, '\n', /*skip_empty=*/true);
  id.description = lines.empty() ? real : lines.back();
  return id;
}

namespace {

// Bumped independently of the compile key: the two never share entries.
constexpr std::string_view kDepScanKeyVersion = "vcache-depscan-v4";
constexpr std::string_view kManifestHeader = "vcache-depmanifest-2";

// One remembered state: the files a scan read, and where its answer is stored.
struct DepManifestEntry {
  std::string result_key;
  std::vector<ManifestFile> files;
};

}  // namespace

std::vector<std::string> DepScanKeyArgs(const args::CompilerArgs& parsed,
                                        const RootMap& roots,
                                        bool keep_link_args) {
  const auto canonical_arg = [&](const std::string& arg) {
    // Macro values and tool selectors such as -B must remain as spelled.
    static constexpr std::string_view kPathPrefixes[] = {
        "-include-pch=", "-isystem", "-iquote", "-idirafter", "-include", "-imacros",
        "-isysroot", "-iframework", "-iprefix", "-iwithprefixbefore", "-iwithprefix",
        "--sysroot=", "-I", "-F", "-L", "-fsanitize-ignorelist=", "-fsanitize-blacklist=",
        "-fsanitize-system-ignorelist=", "-fsanitize-coverage-allowlist=",
        "-fsanitize-coverage-ignorelist=", "-fprofile-list=", "-fprofile-sample-use=",
        "-fprofile-remapping-file=",
    };
    for (std::string_view prefix : kPathPrefixes) {
      if (arg.size() > prefix.size() && util::StartsWith(arg, prefix)) {
        return std::string(prefix) + roots.Canonicalize(std::string_view(arg).substr(prefix.size()));
      }
    }
    return roots.Canonicalize(arg);
  };
  std::vector<std::string> out;
  for (size_t i = 1; i < parsed.argv.size(); ++i) {
    const std::string& arg = parsed.argv[i];
    if (arg == "-o" || arg == "-MF") {
      ++i;  // and its value
      continue;
    }
    if (util::StartsWith(arg, "-MF") && arg.size() > 3) continue;
    // A dependency list is no more sensitive to linker flags than an object is;
    // the driver split is the same one ComputeKey makes, and for the same
    // reason -- this path stores clang's diagnostics too.
    if (!keep_link_args && args::IsLinkOnlyFlag(arg)) {
      if (args::LinkOnlyFlagTakesValue(arg) && i + 1 < parsed.argv.size()) ++i;
      continue;
    }
    out.push_back(canonical_arg(arg));
  }
  return out;
}

namespace {

// A header line, then per entry an "entry <key>" line followed by one
// RenderManifestFile line per file.
std::string RenderManifest(const std::vector<DepManifestEntry>& entries) {
  std::string out(kManifestHeader);
  out.push_back('\n');
  for (const DepManifestEntry& entry : entries) {
    out += "entry ";
    out += entry.result_key;
    out.push_back('\n');
    for (const ManifestFile& file : entry.files) {
      out += RenderManifestFile(file);
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
    ManifestFile file;
    if (!ParseManifestFile(line, &file)) return false;
    entries->back().files.push_back(std::move(file));
  }
  return true;
}

// The list a manifest blob holds, or empty when there is none or it does not
// parse. Lookup and the re-read before a store share this so a corrupt
// manifest is dropped the same way in both places.
std::vector<DepManifestEntry> DepManifestEntries(const storage::GetResult& got) {
  std::vector<DepManifestEntry> entries;
  storage::Blob blob;
  if (!got.hit || !storage::DeserializeBlob(got.value, &blob) || !blob.has_dep_manifest) {
    return entries;
  }
  if (!ParseManifest(blob.dep_manifest, &entries)) {
    VCACHE_LOG("dep scan: manifest did not parse; starting a new one");
    entries.clear();
  }
  return entries;
}

// Writes the dependency text where the caller asked for it: the named file, or
// stdout when neither -o nor -MF was given.
bool EmitDepOutput(const std::string& path, const std::string& text) {
  if (path.empty()) {
    ::fwrite(text.data(), 1, text.size(), stdout);
    ::fflush(stdout);
    return true;
  }
  return util::WriteFileAtomic(path, text, /*durable=*/false);
}

}  // namespace

int RunDepScan(const std::vector<std::string>& argv, const Config& config,
               const RootMap& roots, storage::CacheChain* cache) {
  const std::string& cache_dir = config.disk.dir;
  const args::CompilerArgs parsed = args::Parse(argv);

  if (config.dep_scan_policy == DepScanPolicy::kUncacheable) {
    RecordDecision(cache_dir, Reason::kDepScanPolicy);
    return RunPassthrough(argv);
  }

  const std::string check_mode = EnvOr("VCACHE_COMPILER_CHECK", "version");
  const CompilerId compiler_id =
      ResolveCompilerId(parsed.compiler, check_mode, cache_dir, roots);

  std::string native_target;
  if (!parsed.native_flags.empty()) {
    if (config.native_target_policy == NativeTargetPolicy::kUncacheable) {
      RecordDecision(cache_dir,
                     {Reason::kNativeTargetPolicy, util::Join(parsed.native_flags, " ")});
      return RunPassthrough(argv);
    }
    native_target = ResolveNativeTarget(parsed, compiler_id, cache_dir);
    if (native_target.empty()) {
      RecordDecision(cache_dir,
                     {Reason::kNativeTargetUnresolved, util::Join(parsed.native_flags, " ")});
      return RunPassthrough(argv);
    }
  }

  // The source is hashed into the key rather than verified through the
  // manifest, so that editing a file back and forth does not make two versions
  // fight over one entry.
  auto source_digest = hash::HashFile(parsed.source);
  if (!source_digest) {
    RecordDecision(cache_dir, {Reason::kUnreadableSource, parsed.source});
    return RunPassthrough(argv);
  }

  hash::Hasher hasher;
  hasher.UpdateDelimited(kDepScanKeyVersion);
  hasher.UpdateDelimited(compiler_id.fingerprint);
  hasher.UpdateDelimited(native_target);
  hasher.UpdateDelimited(roots.Fingerprint());
  hasher.UpdateDelimited(*source_digest);
  for (const std::string& arg : DepScanKeyArgs(parsed, roots, compiler_id.is_clang)) {
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
    entries = DepManifestEntries(got);
  }

  if (!config.recache && cache != nullptr) {
    for (const DepManifestEntry& entry : entries) {
      // A file that has been deleted, changed or replaced rules the entry out,
      // which is the whole guarantee behind this mode.
      if (auto stale = FindStaleManifestFile(entry.files, roots)) {
        VCACHE_LOG("dep manifest: " + *stale);
        continue;
      }
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

  auto temp_dir = util::MakeScratchDir(cache_dir, "vcache-dep-");
  if (!temp_dir) {
    RecordDecision(cache_dir, Reason::kNoTempDir);
    return RunPassthrough(argv);
  }
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
    RecordDecision(cache_dir, Reason::kNoDepScanOutput);
    return RunPassthrough(argv);
  }

  auto dep = ParseDepFile(*text);
  if (!dep) {
    // Emit what the compiler said rather than nothing, and do not store an
    // entry vcache cannot replay faithfully.
    VCACHE_LOG("dep scan: output did not parse; passing it through unstored");
    if (!EmitDepOutput(parsed.depfile, *text)) {
      RecordDecision(cache_dir, {Reason::kDepfileUnwritable, parsed.depfile});
      return RunPassthrough(argv);
    }
    RecordCounter(cache_dir, Counter::kStoreFailed);
    return media_fail_exit();
  }

  // Emit the re-rendered form, not the compiler's, so that what lands on disk
  // does not depend on whether this run was a hit. The two say the same thing;
  // only gcc's line wrapping differs, and the unwrapped form is the one cargo
  // can also read.
  if (!EmitDepOutput(parsed.depfile, RenderDepFile(*dep))) {
    RecordDecision(cache_dir, {Reason::kDepfileUnwritable, parsed.depfile});
    return RunPassthrough(argv);
  }

  // ---- store --------------------------------------------------------------

  if (config.read_only || cache == nullptr) return media_fail_exit();

  // Every prerequisite becomes a manifest entry. -MP's phony rules carry no
  // prerequisites and contribute nothing, which is correct: they name the same
  // headers the first rule already lists.
  std::vector<ManifestFile> files;
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

  DepManifestEntry fresh;
  fresh.result_key = result_key;
  fresh.files = std::move(files);
  PauseBeforeManifestPut();
  storage::GetResult again = cache->Get(key);
  const bool reread_failed = ReportCacheMediaErrors(again.errors, cache_dir);
  media_failed |= reread_failed;
  const std::vector<DepManifestEntry> reread = DepManifestEntries(again);
  const std::vector<DepManifestEntry> updated = MergeManifestStates(
      std::move(fresh), std::move(reread), entries, &DepManifestEntry::result_key);

  storage::Blob manifest_blob;
  manifest_blob.dep_manifest = RenderManifest(updated);
  manifest_blob.has_dep_manifest = true;
  manifest_blob.meta = "dependency-scan manifest\nstates: " +
                       std::to_string(updated.size()) + "\n";

  const storage::PutResult manifest_put =
      cache->Put(key, storage::SerializeBlob(manifest_blob));
  media_failed |= ReportCacheMediaErrors(manifest_put.errors, cache_dir, !reread_failed);
  if (manifest_put.stored) {
    RecordCounter(cache_dir, Counter::kStored);
  } else {
    RecordCounter(cache_dir, Counter::kStoreFailed);
  }
  return media_fail_exit();
}

bool ReportCacheMediaErrors(const std::vector<std::string>& errors,
                            const std::string& cache_dir, bool count) {
  for (const std::string& e : errors) {
    ::fprintf(stderr, "vcache: warning: cache layer failed: %s\n", e.c_str());
    if (count) RecordCounter(cache_dir, Counter::kCacheMediaError);
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

namespace {

// New kinds of entry, so neither changes an existing key: the manifest itself,
// and the result key of a compile that could not be preprocessed.
constexpr std::string_view kCxxManifestKeyVersion = "vcache-cxx-manifest-v4";
constexpr std::string_view kCxxDerivedKeyVersion = "vcache-cxx-derived-v1";

// gcc writes a dependency file named by one of these from the preprocessor.
// A manifest hit runs none, and a compile with -MD added ignores them.
constexpr const char* kDependencyEnvVars[] = {"DEPENDENCIES_OUTPUT", "SUNPRO_DEPENDENCIES"};

bool UsesDependencyFlag(const args::CompilerArgs& parsed, std::string_view flag) {
  for (const std::string& arg : parsed.dep_args) {
    if (arg == flag) return true;
  }
  return false;
}

// The manifest key: everything the preprocessor key covers except the
// preprocessed text, plus everything that decides that text before a header is
// read. The working directory is in it because relative -I paths and relative
// sources resolve against it; the source's canonical path because quoted
// includes resolve beside it. Returns empty, after logging why, when this
// compile cannot use a manifest.
std::string ComputeCxxManifestKey(const args::CompilerArgs& parsed, const RootMap& roots,
                                  const CompilerId& compiler_id, const Config& config,
                                  const std::string& native_target, const std::string& cwd,
                                  const std::string& memo_dir,
                                  std::vector<std::string>* keyed_pchs) {
  // -MMD leaves system headers out of the depfile, so a state recorded from it
  // would not notice a system header changing.
  if (UsesDependencyFlag(parsed, "-MMD")) {
    VCACHE_LOG("cxx manifest: -MMD omits system headers; using the preprocessor");
    return "";
  }
  for (const char* name : kDependencyEnvVars) {
    if (std::getenv(name) != nullptr) {
      VCACHE_LOG(std::string("cxx manifest: ") + name + " is set; using the preprocessor");
      return "";
    }
  }
  // -DSTAMP=__TIME__ expands differently on every run, and nothing the manifest
  // hashes would change.
  for (const std::string& arg : parsed.argv) {
    if (const auto macro = FindTimeMacro(arg)) {
      VCACHE_LOG("cxx manifest: the command line uses " + std::string(*macro) +
                 "; using the preprocessor");
      return "";
    }
  }
  const ConsumedPchs pchs = FindConsumedPchs(parsed, cwd);
  if (!pchs.unsupported.empty()) {
    VCACHE_LOG("cxx manifest: " + pchs.unsupported + "; using the preprocessor");
    return "";
  }
  std::vector<std::string> pch_identities;
  for (const std::string& pch : pchs.paths) {
    std::optional<std::string> identity = PchIdentity(pch, memo_dir);
    if (!identity) {
      VCACHE_LOG("cxx manifest: cannot read PCH " + pch + "; using the preprocessor");
      return "";
    }
    VCACHE_LOG("cxx manifest: PCH " + pch + " is " + identity->substr(0, identity->find(' ')));
    pch_identities.push_back(std::move(*identity));
  }
  const std::optional<std::string> source_digest =
      hash::HashFileMemoized(parsed.source, memo_dir);
  if (!source_digest) {
    VCACHE_LOG("cxx manifest: cannot read " + parsed.source + "; using the preprocessor");
    return "";
  }

  hash::Hasher hasher;
  hasher.UpdateDelimited(kCxxManifestKeyVersion);
  // States name result keys of this version, so a bump must not be bypassed by
  // a manifest written before it.
  hasher.UpdateDelimited(kCacheKeyVersion);
  hasher.UpdateDelimited(compiler_id.fingerprint);
  hasher.UpdateDelimited(native_target);
  hasher.UpdateDelimited(roots.Fingerprint());
  hasher.UpdateDelimited(roots.Canonicalize(cwd));
  hasher.UpdateDelimited(roots.Canonicalize(CxxAbsolutePath(cwd, parsed.source)));
  // And as typed: __FILE__ and the debug info name the source as the command
  // line spelled it, so src/f.c and /abs/src/f.c in one directory differ.
  hasher.UpdateDelimited(roots.Canonicalize(parsed.source));
  hasher.UpdateDelimited(*source_digest);
  // A PCH loads only at the paths that built it, so ComputeKey keys a PCH build
  // on the raw cwd and argv; a manifest state names that key, and must not
  // hand a tree at another path the entry.
  if (parsed.produces_pch) {
    hasher.UpdateDelimited("pch-built-at");
    hasher.UpdateDelimited(cwd);
    for (const std::string& arg : parsed.argv) hasher.UpdateDelimited(arg);
  }
  const std::vector<std::string> key_args =
      CxxManifestKeyArgs(parsed, roots, compiler_id.is_clang);
  hasher.UpdateU64(key_args.size());
  for (const std::string& arg : key_args) hasher.UpdateDelimited(arg);
  for (const std::string& path : parsed.key_files) {
    const std::optional<std::string> digest = hash::HashFileMemoized(path, memo_dir);
    if (!digest) {
      VCACHE_LOG("cxx manifest: cannot hash keyed input file " + path);
      return "";
    }
    hasher.UpdateDelimited(*digest);
  }
  for (const std::string& name : config.extra_env_vars) {
    const char* value = std::getenv(name.c_str());
    hasher.UpdateDelimited(name);
    hasher.UpdateDelimited(value != nullptr ? value : "");
  }
  for (const char* name : kIncludePathEnvVars) {
    const char* value = std::getenv(name);
    hasher.UpdateDelimited(name);
    hasher.UpdateDelimited(value != nullptr ? "=" + roots.CanonicalizeText(value) : "unset");
  }
  hasher.UpdateU64(pch_identities.size());
  for (const std::string& identity : pch_identities) hasher.UpdateDelimited(identity);
  *keyed_pchs = pchs.paths;
  return hasher.Hex();
}

// The result key of a compile the preprocessor could not handle, from the
// state's digests, as ccache's depend mode keys every result. Only a manifest
// lookup can name it.
std::string DeriveCxxResultKey(const std::string& manifest_key,
                               const std::vector<ManifestFile>& files) {
  hash::Hasher hasher;
  hasher.UpdateDelimited(kCxxDerivedKeyVersion);
  hasher.UpdateDelimited(manifest_key);
  for (const auto& [path, digest] : files) {
    hasher.UpdateDelimited(path);
    hasher.UpdateDelimited(digest);
  }
  return hasher.Hex();
}

std::vector<CxxManifestState> LoadCxxManifest(storage::CacheChain* cache,
                                              const std::string& manifest_key,
                                              const std::string& cache_dir,
                                              bool* media_failed, bool log_if_absent,
                                              bool* get_failed) {
  std::vector<CxxManifestState> states;
  storage::GetResult got = cache->Get(manifest_key);
  const bool failed = ReportCacheMediaErrors(got.errors, cache_dir);
  *media_failed |= failed;
  if (get_failed != nullptr) *get_failed = failed;
  storage::Blob blob;
  if (!got.hit || !storage::DeserializeBlob(got.value, &blob) || !blob.has_dep_manifest) {
    if (log_if_absent) VCACHE_LOG("cxx manifest: none stored");
    return states;
  }
  if (!ParseCxxManifest(blob.dep_manifest, &states)) {
    VCACHE_LOG("cxx manifest: did not parse; starting a new one");
    states.clear();
  }
  return states;
}

void StoreCxxManifest(storage::CacheChain* cache, const std::string& manifest_key,
                      CxxManifestState fresh, const std::vector<CxxManifestState>& loaded,
                      const std::string& cache_dir, bool* media_failed) {
  PauseBeforeManifestPut();
  bool reread_failed = false;
  const std::vector<CxxManifestState> reread =
      LoadCxxManifest(cache, manifest_key, cache_dir, media_failed, false, &reread_failed);
  const std::vector<CxxManifestState> states = MergeManifestStates(
      std::move(fresh), reread, loaded, &CxxManifestState::result_key);
  storage::Blob blob;
  blob.dep_manifest = RenderCxxManifest(states);
  blob.has_dep_manifest = true;
  blob.meta = "c/c++ manifest\nstates: " + std::to_string(states.size()) + "\n";
  const storage::PutResult put = cache->Put(manifest_key, storage::SerializeBlob(blob));
  *media_failed |= ReportCacheMediaErrors(put.errors, cache_dir, !reread_failed);
  VCACHE_LOG(put.stored ? "cxx manifest: stored " + std::to_string(states.size()) + " states"
                        : std::string("cxx manifest: could not store"));
}

// The compiler's own <...> include directories, from `<compiler> -E -v` in the
// compile's language with the flags that can move them. Memoised in the cache
// directory, since the probe is a process; empty when it fails.
//
// The probe runs without the CPATH family: gcc lists those directories among
// its own, and a project header there would pass for a compiler header. That
// also keeps the memo independent of them. GCC_EXEC_PREFIX and COMPILER_PATH
// choose which compiler parts run, so they are in the memo key.
std::vector<std::string> SystemIncludeDirs(const args::CompilerArgs& parsed,
                                           const CompilerId& compiler_id,
                                           const std::string& cache_dir) {
  std::vector<std::string> cmd{parsed.compiler};
  for (size_t i = 0; i < parsed.key_args.size(); ++i) {
    const std::string& arg = parsed.key_args[i];
    if (util::StartsWith(arg, "--vcache-") || arg == "-MP") continue;
    if (arg == "-MT" || arg == "-MQ") {
      ++i;
      continue;
    }
    if (util::StartsWith(arg, "-MT") || util::StartsWith(arg, "-MQ")) continue;
    cmd.push_back(arg);
  }
  cmd.insert(cmd.end(), {"-x", NativeProbeLanguage(parsed.language), "-E", "-v", "/dev/null"});

  hash::Hasher memo_key;
  memo_key.UpdateDelimited("system-include-dirs-memo-v2");
  memo_key.UpdateDelimited(compiler_id.fingerprint);
  for (const std::string& arg : cmd) memo_key.UpdateDelimited(arg);
  for (const char* name : {"GCC_EXEC_PREFIX", "COMPILER_PATH"}) {
    const char* value = std::getenv(name);
    memo_key.UpdateDelimited(value != nullptr ? std::string(name) + "=" + value : "unset");
  }
  const std::string memo_path = SystemIncludeMemoDir(cache_dir) + "/" + memo_key.Hex();
  if (auto cached = util::ReadFile(memo_path)) {
    return util::Split(*cached, '\n', /*skip_empty=*/true);
  }
  util::ProcOptions options{.capture_stdout = true, .capture_stderr = true};
  options.unset_env.assign(std::begin(kIncludePathEnvVars), std::end(kIncludePathEnvVars));
  util::ProcResult probe = util::Run(cmd, options);
  std::vector<std::string> dirs =
      probe.exit_code == 0 ? ParseSystemIncludeDirs(probe.stderr_data) : std::vector<std::string>{};
  if (dirs.empty()) {
    VCACHE_LOG("cxx manifest: could not list the compiler's include directories");
    return dirs;
  }
  util::WriteFileAtomic(memo_path, util::Join(dirs, "\n") + "\n");
  return dirs;
}

// The tracer to run the recording compile under, or empty, after logging why
// and that the state is to come from `fallback` instead.
std::string CxxManifestTracer(const Config& config, const std::string& fallback) {
#ifdef __linux__
  if (!config.cxx_manifest_trace) {
    VCACHE_LOG("cxx manifest: tracing is off; recording from " + fallback);
    return "";
  }
  const std::string tracer = TracerPath();
  if (tracer.empty() || ::access(tracer.c_str(), R_OK) != 0) {
    VCACHE_LOG("cxx manifest: no tracer at '" + tracer + "'; recording from " + fallback);
    return "";
  }
  if (!TracerLoadsHere(tracer)) {
    VCACHE_LOG("cxx manifest: '" + tracer + "' is not a shared object this binary's loader "
               "would preload; recording from " + fallback);
    return "";
  }
  // Replacing it would change what the compile runs with, and adding to it
  // would trace a library loaded before the tracer could see it.
  const char* inherited_preload = std::getenv("LD_PRELOAD");
  if (inherited_preload != nullptr && *inherited_preload != '\0') {
    VCACHE_LOG("cxx manifest: LD_PRELOAD is set; recording from " + fallback);
    return "";
  }
  return tracer;
#else
  (void)config;
  VCACHE_LOG("cxx manifest: no tracer on this platform; recording from " + fallback);
  return "";
#endif
}

// The basenames of the executables a compiler driver runs, besides itself.
constexpr const char* kCompilerToolNames[] = {
    "as", "ld", "collect2", "cc1", "cc1plus", "cc1obj", "cc1objplus", "lto1", "lto-wrapper",
    "gcc", "g++", "cc", "c++", "cpp", "clang", "clang++",
};

// What the trace of the compile adds to its depfile, or nullopt, after logging
// why, when the trace cannot be trusted to be whole: no process loaded the
// tracer (a static compiler, a hardened loader), the tracer said it lost a
// record, a record is malformed, or the compiler's include directories cannot
// be listed, which leaves nothing to tell its trees from what they hold.
std::optional<CxxTraceInputs> ReadCxxManifestTrace(const std::string& trace_log,
                                                   const std::string& tracer,
                                                   const std::string& scratch_dir,
                                                   const std::vector<std::string>& keyed_pchs,
                                                   const args::CompilerArgs& parsed,
                                                   const CompilerId& compiler_id,
                                                   const RootMap& roots,
                                                   const std::string& cwd,
                                                   const std::string& cache_dir) {
  if (!util::FileExists(trace_log)) {
    VCACHE_LOG("cxx manifest: no process loaded the tracer; recording from the depfile");
    return std::nullopt;
  }
  if (TraceMarkedIncomplete(trace_log)) {
    VCACHE_LOG("cxx manifest: the trace is incomplete; recording from the depfile");
    return std::nullopt;
  }
  TraceRecords records;
  if (!ReadTraceRecords(trace_log, cwd, &records)) {
    VCACHE_LOG("cxx manifest: the trace did not parse; recording from the depfile");
    return std::nullopt;
  }
  if (records.tools.empty()) {
    VCACHE_LOG("cxx manifest: the trace names no process; recording from the depfile");
    return std::nullopt;
  }
  std::vector<std::string> include_dirs = SystemIncludeDirs(parsed, compiler_id, cache_dir);
  if (include_dirs.empty()) {
    VCACHE_LOG("cxx manifest: the compiler's include directories are unknown; recording from "
               "the depfile");
    return std::nullopt;
  }
  for (std::string& dir : CxxIncludeDirs(parsed, cwd)) include_dirs.push_back(std::move(dir));
  CxxTraceFilter filter;
  filter.scratch_dir = scratch_dir;
  filter.tracer = tracer;
  filter.keyed_pchs = keyed_pchs;
  filter.compiler_files = records.tools;
  filter.compiler_files.insert(filter.compiler_files.end(), records.loaded.begin(),
                               records.loaded.end());
  filter.compiler_dirs = CompilerInstallDirs(records.tools);
  filter.include_dirs = std::move(include_dirs);
  filter.search_path_dirs = SearchPathDirs(EnvOr("PATH", ""));
  filter.search_path_names.assign(std::begin(kCompilerToolNames), std::end(kCompilerToolNames));
  filter.search_path_names.push_back(util::BaseName(parsed.compiler));
  for (const std::string& tool : records.tools) {
    filter.search_path_names.push_back(util::BaseName(tool));
  }
  return SelectCxxTraceInputs(records, filter);
}

// The version of the absent-deps patch whose -fabsent-deps list and depfile
// vcache knows the meaning of.
constexpr int kKnownAbsentDepsVersion = 1;

// True when clang is told to resolve relative paths against another
// directory than the one it runs in. The relative paths in its list would then
// be joined to the wrong directory.
bool SetsWorkingDirectory(const args::CompilerArgs& parsed) {
  for (const std::string& arg : parsed.argv) {
    for (const std::string_view flag : {"-working-directory", "--working-directory"}) {
      if (arg == flag || util::StartsWith(arg, std::string(flag) + "=")) return true;
    }
  }
  return false;
}

// True when a recording compile without the tracer can ask the compiler for
// its own list of the paths it probed and did not find. A patch version this
// vcache does not know may mean something else by its list.
bool CompilerWritesAbsentList(const Config& config, const CompilerId& compiler_id,
                              const args::CompilerArgs& parsed) {
  return compiler_id.absent_deps_version == kKnownAbsentDepsVersion &&
         config.cxx_manifest_absent_deps && !SetsWorkingDirectory(parsed);
}

// Why a compiler with the absent-deps patch is not asked for its list.
void LogAbsentListUnused(const Config& config, const CompilerId& compiler_id) {
  if (compiler_id.absent_deps_version == 0) return;
  if (compiler_id.absent_deps_version != kKnownAbsentDepsVersion) {
    VCACHE_LOG("cxx manifest: the compiler reports absent-deps-" +
               std::to_string(compiler_id.absent_deps_version) +
               ", a version this vcache does not read; not asking for its list");
  } else if (!config.cxx_manifest_absent_deps) {
    VCACHE_LOG("cxx manifest: absent-deps lists are off; not asking for the compiler's list");
  } else {
    VCACHE_LOG("cxx manifest: -working-directory is set; not asking for the compiler's list");
  }
}

// The driver as run and as resolved, for CompilerInstallDirs.
std::vector<std::string> CompilerExecutables(const std::string& compiler,
                                             const std::string& cwd) {
  const std::optional<std::string> driver =
      compiler.find('/') != std::string::npos
          ? std::optional<std::string>(CxxAbsolutePath(cwd, compiler))
          : util::FindInPath(compiler, "");
  std::vector<std::string> executables;
  if (!driver) return executables;
  executables.push_back(*driver);
  if (auto real = util::RealPath(*driver)) executables.push_back(*real);
  return executables;
}

// The paths the compiler's -fabsent-deps list names, with what the trace
// loses as noise dropped, or nullopt with `declined` saying why the state is
// not to be recorded. A listed path that exists is a fault in the compiler:
// the list cannot be trusted to be whole, and a state holding that path would
// never be served.
std::optional<CxxTraceInputs> ReadCxxAbsentList(const std::string& list_path,
                                                const std::string& scratch_dir,
                                                const std::vector<std::string>& keyed_pchs,
                                                const args::CompilerArgs& parsed,
                                                const CompilerId& compiler_id,
                                                const std::string& cwd,
                                                const std::string& cache_dir,
                                                std::string* declined) {
  const std::optional<std::string> text = util::ReadFile(list_path);
  if (!text) {
    *declined = "the compiler wrote no -fabsent-deps list";
    return std::nullopt;
  }
  CxxAbsentList list = ParseCxxAbsentList(*text, cwd);
  if (list.existing_path) {
    struct stat st;
    *declined = "the compiler listed " + *list.existing_path + " as absent but it " +
                (::lstat(list.existing_path->c_str(), &st) == 0 ? "exists"
                                                                : "cannot be shown absent");
    return std::nullopt;
  }
  CxxTraceFilter filter;
  filter.scratch_dir = scratch_dir;
  filter.keyed_pchs = keyed_pchs;
  // Without the include directories nothing tells the compiler's trees from
  // the include directories inside them, so nothing is dropped as its own.
  std::vector<std::string> include_dirs = SystemIncludeDirs(parsed, compiler_id, cache_dir);
  if (!include_dirs.empty()) {
    for (std::string& dir : CxxIncludeDirs(parsed, cwd)) include_dirs.push_back(std::move(dir));
    filter.compiler_dirs = CompilerInstallDirs(CompilerExecutables(parsed.compiler, cwd));
    filter.include_dirs = std::move(include_dirs);
  }
  TraceRecords records;
  records.absent = std::move(list.paths);
  return SelectCxxTraceInputs(records, filter);
}

// What the compile that just ran read, from the depfile it wrote and, with a
// `trace` that shows every file the depfile names being read, from what the
// tracer saw it open and fail to find, or, with an `absent_list`, from the
// compiler's own list of what it did not find. Declined, as
// RecordDepfileInputs and AddCxxTraceInputs decline, and when the source is
// not among the files. Without either, also declined under gcc when a .gch the
// key does not cover sits where an #include could have picked it up, or when a
// file outside the compiler's own include directories uses __has_include,
// since gcc's depfile names neither a .gch nor a header that was only probed.
// The trace sees both; the compiler's list sees the probes, and returns before
// these declines. A patched compiler's depfile also names a PCH the key
// identifies; that one is left out. Its naming a header __has_include found
// does not stand in for a probe that found nothing, so from the depfile alone
// the declines stay.
RecordedInputs ReadCxxManifestInputs(const std::string& depfile_path,
                                     const CxxTraceInputs* trace,
                                     const CxxTraceInputs* absent_list,
                                     const args::CompilerArgs& parsed, const RootMap& roots,
                                     const CompilerId& compiler_id, const std::string& cwd,
                                     const std::string& cache_dir, const std::string& memo_dir,
                                     const std::vector<std::string>& keyed_pchs,
                                     int64_t started_ns, bool scan_incbin) {
  RecordedInputs inputs;
  const std::optional<std::string> text = util::ReadFile(depfile_path);
  std::optional<DepFile> dep;
  if (text) dep = ParseDepFile(*text);
  if (!dep) {
    inputs.declined = "the compile wrote no readable dependency file";
    return inputs;
  }
  const auto normal = [](const std::string& path) {
    return std::filesystem::path(path).lexically_normal().string();
  };
  if (compiler_id.absent_deps_version == kKnownAbsentDepsVersion && !keyed_pchs.empty()) {
    // The key identifies a PCH in a way that holds across trees whose PCH
    // bytes differ; hashing the bytes as well would keep the state to one tree.
    std::vector<std::string> keyed;
    for (const std::string& pch : keyed_pchs) keyed.push_back(normal(pch));
    for (DepRule& rule : dep->rules) {
      std::erase_if(rule.prerequisites, [&](const std::string& prereq) {
        return std::find(keyed.begin(), keyed.end(), normal(CxxAbsolutePath(cwd, prereq))) !=
               keyed.end();
      });
    }
  }
  inputs = RecordDepfileInputs(*dep, cwd, roots, started_ns, scan_incbin,
                               /*find_probes=*/!compiler_id.is_clang);
  if (!inputs.declined.empty()) return inputs;
  // A state lists what it is checked against; one without the source would
  // match whatever is on disk. Compared lexically normalised, since both
  // compilers drop a leading ./ from the source they write.
  const std::string source = normal(CxxAbsolutePath(cwd, parsed.source));
  if (std::none_of(inputs.files.begin(), inputs.files.end(), [&](const ManifestFile& file) {
        return normal(roots.Localize(file.first)) == source;
      })) {
    inputs.files.clear();
    inputs.declined = "the dependency file does not name the source";
    return inputs;
  }
  if (absent_list != nullptr) {
    AddCxxTraceInputs(*absent_list, roots, cwd, started_ns, memo_dir, &inputs);
    if (inputs.declined.empty()) {
      VCACHE_LOG("cxx manifest: recorded from the compiler's absent list: " +
                 std::to_string(inputs.files.size()) + " files, " +
                 std::to_string(inputs.absent.size()) + " absent paths");
    }
    return inputs;
  }
  if (trace != nullptr) {
    if (auto untraced = FindUntracedFile(inputs.files, roots, *trace)) {
      VCACHE_LOG("cxx manifest: the trace does not show " + *untraced +
                 " being read; recording from the depfile");
    } else {
      AddCxxTraceInputs(*trace, roots, cwd, started_ns, memo_dir, &inputs);
      if (inputs.declined.empty()) {
        VCACHE_LOG("cxx manifest: recorded from the trace: " +
                   std::to_string(inputs.files.size()) + " files, " +
                   std::to_string(inputs.absent.size()) + " absent paths");
      }
      return inputs;
    }
  }
  if (compiler_id.is_clang) return inputs;

  if (!inputs.probing_files.empty()) {
    const std::vector<std::string> system_dirs =
        SystemIncludeDirs(parsed, compiler_id, cache_dir);
    for (const std::string& path : inputs.probing_files) {
      if (system_dirs.empty() || !UnderAnyDir(path, system_dirs)) {
        inputs.files.clear();
        inputs.declined = path + " uses __has_include, and gcc's depfile does not name " +
                          "a header that was only probed";
        return inputs;
      }
    }
  }

  std::vector<std::string> dirs = {cwd, util::DirName(CxxAbsolutePath(cwd, parsed.source))};
  for (std::string& dir : CxxIncludeDirs(parsed, cwd)) dirs.push_back(std::move(dir));
  for (const ManifestFile& file : inputs.files) {
    dirs.push_back(util::DirName(roots.Localize(file.first)));
  }
  if (auto gch = FindUnkeyedGch(dirs, keyed_pchs)) {
    inputs.files.clear();
    inputs.declined = "gcc could have used " + *gch + ", which the depfile does not name";
  }
  return inputs;
}

}  // namespace

bool PchCarriesTimestamps(const args::CompilerArgs& parsed, bool is_clang) {
  return parsed.produces_pch && is_clang && !parsed.pch_timestamp_disabled;
}

int RunCompile(const std::vector<std::string>& argv, const Config& config,
               const RootMap& roots, storage::CacheChain* cache) {
  const std::string& cache_dir = config.disk.dir;

  args::CompilerArgs parsed = args::Parse(argv);

  // Every path below, passthrough included, may replace the PCH, so the key
  // that described the old one goes first. Only a run that knows the new PCH's
  // key writes the file again.
  const std::string pch_key_file =
      parsed.produces_pch && !parsed.output.empty() && parsed.output != "-" &&
              parsed.output != "/dev/null"
          ? parsed.output + std::string(kPchKeyFileSuffix)
          : "";
  if (!pch_key_file.empty()) util::RemoveFile(pch_key_file);

  if (!parsed.cacheable()) {
    RecordDecision(cache_dir, *parsed.uncacheable);
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
        RecordDecision(cache_dir, Reason::kKeptPrefixMaps);
        return RunPassthrough(argv);
      case IncomingMapPolicy::kStrip:
        VCACHE_LOG("stripped incoming prefix maps: " +
                   util::Join(parsed.incoming_prefix_maps, " "));
        break;
    }
  }

  const std::string check_mode = EnvOr("VCACHE_COMPILER_CHECK", "version");
  const CompilerId compiler_id =
      ResolveCompilerId(parsed.compiler, check_mode, cache_dir, roots);

  if (PchCarriesTimestamps(parsed, compiler_id.is_clang)) {
    RecordDecision(cache_dir, {Reason::kPchWithTimestamps, parsed.output});
    return RunPassthrough(argv);
  }

  // -march=native and friends: resolve them to a concrete target fingerprint,
  // or decline to cache if the caller asked for that or the probe failed.
  std::string native_target;
  if (!parsed.native_flags.empty()) {
    if (config.native_target_policy == NativeTargetPolicy::kUncacheable) {
      RecordDecision(cache_dir,
                     {Reason::kNativeTargetPolicy, util::Join(parsed.native_flags, " ")});
      return RunPassthrough(argv);
    }
    native_target = ResolveNativeTarget(parsed, compiler_id, cache_dir);
    if (native_target.empty()) {
      RecordDecision(cache_dir,
                     {Reason::kNativeTargetUnresolved, util::Join(parsed.native_flags, " ")});
      return RunPassthrough(argv);
    }
    VCACHE_LOG("native target " + native_target.substr(0, 16) + " for " +
               util::Join(parsed.native_flags, " "));
  }

  auto temp_dir = util::MakeScratchDir(cache_dir, "vcache-");
  if (!temp_dir) {
    RecordDecision(cache_dir, Reason::kNoTempDir);
    return RunPassthrough(argv);
  }
  struct TempDirGuard {
    std::string path;
    ~TempDirGuard() { util::RemoveRecursive(path); }
  } guard{*temp_dir};

  // Taken before anything reads the sources: a file changed after this may not
  // be the version the compile read, so it is not recorded in a manifest.
  const int64_t started_ns = util::NowUnixNs();
  const std::string cwd = util::CurrentDir();
  const std::string memo_dir = hash::FileDigestMemoDir(cache_dir);

  // Tracks whether any layer was broken, as opposed to cold, across this whole
  // invocation. A hit does not clear it: the media still failed.
  bool media_failed = false;

  // Written after the PCH itself, so no reader pairs this key with the PCH it
  // replaced.
  const auto write_pch_key_file = [&](const std::string& identity) {
    if (pch_key_file.empty()) return;
    if (identity.empty()) {
      VCACHE_LOG("no identity for " + pch_key_file);
      return;
    }
    if (!util::WriteFileAtomic(pch_key_file, identity + "\n", /*durable=*/false)) {
      VCACHE_LOG("could not write " + pch_key_file);
    }
  };

  // Serves `result_key` when its entry exists and fits this compile.
  // `manifest_state` names the manifest state that chose the key; it is empty
  // when the preprocessed text did.
  const auto try_hit = [&](const std::string& result_key, const std::string& manifest_state) {
    if (cache == nullptr) return false;
    const auto lookup_started = std::chrono::steady_clock::now();
    storage::GetResult got = cache->Get(result_key);
    media_failed |= ReportCacheMediaErrors(got.errors, cache_dir);
    if (got.hit) {
      storage::Blob blob;
      const bool blob_decoded = storage::DeserializeBlob(got.value, &blob);
      // Dependency flags are not in the key, so an entry stored by a compile
      // without -MD serves this one too, and has no depfile to replay. The
      // recompile stores one, and the entry then serves both shapes.
      if (blob_decoded && parsed.generates_deps && !parsed.depfile.empty() &&
          !blob.has_depfile) {
        VCACHE_LOG("hit on " + got.layer + " has no dependency file for " + parsed.depfile +
                   "; recompiling");
        return false;
      }
      if (blob_decoded && MaterializeHit(blob, parsed, roots, lookup_started)) {
        write_pch_key_file(PchIdentityFromMeta(blob.meta));
        const Counter layer = got.layer == "s3" ? Counter::kHitS3 : Counter::kHitDisk;
        if (manifest_state.empty()) {
          VCACHE_LOG("hit on " + got.layer);
          RecordCounter(cache_dir, layer);
        } else {
          VCACHE_LOG("cxx manifest hit on " + got.layer + ": " + manifest_state +
                     "; preprocessor skipped");
          RecordCounters(cache_dir, layer, Counter::kManifestHit);
        }
        return true;
      }
      VCACHE_LOG("hit on " + got.layer + " but entry was unusable; recompiling");
    }
    return false;
  };

  // ---- manifest: answer without preprocessing -----------------------------

  std::string manifest_key;
  std::vector<std::string> keyed_pchs;
  std::vector<CxxManifestState> manifest_states;
  if (config.cxx_manifest && cache != nullptr) {
    manifest_key = ComputeCxxManifestKey(parsed, roots, compiler_id, config, native_target,
                                         cwd, memo_dir, &keyed_pchs);
    if (!manifest_key.empty()) {
      VCACHE_LOG("cxx manifest key " + manifest_key + " for " + parsed.source);
      manifest_states =
          LoadCxxManifest(cache, manifest_key, cache_dir, &media_failed, true, nullptr);
    }
  }

  // A result key a matching state named that did not serve. The preprocessor
  // usually arrives at the same key, and asking again would only repeat why.
  std::string manifest_unserved_key;
  const auto try_manifest_hit = [&]() {
    for (size_t i = 0; i < manifest_states.size(); ++i) {
      const std::string which = manifest_key.substr(0, 16) + " state " + std::to_string(i + 1) +
                                " of " + std::to_string(manifest_states.size());
      if (auto stale = FindStaleManifestFile(manifest_states[i].files, roots, memo_dir)) {
        VCACHE_LOG("cxx manifest: " + which + " rejected: " + *stale);
        continue;
      }
      if (auto appeared = FindAppearedPath(manifest_states[i].absent, roots)) {
        VCACHE_LOG("cxx manifest: " + which + " rejected: " + *appeared);
        continue;
      }
      if (try_hit(manifest_states[i].result_key, which)) {
        if (i > 0 && !config.read_only) {
          StoreCxxManifest(cache, manifest_key, manifest_states[i], manifest_states, cache_dir,
                           &media_failed);
        }
        return true;
      }
      manifest_unserved_key = manifest_states[i].result_key;
      VCACHE_LOG("cxx manifest: " + which + " matched but its entry did not serve");
    }
    return false;
  };
  if (!config.recache && try_manifest_hit()) {
    return media_failed && config.error_on_cache_media_failure ? kCacheMediaFailureExit : 0;
  }

  // ---- preprocess ---------------------------------------------------------

  const std::string preprocessed = *temp_dir + "/pp.i";
  const std::vector<std::string> pp_cmd =
      BuildPreprocessCommand(parsed, roots, compiler_id);
  VCACHE_LOG("preprocess: " + util::Join(pp_cmd, " "));

  util::ProcResult pp = util::Run(
      pp_cmd, {.capture_stdout = true, .capture_stderr = true, .stdout_file = preprocessed});
  // A compile the preprocessor cannot handle can still be cached from what the
  // compile itself reads: a PCH consumer whose command lacks an include path
  // the PCH's header text needs compiles, and fails only under -E. Only an
  // error the compiler reported (status 1) qualifies; a tool that could not
  // run or crashed gets the original command line, as before.
  const bool derive_key = pp.exit_code == 1 && !manifest_key.empty() && !config.read_only;
  const std::string pp_failure =
      pp.exit_code != 0
          ? "exit code " + std::to_string(pp.exit_code) + ": " + pp.stderr_data.substr(0, 512)
          : "";
  std::string key;
  std::string pch_identity;
  if (pp.exit_code != 0 && !derive_key) {
    // The compilation itself will fail too; let the compiler produce the real
    // diagnostics rather than reporting a vcache-shaped error.
    RecordDecision(cache_dir, {Reason::kPreprocessorError, pp_failure});
    return RunPassthrough(argv);
  }
  if (derive_key) {
    VCACHE_LOG("cxx manifest: preprocessing failed; deriving the key from what the compile "
               "reads: " + pp_failure);
  } else {
    bool saw_incbin = false;
    key = ComputeKey(parsed, roots, compiler_id, config, native_target, preprocessed,
                     &saw_incbin, &pch_identity);
    if (key.empty()) {
      RecordDecision(cache_dir, Reason::kNoCacheKey);
      return RunPassthrough(argv);
    }
    // The preprocessed text is the whole of what vcache knows about this
    // compilation, and .incbin means the assembler will read a file that text
    // never mentions. Two builds can preprocess identically and still owe
    // different objects, so there is no key that would be honest here.
    if (saw_incbin) {
      RecordDecision(cache_dir, Reason::kIncbin);
      return RunPassthrough(argv);
    }
    VCACHE_LOG("key " + key + " for " + parsed.source + " -> " + parsed.output);
  }

  // ---- lookup -------------------------------------------------------------

  if (!derive_key && !config.recache && key != manifest_unserved_key && try_hit(key, "")) {
    return media_failed && config.error_on_cache_media_failure ? kCacheMediaFailureExit : 0;
  }

  // ---- miss: compile for real ---------------------------------------------

  auto session = daemon::DaemonClient::OpenCompileSession(config);
  if (session && config.daemon.single_flight && !config.recache && cache != nullptr) {
    const auto cost_key = ComputeCostKey("compile", parsed.source,
        args::LanguageName(parsed.language), parsed.key_args, roots);
    // Without a result key yet, peers wait on the manifest key and look in the
    // manifest once the holder has recorded its state there.
    const std::string& lease_key = derive_key ? manifest_key : key;
    const auto outcome =
        session->AcquireLease(lease_key, daemon::LeaseWaitBoundMs(cache_dir, cost_key));
    if (outcome != daemon::LeaseOutcome::kCompile) {
      bool served = false;
      if (derive_key) {
        manifest_states =
            LoadCxxManifest(cache, manifest_key, cache_dir, &media_failed, false, nullptr);
        served = try_manifest_hit();
      } else {
        served = try_hit(key, "");
      }
      if (served) {
        session->ReleaseLease(true);
        return media_failed && config.error_on_cache_media_failure ? kCacheMediaFailureExit : 0;
      }
    }
  }
  // Counted once the compile succeeds when the key is derived: until then this
  // is the preprocessor failure it was before the manifest existed.
  if (!derive_key) RecordCounter(cache_dir, Counter::kMiss);

  // Compile to a temporary and move into place only on success, so a failed
  // build never leaves a half-written object where make expects a good one.
  const std::string tmp_output = *temp_dir + "/out" ;
  const std::string tmp_depfile =
      parsed.generates_deps && !parsed.depfile.empty() ? *temp_dir + "/out.d" : "";
  // The depfile a manifest state is read from: the caller's -MD output, or one
  // vcache asks for when the caller asked for none.
  const bool add_md = !manifest_key.empty() && !parsed.generates_deps;
  std::string manifest_depfile;
  if (add_md) {
    manifest_depfile = *temp_dir + "/manifest.d";
  } else if (!manifest_key.empty() && UsesDependencyFlag(parsed, "-MD")) {
    manifest_depfile = tmp_depfile;
  }

  const bool records_manifest = !manifest_depfile.empty() && !config.read_only;
  // The trace comes first: it also sees what the driver reads and probes on
  // its own, which the compiler's list does not name. The list stands in for
  // it where there is no tracer. The -E run stays untraced either way: only
  // what the real compile opens and probes goes into a state.
  const bool list_available = CompilerWritesAbsentList(config, compiler_id, parsed);
  const std::string manifest_tracer =
      records_manifest
          ? CxxManifestTracer(config, list_available ? "the compiler's absent list"
                                                     : "the depfile")
          : "";
  if (records_manifest && manifest_tracer.empty() && !list_available) {
    LogAbsentListUnused(config, compiler_id);
  }
  // Like -MD, the list is vcache's own addition, in neither key.
  const std::string absent_list_path =
      records_manifest && manifest_tracer.empty() && list_available ? *temp_dir + "/absent"
                                                                     : "";
  const std::vector<std::string> compile_cmd =
      BuildCompileCommand(parsed, roots, tmp_output, add_md ? manifest_depfile : tmp_depfile,
                          add_md, absent_list_path);
  VCACHE_LOG("compile: " + util::Join(compile_cmd, " "));

  util::ProcOptions compile_options{.capture_stderr = true};
  const std::string trace_log = manifest_tracer.empty() ? "" : *temp_dir + "/trace";
  if (!manifest_tracer.empty()) {
    compile_options.env.emplace_back("VCACHE_TRACE_LOG", trace_log);
    compile_options.env.emplace_back("LD_PRELOAD", manifest_tracer);
  }
  if (session && config.daemon.admission) {
    const auto cost_key = ComputeCostKey("compile", parsed.source,
        args::LanguageName(parsed.language), parsed.key_args, roots);
    session->ReserveMemory(cost_key, daemon::MemoryEstimateKb(config, cost_key, false));
    compile_options.on_spawn = [&](int pid) { session->CompilerSpawned(pid); };
  } else if (config.daemon.admission && !config.read_only &&
             config.daemon.mode != DaemonMode::kOff) {
    VCACHE_LOG("reserve: daemon unavailable, running unreserved");
  }
  util::ProcResult compiled = util::Run(compile_cmd, compile_options);
  // Recorded on a failed compile too: an OOM kill is the sample that matters
  // most, and the cost file is not the cache entry.
  RecordCompileCost(cache_dir, "compile", parsed.source,
                    args::LanguageName(parsed.language), parsed.key_args, roots, compiled);

  // ld.so's refusal to preload the tracer into some process of the compile is
  // the tracer's doing, not the compiler's: neither printed nor stored, and
  // the trace is not used, since that process recorded nothing.
  bool preload_refused = false;
  if (!trace_log.empty()) {
    compiled.stderr_data =
        WithoutPreloadRefusals(compiled.stderr_data, manifest_tracer, &preload_refused);
  }
  if (!compiled.stderr_data.empty()) {
    ::fwrite(compiled.stderr_data.data(), 1, compiled.stderr_data.size(), stderr);
  }
  if (compiled.exit_code != 0) {
    // gcc emits the dependency file during preprocessing, so a compilation
    // that fails afterwards still leaves one behind, and builds rely on that.
    // The kernel's lib/test_fortify targets compile code that is *meant* not
    // to build, and kbuild still hands the .d to fixdep when it does not:
    // without this the build stops with "fixdep: error opening file".
    // No object is placed, exactly as the compiler leaves none.
    if (!tmp_depfile.empty()) {
      if (auto text = util::ReadFile(tmp_depfile)) {
        util::WriteFileAtomic(parsed.depfile, *text, /*durable=*/false);
      }
    }
    VCACHE_LOG("compile failed with exit code " + std::to_string(compiled.exit_code));
    if (derive_key) {
      RecordDecision(cache_dir, {Reason::kPreprocessorError, pp_failure});
    } else {
      RecordCounter(cache_dir, Counter::kCompileFailed);
    }
    return compiled.exit_code;
  }

  if (!util::LinkOrCopy(tmp_output, parsed.output)) {
    RecordDecision(cache_dir, {Reason::kOutputUnplaceable, parsed.output});
    return RunPassthrough(argv);
  }

  std::string canonical_depfile;
  bool have_depfile = false;
  if (!tmp_depfile.empty()) {
    auto text = util::ReadFile(tmp_depfile);
    if (text) {
      if (!util::WriteFileAtomic(parsed.depfile, *text, /*durable=*/false)) {
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

  // ---- what the compile read, for the manifest ----------------------------

  RecordedInputs manifest_inputs;
  if (!manifest_key.empty()) {
    if (manifest_depfile.empty()) {
      manifest_inputs.declined = "the compile writes no -MD dependency file";
    } else {
      if (preload_refused) {
        VCACHE_LOG("cxx manifest: ld.so refused the tracer in a process of the compile; "
                   "recording from the depfile");
      }
      const std::optional<CxxTraceInputs> traced =
          trace_log.empty() || preload_refused
              ? std::nullopt
              : ReadCxxManifestTrace(trace_log, manifest_tracer, *temp_dir, keyed_pchs, parsed,
                                     compiler_id, roots, cwd, cache_dir);
      const std::optional<CxxTraceInputs> absent_from_list =
          absent_list_path.empty()
              ? std::nullopt
              : ReadCxxAbsentList(absent_list_path, *temp_dir, keyed_pchs, parsed, compiler_id,
                                  cwd, cache_dir, &manifest_inputs.declined);
      if (manifest_inputs.declined.empty()) {
        manifest_inputs = ReadCxxManifestInputs(
            manifest_depfile, traced ? &*traced : nullptr,
            absent_from_list ? &*absent_from_list : nullptr, parsed, roots, compiler_id, cwd,
            cache_dir, memo_dir, keyed_pchs, started_ns, /*scan_incbin=*/derive_key);
      }
    }
    if (!manifest_inputs.declined.empty()) {
      VCACHE_LOG("cxx manifest: not recording: " + manifest_inputs.declined);
    }
  }
  if (derive_key) {
    if (!manifest_inputs.declined.empty()) {
      RecordDecision(cache_dir, {Reason::kPreprocessorError, pp_failure});
      return compiled.exit_code;
    }
    RecordCounter(cache_dir, Counter::kMiss);
    key = DeriveCxxResultKey(manifest_key, manifest_inputs.files);
    VCACHE_LOG("derived key " + key + " for " + parsed.source + " -> " + parsed.output);
  }
  write_pch_key_file(pch_identity);

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
  if (!pch_identity.empty()) {
    blob.meta += std::string(kPchIdentityMetaKey) + pch_identity + "\n";
  }
  AppendCostMeta(&blob.meta, compiled.max_rss_kb, compiled.wall_ms);

  const storage::PutResult put = cache->Put(key, storage::SerializeBlob(blob));
  media_failed |= ReportCacheMediaErrors(put.errors, cache_dir);
  if (put.stored) {
    RecordCounter(cache_dir, Counter::kStored);
    // After the entry, so a state never names a result that is not there yet,
    // and before the lease goes, so a peer woken on the manifest key finds it.
    if (!manifest_key.empty() && manifest_inputs.declined.empty()) {
      StoreCxxManifest(cache, manifest_key,
                       {key, std::move(manifest_inputs.files), std::move(manifest_inputs.absent)},
                       manifest_states, cache_dir, &media_failed);
    }
  } else {
    RecordCounter(cache_dir, Counter::kStoreFailed);
  }
  if (session) session->ReleaseLease(put.stored);
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
