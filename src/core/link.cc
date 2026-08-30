// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "core/link.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "args/link_args.h"
#include "core/compile.h"
#include "core/link_trace.h"
#include "core/stats.h"
#include "hash/hasher.h"
#include "storage/storage.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/str.h"
#include "util/subprocess.h"

namespace vcache::core {
namespace {

// Bumped independently of the compile key: the two never share entries.
constexpr std::string_view kLinkKeyVersion = "vcache-link-v1";
constexpr std::string_view kLinkManifestHeader = "vcache-linkmanifest-1";

// How many remembered input-states one manifest holds, matching the dependency
// scan's reasoning: alternating between two branches should keep hitting rather
// than each run overwriting the state the other just stored.
constexpr size_t kMaxLinkManifestEntries = 8;

// Environment variables that change what a link produces. LD_LIBRARY_PATH is
// deliberately absent: it affects running a binary, not producing one.
constexpr const char* kLinkEnvVars[] = {
    "LIBRARY_PATH", "COMPILER_PATH", "GCC_EXEC_PREFIX",
    "LD_RUN_PATH",  "SOURCE_DATE_EPOCH",
};

std::string EnvOrEmpty(const char* name) {
  const char* v = std::getenv(name);
  return v != nullptr ? std::string(v) : std::string();
}

struct LinkManifestEntry {
  std::string result_key;
  std::vector<std::pair<std::string, std::string>> inputs;  // canonical, digest
  std::vector<std::string> absent;                          // canonical
};

// "entry <key>", then "I <digest> <path>" / "A <path>" lines. The digest comes
// first on an input line so a path containing spaces still parses.
std::string RenderLinkManifest(const std::vector<LinkManifestEntry>& entries) {
  std::string out(kLinkManifestHeader);
  out.push_back('\n');
  for (const LinkManifestEntry& e : entries) {
    out += "entry " + e.result_key + "\n";
    for (const auto& [path, digest] : e.inputs) {
      out += "I " + digest + " " + path + "\n";
    }
    for (const std::string& path : e.absent) out += "A " + path + "\n";
  }
  return out;
}

bool ParseLinkManifest(const std::string& text,
                       std::vector<LinkManifestEntry>* entries) {
  const auto lines = util::Split(text, '\n', /*skip_empty=*/true);
  if (lines.empty() || lines.front() != kLinkManifestHeader) return false;
  for (size_t i = 1; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    if (util::StartsWith(line, "entry ")) {
      LinkManifestEntry e;
      e.result_key = line.substr(6);
      if (e.result_key.size() != hash::kDigestHexLen) return false;
      entries->push_back(std::move(e));
      continue;
    }
    if (entries->empty()) return false;
    if (util::StartsWith(line, "I ")) {
      if (line.size() < 2 + hash::kDigestHexLen + 2) return false;
      const std::string digest = line.substr(2, hash::kDigestHexLen);
      if (line[2 + hash::kDigestHexLen] != ' ') return false;
      entries->back().inputs.emplace_back(
          line.substr(3 + hash::kDigestHexLen), digest);
      continue;
    }
    if (util::StartsWith(line, "A ")) {
      entries->back().absent.push_back(line.substr(2));
      continue;
    }
    return false;
  }
  return true;
}

// Both halves of the correctness condition. Every recorded input must still
// hash the same, and every recorded absent path must still be absent.
bool LinkManifestStillHolds(const LinkManifestEntry& e, const RootMap& roots) {
  for (const auto& [canonical, digest] : e.inputs) {
    const std::string local = roots.Localize(canonical);
    auto actual = hash::HashFile(local);
    if (!actual) {
      VCACHE_LOG("link manifest: input is gone: " + local);
      return false;
    }
    if (*actual != digest) {
      VCACHE_LOG("link manifest: input changed: " + local);
      return false;
    }
  }
  for (const std::string& canonical : e.absent) {
    const std::string local = roots.Localize(canonical);
    struct stat st;
    if (::lstat(local.c_str(), &st) == 0) {
      // Something now exists where the recorded link found nothing. The search
      // that produced this entry would resolve differently today.
      VCACHE_LOG("link manifest: a path that was absent now exists: " + local);
      return false;
    }
    if (errno != ENOENT) {
      // EACCES, EIO and friends do not prove absence. The original trace only
      // records ENOENT, so require that same answer when validating a hit.
      VCACHE_LOG("link manifest: cannot prove path is still absent: " + local);
      return false;
    }
  }
  return true;
}

// The part of the key that can be computed without running anything.
std::string ComputePreKey(const args::LinkArgs& parsed, const RootMap& roots,
                          const CompilerId& driver_id) {
  hash::Hasher h;
  h.UpdateDelimited(kLinkKeyVersion);
  h.UpdateDelimited(driver_id.fingerprint);
  h.UpdateDelimited(roots.Fingerprint());
  for (const std::string& a : parsed.key_args) {
    // CanonicalizeText, not Canonicalize: the latter rewrites a path only when
    // the whole string is one, so "-L/checkout/lib" and "-Wl,-rpath,/checkout"
    // pass through untouched and make the key differ per checkout. A link
    // command line is mostly paths embedded in flags, so prefix matching leaves
    // almost all of them local -- which is why two checkouts of one revision
    // derived different keys and shared nothing.
    h.UpdateDelimited(roots.CanonicalizeText(a));
  }
  // Inputs named on the command line are hashed by content here, so a changed
  // object does not even reach the manifest lookup.
  for (const std::string& in : parsed.inputs) {
    auto digest = hash::HashFile(in);
    h.UpdateDelimited(roots.CanonicalizeText(in));
    h.UpdateDelimited(digest ? *digest : std::string("<missing>"));
  }
  for (const char* name : kLinkEnvVars) {
    h.UpdateDelimited(name);
    h.UpdateDelimited(EnvOrEmpty(name));
  }

  // Per-group digests, so a key that differs between two checkouts can be
  // narrowed to the group responsible by diffing two logs, rather than by
  // bisecting the whole command line.
  if (util::LoggingEnabled()) {
    hash::Hasher ha, hi;
    for (const std::string& a : parsed.key_args) {
      ha.UpdateDelimited(roots.CanonicalizeText(a));
    }
    for (const std::string& in : parsed.inputs) {
      auto d = hash::HashFile(in);
      hi.UpdateDelimited(roots.CanonicalizeText(in));
      hi.UpdateDelimited(d ? *d : std::string("<missing>"));
    }
    VCACHE_LOG("link key groups: driver=" + driver_id.fingerprint.substr(0, 16) +
               " roots=" + roots.Fingerprint() +
               " args=" + ha.Hex().substr(0, 16) +
               " inputs=" + hi.Hex().substr(0, 16));
  }
  return h.Hex();
}

std::string TracerPath() {
  const char* override_path = std::getenv("VCACHE_TRACER");
  if (override_path != nullptr && *override_path != '\0') return override_path;
  auto self = util::RealPath("/proc/self/exe");
  if (!self) return "";
  return util::DirName(*self) + "/vcache-fstrace.so";
}

// Every output the link is expected to write.
// Where a link output is kept, content-addressed by its digest.
//
// Deliberately inside the disk cache's own shard layout: Trim, Clear and
// TotalSize all walk those shards, so these files are evicted, cleared and
// counted like any other entry without a second accounting path. The coupling
// to that layout is the price; the alternative is a parallel store that the
// budget does not know about.
std::string LinkOutputPath(const std::string& cache_dir,
                           const std::string& digest) {
  return cache_dir + "/" + digest.substr(0, 2) + "/" + digest + ".linkout";
}

std::vector<std::string> AllOutputs(const args::LinkArgs& parsed) {
  std::vector<std::string> out{parsed.output};
  out.insert(out.end(), parsed.extra_outputs.begin(), parsed.extra_outputs.end());
  return out;
}

bool MaterializeOutputs(const storage::Blob& blob,
                        const std::vector<std::string>& outputs,
                        const std::string& cache_dir) {
  std::vector<std::string> expected = outputs;
  std::vector<std::string> stored_names;
  stored_names.reserve(blob.files.size());
  for (const storage::BlobFile& f : blob.files) stored_names.push_back(f.name);
  std::sort(expected.begin(), expected.end());
  std::sort(stored_names.begin(), stored_names.end());
  if (expected != stored_names) {
    VCACHE_LOG("link hit: cached output set does not match this invocation");
    return false;
  }

  for (const storage::BlobFile& f : blob.files) {
    const auto it = std::find(outputs.begin(), outputs.end(), f.name);
    if (it == outputs.end()) {
      VCACHE_LOG("link hit: entry names an output this run did not ask for: " +
                 f.name);
      return false;
    }
    // `contents` carries the digest, not the bytes. Link outputs are far too
    // large to move through a blob: a 2.2 GB shared library took 37 s to replay
    // that way against 51 s to link outright, and peaked at more memory than
    // the linker used. Cloning the stored file is a reflink where the
    // filesystem allows one.
    const bool valid_digest =
        f.contents.size() == hash::kDigestHexLen &&
        std::all_of(f.contents.begin(), f.contents.end(), [](unsigned char c) {
          return std::isxdigit(c) != 0;
        });
    if (!valid_digest) {
      VCACHE_LOG("link hit: cached output has an invalid content digest");
      return false;
    }
    const std::string stored = LinkOutputPath(cache_dir, f.contents);
    const auto actual_digest = hash::HashFile(stored);
    if (!actual_digest || *actual_digest != f.contents) {
      VCACHE_LOG("link hit: stored output failed digest verification: " + stored);
      return false;
    }
    if (!util::CloneFile(stored, f.name)) {
      VCACHE_LOG("link hit: stored output is gone or unreadable: " + stored);
      return false;
    }
    if (f.executable) {
      // Add execute where the umask would have allowed it, not everywhere.
      // WriteFileAtomic has already applied 0666 & ~umask, so under a strict
      // umask the file is 0600; adding all three execute bits unconditionally
      // would publish 0711 and let other users run a binary they cannot read.
      struct stat st;
      if (::stat(f.name.c_str(), &st) != 0) return false;
      const mode_t allowed = util::DefaultFileMode();
      mode_t add = 0;
      if (allowed & S_IRUSR) add |= S_IXUSR;
      if (allowed & S_IRGRP) add |= S_IXGRP;
      if (allowed & S_IROTH) add |= S_IXOTH;
      if (::chmod(f.name.c_str(), st.st_mode | add) != 0) return false;
    }
  }
  return true;
}

}  // namespace

int RunLink(const std::vector<std::string>& argv, const Config& config,
            const RootMap& roots, storage::CacheChain* cache, bool* handled) {
  *handled = false;
  const std::string& cache_dir = config.disk.dir;

  args::LinkArgs parsed = args::ParseLink(argv);
  if (!parsed.is_link) return 0;
  *handled = true;

  if (!parsed.uncacheable.empty()) {
    VCACHE_LOG("uncacheable link: " + parsed.uncacheable);
    RecordCounter(cache_dir, Counter::kUncacheable);
    return RunPassthrough(argv);
  }

  const std::string tracer = TracerPath();
  if (tracer.empty() || ::access(tracer.c_str(), R_OK) != 0) {
    // Without the tracer there is no absent set, and without an absent set a
    // hit cannot be shown to be sound. Decline rather than cache on a guess.
    VCACHE_LOG("uncacheable link: tracer not found at " + tracer);
    RecordCounter(cache_dir, Counter::kUncacheable);
    return RunPassthrough(argv);
  }
  if (!config.disk.enabled) {
    // Link outputs are content-addressed sidecars in the disk cache. A remote
    // blob alone only contains their digests and cannot materialise a hit.
    VCACHE_LOG("uncacheable link: link caching requires the disk cache");
    RecordCounter(cache_dir, Counter::kUncacheable);
    return RunPassthrough(argv);
  }
  const char* inherited_preload = std::getenv("LD_PRELOAD");
  if (inherited_preload != nullptr && *inherited_preload != '\0') {
    // A preloaded library can change linker behaviour, and it is loaded before
    // this tracer can observe it. Decline rather than key only its pathname and
    // miss an in-place content change.
    VCACHE_LOG("uncacheable link: an existing LD_PRELOAD is active");
    RecordCounter(cache_dir, Counter::kUncacheable);
    return RunPassthrough(argv);
  }

  const std::string check_mode = EnvOr("VCACHE_COMPILER_CHECK", "version");
  const CompilerId driver_id =
      ResolveCompilerId(parsed.driver, check_mode, cache_dir, roots);
  const std::string pre_key = ComputePreKey(parsed, roots, driver_id);
  const std::vector<std::string> outputs = AllOutputs(parsed);

  VCACHE_LOG("link pre-key " + pre_key + " for " + parsed.output);

  bool media_failed = false;
  std::vector<LinkManifestEntry> entries;
  if (cache != nullptr) {
    storage::GetResult got = cache->Get(pre_key);
    media_failed |= ReportCacheMediaErrors(got.errors, cache_dir);
    storage::Blob manifest_blob;
    if (got.hit && storage::DeserializeBlob(got.value, &manifest_blob) &&
        manifest_blob.has_dep_manifest &&
        !ParseLinkManifest(manifest_blob.dep_manifest, &entries)) {
      VCACHE_LOG("link: manifest did not parse; starting a new one");
      entries.clear();
    }
  }

  if (!config.recache && cache != nullptr) {
    for (const LinkManifestEntry& e : entries) {
      if (!LinkManifestStillHolds(e, roots)) continue;
      storage::GetResult result = cache->Get(e.result_key);
      media_failed |= ReportCacheMediaErrors(result.errors, cache_dir);
      storage::Blob blob;
      if (!result.hit || !storage::DeserializeBlob(result.value, &blob)) {
        VCACHE_LOG("link: manifest entry matched but its result is gone");
        continue;
      }
      if (!MaterializeOutputs(blob, outputs, cache_dir)) break;
      if (!blob.stderr_text.empty()) {
        ::fputs(roots.LocalizeText(blob.stderr_text).c_str(), stderr);
      }
      VCACHE_LOG("link hit on " + result.layer);
      RecordCounter(cache_dir, result.layer == "s3" ? Counter::kHitS3
                                                    : Counter::kHitDisk);
      return media_failed && config.error_on_cache_media_failure
                 ? kCacheMediaFailureExit
                 : 0;
    }
  }
  RecordCounter(cache_dir, Counter::kMiss);

  // ---- miss: run the link under the tracer --------------------------------

  auto temp_dir = util::MakeTempDir("vcache-link-");
  if (!temp_dir) return RunPassthrough(argv);
  struct TempDirGuard {
    std::string path;
    ~TempDirGuard() { util::RemoveRecursive(path); }
  } guard{*temp_dir};
  const std::string trace_log = *temp_dir + "/trace";

  util::ProcOptions opts;
  opts.capture_stderr = true;
  opts.env.emplace_back("VCACHE_TRACE_LOG", trace_log);
  opts.env.emplace_back("LD_PRELOAD", tracer);
  util::ProcResult result = util::Run(argv, opts);

  if (!result.stderr_data.empty()) {
    ::fputs(result.stderr_data.c_str(), stderr);
  }
  if (result.exit_code != 0) {
    RecordCounter(cache_dir, Counter::kCompileFailed);
    return result.exit_code < 0 ? 127 : result.exit_code;
  }

  // The tracer creates this when it could not record something: a write that
  // failed, or a record too long to fit in one atomic write. Either way the
  // input set is missing an entry, and an entry with a missing input is one
  // that can hit when it should not. Storing it would be worse than not
  // caching this link at all.
  struct stat incomplete_st;
  if (::stat((trace_log + ".incomplete").c_str(), &incomplete_st) == 0) {
    VCACHE_LOG("link: the trace is incomplete; not storing");
    RecordCounter(cache_dir, Counter::kUncacheable);
    return 0;
  }

  LinkTrace trace;
  if (!ParseTraceLog(trace_log, outputs, util::CurrentDir(), &trace)) {
    VCACHE_LOG("link: no usable trace; not storing");
    return 0;
  }
  if (trace.tools.empty()) {
    // The tracer produced nothing about the process tree, which means it was
    // not loaded -- a statically linked linker, or a hardened loader. Storing
    // an entry whose input set may be partial would be worse than not caching.
    VCACHE_LOG("link: tracer saw no processes; not storing");
    RecordCounter(cache_dir, Counter::kUncacheable);
    return 0;
  }

  if (config.read_only) {
    // The link has already run, but read-only means no sidecars or entries may
    // be written. Staging a .linkout directly would bypass Storage::writable().
    return 0;
  }

  LinkManifestEntry fresh;
  hash::Hasher result_hasher;
  result_hasher.UpdateDelimited(pre_key);
  result_hasher.UpdateDelimited("tools");
  for (const std::string& tool : trace.tools) {
    auto digest = hash::HashFile(tool);
    if (!digest) continue;
    fresh.inputs.emplace_back(roots.Canonicalize(tool), *digest);
    result_hasher.UpdateDelimited(roots.Canonicalize(tool));
    result_hasher.UpdateDelimited(*digest);
  }
  // Files named on the command line are already hashed into the pre-key, so a
  // pre-key match has proved they are unchanged. Recording them again would
  // make every hit hash them a second time, which on a large link is not a
  // rounding error: libLLVM.so takes 288 inputs totalling 13 GB, and hashing
  // that set twice was half the cost of the hit.
  std::vector<std::string> named;
  named.reserve(parsed.inputs.size());
  for (const std::string& in : parsed.inputs) {
    auto real = util::RealPath(in);
    named.push_back(roots.Canonicalize(real ? *real : in));
  }
  std::sort(named.begin(), named.end());

  result_hasher.UpdateDelimited("inputs");
  for (const std::string& in : trace.inputs) {
    if (std::binary_search(named.begin(), named.end(), roots.Canonicalize(in))) {
      continue;
    }
    auto digest = hash::HashFile(in);
    // A file that is gone by now was an intermediate the toolchain created and
    // removed -- collect2's constructor table, the LTO resolution file. It
    // cannot be hashed, and recording it would make the entry unhittable,
    // because the next run's intermediate has a different random name. This is
    // what keeps temporary files out of the manifest without a path rule that
    // would also exclude real inputs.
    if (!digest) continue;
    fresh.inputs.emplace_back(roots.Canonicalize(in), *digest);
    result_hasher.UpdateDelimited(roots.Canonicalize(in));
    result_hasher.UpdateDelimited(*digest);
  }
  result_hasher.UpdateDelimited("absent");
  for (const std::string& a : trace.absent) {
    fresh.absent.push_back(roots.Canonicalize(a));
    result_hasher.UpdateDelimited(roots.Canonicalize(a));
  }
  fresh.result_key = result_hasher.Hex();

  storage::Blob out_blob;
  out_blob.stderr_text = roots.CanonicalizeText(result.stderr_data);
  for (const std::string& path : outputs) {
    auto digest = hash::HashFile(path);
    if (!digest) {
      VCACHE_LOG("link: expected output was not produced: " + path);
      return 0;
    }
    const std::string stored = LinkOutputPath(cache_dir, *digest);
    if (!util::MakeDirs(util::DirName(stored))) return 0;
    // Content-addressed, so an output identical to one already stored costs
    // nothing to store again -- and the clone is a reflink where possible, so
    // it costs nothing in space either.
    if (!util::CloneFile(path, stored)) {
      VCACHE_LOG("link: could not stage output into the cache: " + path);
      return 0;
    }
    struct stat st;
    const bool executable =
        ::stat(path.c_str(), &st) == 0 && (st.st_mode & S_IXUSR) != 0;
    out_blob.files.push_back({path, *digest, executable});
  }

  if (cache != nullptr) {
    storage::PutResult put = cache->Put(fresh.result_key,
                                        storage::SerializeBlob(out_blob));
    media_failed |= ReportCacheMediaErrors(put.errors, cache_dir);
    if (put.stored) {
      std::vector<LinkManifestEntry> updated{fresh};
      for (LinkManifestEntry& e : entries) {
        if (e.result_key == fresh.result_key) continue;
        if (updated.size() >= kMaxLinkManifestEntries) break;
        updated.push_back(std::move(e));
      }
      storage::Blob manifest_blob;
      manifest_blob.dep_manifest = RenderLinkManifest(updated);
      manifest_blob.has_dep_manifest = true;
      storage::PutResult mput =
          cache->Put(pre_key, storage::SerializeBlob(manifest_blob));
      media_failed |= ReportCacheMediaErrors(mput.errors, cache_dir);
      RecordCounter(cache_dir, Counter::kStored);
    } else {
      RecordCounter(cache_dir, Counter::kStoreFailed);
    }
  }

  return media_failed && config.error_on_cache_media_failure
             ? kCacheMediaFailureExit
             : 0;
}

}  // namespace vcache::core
