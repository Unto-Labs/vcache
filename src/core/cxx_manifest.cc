// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "core/cxx_manifest.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>

#include "core/preprocessed.h"
#include "hash/file_memo.h"
#include "hash/hasher.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/str.h"

namespace vcache::core {
namespace {

constexpr std::string_view kCxxManifestHeader = "vcache-cxxmanifest-2";
constexpr std::string_view kAbsentLinePrefix = "absent ";

int64_t TimespecNs(const struct timespec& ts) {
  return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + static_cast<int64_t>(ts.tv_nsec);
}

// The newer of mtime and ctime. ctime moves too when a file is renamed into
// place or copied with its mtime preserved.
std::optional<int64_t> LastChangeNs(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return std::nullopt;
#if defined(__APPLE__)
  return std::max(TimespecNs(st.st_mtimespec), TimespecNs(st.st_ctimespec));
#else
  return std::max(TimespecNs(st.st_mtim), TimespecNs(st.st_ctim));
#endif
}

// Canonicalises the path inside a joined include-path flag, which CMake
// writes as -I/abs/dir. RootMap::Canonicalize matches whole paths only, so
// -I/home/a/proj/inc would otherwise stay apart from -I/home/b/proj/inc. Other
// joined values, -D among them, are left as written: a path inside a macro
// value reaches the object as spelled, so two checkouts must not share it.
// -B is left as written too: the cc1, as and specs it selects are in no
// depfile, so two trees must not share a state through it.
std::string CanonicalizeArg(const std::string& arg, const RootMap& roots) {
  static constexpr std::string_view kPathPrefixes[] = {
      "-isystem", "-iquote", "-idirafter", "-include", "-imacros", "-isysroot",
      "-iframework", "--sysroot=", "-I", "-F",
  };
  for (std::string_view prefix : kPathPrefixes) {
    if (arg.size() > prefix.size() && util::StartsWith(arg, prefix)) {
      return std::string(prefix) + roots.Canonicalize(std::string_view(arg).substr(prefix.size()));
    }
  }
  return roots.Canonicalize(arg);
}

}  // namespace

// A header line, then per state a "state <result key>" line followed by one
// RenderManifestFile line per file and one "absent <path>" line per absent
// path. A file line starts with a digest, so the two cannot be confused.
std::string RenderCxxManifest(const std::vector<CxxManifestState>& states) {
  std::string out(kCxxManifestHeader);
  out.push_back('\n');
  for (const CxxManifestState& state : states) {
    out += "state " + state.result_key + "\n";
    for (const ManifestFile& file : state.files) {
      out += RenderManifestFile(file);
      out.push_back('\n');
    }
    for (const std::string& path : state.absent) {
      out += kAbsentLinePrefix;
      out += path;
      out.push_back('\n');
    }
  }
  return out;
}

bool ParseCxxManifest(const std::string& text, std::vector<CxxManifestState>* states) {
  const auto lines = util::Split(text, '\n', /*skip_empty=*/true);
  if (lines.empty() || lines.front() != kCxxManifestHeader) return false;
  for (size_t i = 1; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    if (util::StartsWith(line, "state ")) {
      CxxManifestState state;
      state.result_key = line.substr(6);
      if (state.result_key.size() != hash::kDigestHexLen) return false;
      states->push_back(std::move(state));
      continue;
    }
    if (states->empty()) return false;
    if (util::StartsWith(line, kAbsentLinePrefix)) {
      std::string path = line.substr(kAbsentLinePrefix.size());
      if (path.empty() || path.front() != '/') return false;
      states->back().absent.push_back(std::move(path));
      continue;
    }
    ManifestFile file;
    if (!ParseManifestFile(line, &file)) return false;
    states->back().files.push_back(std::move(file));
  }
  return true;
}

std::vector<std::string> CxxManifestKeyArgs(const args::CompilerArgs& parsed,
                                            const RootMap& roots, bool keep_link_args) {
  std::vector<std::string> out;
  // -c and -S are not in base_args, and the same text compiled with each is a
  // different output.
  if (parsed.compile_only) out.push_back("-c");
  if (parsed.assemble_only) out.push_back("-S");
  for (size_t i = 0; i < parsed.base_args.size(); ++i) {
    const std::string& arg = parsed.base_args[i];
    if (!keep_link_args && args::IsLinkOnlyFlag(arg)) {
      if (args::LinkOnlyFlagTakesValue(arg) && i + 1 < parsed.base_args.size()) ++i;
      continue;
    }
    out.push_back(CanonicalizeArg(arg, roots));
  }
  // key_args adds what base_args leaves out and the output still depends on:
  // the -MT/-MQ target, -MP and the language.
  out.push_back("--vcache-key-args");
  for (const std::string& arg : parsed.key_args) out.push_back(roots.Canonicalize(arg));
  return out;
}

std::string CxxAbsolutePath(const std::string& cwd, const std::string& path) {
  if (!path.empty() && path.front() == '/') return path;
  return cwd + "/" + path;
}

std::vector<std::string> CxxIncludeDirs(const args::CompilerArgs& parsed,
                                        const std::string& cwd) {
  static constexpr std::string_view kFlags[] = {"-iquote", "-I", "-isystem", "-idirafter"};
  std::vector<std::string> dirs;
  const std::vector<std::string>& argv = parsed.base_args;
  for (size_t i = 0; i < argv.size(); ++i) {
    for (std::string_view flag : kFlags) {
      if (argv[i] == flag && i + 1 < argv.size()) {
        dirs.push_back(CxxAbsolutePath(cwd, argv[++i]));
        break;
      }
      if (argv[i].size() > flag.size() && util::StartsWith(argv[i], flag)) {
        dirs.push_back(CxxAbsolutePath(cwd, argv[i].substr(flag.size())));
        break;
      }
    }
  }
  // An empty element names the working directory, as it does for gcc.
  for (const char* name : kIncludePathEnvVars) {
    const char* value = std::getenv(name);
    if (value == nullptr) continue;
    for (const std::string& dir : util::Split(value, ':')) {
      dirs.push_back(dir.empty() ? cwd : CxxAbsolutePath(cwd, dir));
    }
  }
  return dirs;
}

ConsumedPchs FindConsumedPchs(const args::CompilerArgs& parsed, const std::string& cwd) {
  ConsumedPchs found;
  const auto add = [&](const std::string& path) {
    if (std::find(found.paths.begin(), found.paths.end(), path) == found.paths.end()) {
      found.paths.push_back(path);
    }
  };
  // gcc looks for H.gch in each directory it would search for H, and clang's
  // driver turns -include H into -include-pch H.pch (or H.gch) when that file
  // sits beside H. Every candidate that exists is keyed: an extra one costs a
  // miss when it changes, a missing one costs a wrong hit.
  const auto add_beside = [&](const std::string& header) {
    std::vector<std::string> candidates;
    if (!header.empty() && header.front() == '/') {
      candidates.push_back(header);
    } else {
      candidates.push_back(cwd + "/" + header);
      for (const std::string& dir : CxxIncludeDirs(parsed, cwd)) {
        candidates.push_back(dir + "/" + header);
      }
    }
    for (const std::string& candidate : candidates) {
      for (const char* suffix : {".pch", ".gch"}) {
        const std::string pch = candidate + suffix;
        if (util::IsDirectory(pch)) {
          found.unsupported = pch + " is a directory of precompiled headers";
        } else if (util::FileExists(pch)) {
          add(pch);
        }
      }
    }
  };

  const std::vector<std::string>& argv = parsed.base_args;
  for (size_t i = 0; i < argv.size(); ++i) {
    const std::string& arg = argv[i];
    if (arg == "-include-pch" && i + 1 < argv.size()) {
      add(CxxAbsolutePath(cwd, argv[++i]));
    } else if (arg == "-Xclang" && i + 3 < argv.size() && argv[i + 1] == "-include-pch" &&
               argv[i + 2] == "-Xclang") {
      add(CxxAbsolutePath(cwd, argv[i + 3]));
      i += 3;
    } else if (arg == "-include" && i + 1 < argv.size()) {
      add_beside(argv[++i]);
    } else if (arg.size() > 8 && util::StartsWith(arg, "-include")) {
      add_beside(arg.substr(8));
    }
  }
  return found;
}

std::optional<std::string> PchIdentity(const std::string& pch, const std::string& memo_dir) {
  const std::optional<int64_t> pch_mtime = util::FileMtime(pch);
  if (!pch_mtime) return std::nullopt;
  for (const auto& [suffix, label] : {std::pair{".sum", "sum"},
                                      std::pair{".vcache-key", "vcache-key"}}) {
    const std::string side = pch + suffix;
    const std::optional<int64_t> side_mtime = util::FileMtime(side);
    if (!side_mtime) continue;
    if (*side_mtime < *pch_mtime) {
      VCACHE_LOG("cxx manifest: " + side + " is older than the PCH; not using it");
      continue;
    }
    const std::optional<std::string> text = util::ReadFile(side);
    if (!text || util::TrimWhitespace(*text).empty()) continue;
    return std::string(label) + " " + hash::HashString(*text);
  }
  std::optional<std::string> digest = hash::HashFileMemoized(pch, memo_dir);
  if (!digest) return std::nullopt;
  return "bytes " + *digest;
}

std::optional<std::string_view> FindTimeMacro(std::string_view text) {
  static constexpr std::string_view kMacros[] = {"__DATE__", "__TIME__", "__TIMESTAMP__"};
  for (std::string_view macro : kMacros) {
    if (text.find(macro) != std::string_view::npos) return macro;
  }
  return std::nullopt;
}

bool IsPrecompiledHeaderPath(const std::string& path) {
  return util::EndsWith(path, ".gch") || util::EndsWith(path, ".pch");
}

RecordedInputs RecordDepfileInputs(const DepFile& dep, const std::string& cwd,
                                   const RootMap& roots, int64_t started_ns,
                                   bool scan_incbin, bool find_probes) {
  RecordedInputs recorded;
  std::set<std::string> seen;
  for (const DepRule& rule : dep.rules) {
    for (const std::string& prereq : rule.prerequisites) {
      const std::string path = CxxAbsolutePath(cwd, prereq);
      if (!seen.insert(path).second) continue;
      // Read before the stat, so an edit that lands between the two moves the
      // timestamps past started_ns rather than slipping in unseen.
      const std::optional<std::string> bytes = util::ReadFile(path);
      const std::optional<int64_t> changed = LastChangeNs(path);
      if (!changed || !bytes) {
        recorded.declined = "cannot read " + path;
        return recorded;
      }
      if (*changed >= started_ns) {
        recorded.declined = path + " changed while it was being compiled";
        return recorded;
      }
      // A depfile from the absent-deps patch names the PCH the compile read.
      // Its bytes are not source: every PCH holds the names __DATE__ and
      // __TIME__ among its identifiers, and none is assembled or probed.
      if (IsPrecompiledHeaderPath(path)) {
        recorded.files.emplace_back(roots.Canonicalize(path), hash::HashString(*bytes));
        continue;
      }
      if (const auto macro = FindTimeMacro(*bytes)) {
        recorded.declined = path + " uses " + std::string(*macro);
        return recorded;
      }
      if (scan_incbin && ContainsIncbin(*bytes)) {
        recorded.declined = path + " contains .incbin";
        return recorded;
      }
      if (find_probes && bytes->find("__has_include") != std::string::npos) {
        recorded.probing_files.push_back(path);
      }
      recorded.files.emplace_back(roots.Canonicalize(path), hash::HashString(*bytes));
    }
  }
  return recorded;
}

std::vector<std::string> ParseSystemIncludeDirs(const std::string& verbose_stderr) {
  std::vector<std::string> dirs;
  bool listing = false;
  for (const std::string& line : util::Split(verbose_stderr, '\n')) {
    if (util::StartsWith(line, "#include <...> search starts here:")) {
      listing = true;
      continue;
    }
    if (!listing) continue;
    if (util::StartsWith(line, "End of search list.")) return dirs;
    std::string dir = util::TrimWhitespace(line);
    // macOS marks framework directories with a suffix.
    const std::string_view kFramework = " (framework directory)";
    if (util::EndsWith(dir, kFramework)) dir.resize(dir.size() - kFramework.size());
    if (!dir.empty() && dir.front() == '/') {
      dirs.push_back(std::filesystem::path(dir).lexically_normal().string());
    }
  }
  return {};
}

std::string SystemIncludeMemoDir(const std::string& cache_dir) {
  return cache_dir + "/sysinc";
}

size_t PruneSystemIncludeMemos(const std::string& cache_dir, int64_t max_age_seconds) {
  const std::string dir = SystemIncludeMemoDir(cache_dir);
  if (!util::IsDirectory(dir)) return 0;
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  size_t removed = 0;
  for (const util::FileEntry& entry : util::ListFilesRecursive(dir)) {
    if (now - entry.lru_time > max_age_seconds && util::RemoveFile(entry.path)) ++removed;
  }
  return removed;
}

bool UnderAnyDir(const std::string& path, const std::vector<std::string>& dirs) {
  const std::string normal = std::filesystem::path(path).lexically_normal().string();
  for (std::string dir : dirs) {
    if (!dir.empty() && dir.back() != '/') dir.push_back('/');
    if (util::StartsWith(normal, dir)) return true;
  }
  return false;
}

std::optional<std::string> FindUnkeyedGch(const std::vector<std::string>& dirs,
                                          const std::vector<std::string>& keyed) {
  std::set<std::string> listed;
  for (const std::string& dir : dirs) {
    if (!listed.insert(dir).second) continue;
    DIR* handle = ::opendir(dir.c_str());
    if (handle == nullptr) continue;
    std::optional<std::string> found;
    while (const dirent* entry = ::readdir(handle)) {
      const std::string_view name = entry->d_name;
      if (!util::EndsWith(name, ".gch")) continue;
      const std::string path = dir + "/" + std::string(name);
      if (std::find(keyed.begin(), keyed.end(), path) != keyed.end()) continue;
      found = path;
      break;
    }
    ::closedir(handle);
    if (found) return found;
  }
  return std::nullopt;
}

namespace {

// std::filesystem's lexically_normal for an absolute path, without a trailing
// slash, at a fraction of the cost: a trace has thousands of paths to classify.
// A relative path goes through std::filesystem.
std::string LexicalNormal(const std::string& path) {
  if (path.empty() || path.front() != '/') {
    std::string normal = std::filesystem::path(path).lexically_normal().string();
    if (normal.size() > 1 && normal.back() == '/') normal.pop_back();
    return normal;
  }
  std::vector<std::string_view> segments;
  const std::string_view view = path;
  size_t begin = 0;
  while (begin < view.size()) {
    size_t end = view.find('/', begin);
    if (end == std::string_view::npos) end = view.size();
    const std::string_view segment = view.substr(begin, end - begin);
    if (segment == "..") {
      if (!segments.empty()) segments.pop_back();
    } else if (!segment.empty() && segment != ".") {
      segments.push_back(segment);
    }
    begin = end + 1;
  }
  std::string normal;
  normal.reserve(path.size());
  for (std::string_view segment : segments) {
    normal.push_back('/');
    normal.append(segment);
  }
  return normal.empty() ? "/" : normal;
}

// `normal` is `dir` or lies inside it; both lexically normal.
bool InNormalDir(const std::string& normal, const std::string& dir) {
  if (dir == "/") return true;
  return util::StartsWith(normal, dir) &&
         (normal.size() == dir.size() || normal[dir.size()] == '/');
}

bool InAnyNormalDir(const std::string& normal, const std::vector<std::string>& dirs) {
  for (const std::string& dir : dirs) {
    if (InNormalDir(normal, dir)) return true;
  }
  return false;
}

std::vector<std::string> NormalAll(const std::vector<std::string>& paths) {
  std::vector<std::string> normal;
  normal.reserve(paths.size());
  for (const std::string& path : paths) normal.push_back(LexicalNormal(path));
  return normal;
}

// The filter with every path in it lexically normal, so that classifying a
// trace normalises each traced path once.
CxxTraceFilter NormalFilter(const CxxTraceFilter& filter) {
  CxxTraceFilter normal = filter;
  if (!filter.scratch_dir.empty()) normal.scratch_dir = LexicalNormal(filter.scratch_dir);
  if (!filter.tracer.empty()) normal.tracer = LexicalNormal(filter.tracer);
  normal.keyed_pchs = NormalAll(filter.keyed_pchs);
  normal.compiler_files = NormalAll(filter.compiler_files);
  normal.compiler_dirs = NormalAll(filter.compiler_dirs);
  normal.include_dirs = NormalAll(filter.include_dirs);
  normal.search_path_dirs = NormalAll(filter.search_path_dirs);
  return normal;
}

bool Contains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool IsNoiseUnderNormalFilter(const std::string& path, const std::string& normal,
                              const CxxTraceFilter& filter) {
  if (IsIgnoredPath(path) || IsIgnoredPath(normal)) return true;
  if (normal == filter.tracer) return true;
  if (Contains(filter.keyed_pchs, normal) || Contains(filter.compiler_files, normal)) return true;
  if (!filter.scratch_dir.empty() && InNormalDir(normal, filter.scratch_dir)) return true;
  return InAnyNormalDir(normal, filter.compiler_dirs) &&
         !InAnyNormalDir(normal, filter.include_dirs);
}

// A directory `dir` names, and each spelling a symlink in / gives it: on a
// merged /usr, /lib64 is /usr/lib64, and the loader and clang's driver both
// use the short form.
void AddWithTopLevelAliases(const std::string& dir, std::vector<std::string>* dirs) {
  dirs->push_back(dir);
  DIR* top = ::opendir("/");
  if (top == nullptr) return;
  while (const dirent* entry = ::readdir(top)) {
    const std::string alias = std::string("/") + entry->d_name;
    struct stat st;
    if (::lstat(alias.c_str(), &st) != 0 || !S_ISLNK(st.st_mode)) continue;
    const std::optional<std::string> target = util::RealPath(alias);
    if (!target || *target == "/") continue;
    if (dir == *target || util::StartsWith(dir, *target + "/")) {
      dirs->push_back(alias + dir.substr(target->size()));
    } else if (util::StartsWith(*target, dir + "/")) {
      dirs->push_back(alias);
    }
  }
  ::closedir(top);
}

std::vector<std::string> Sorted(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

}  // namespace

std::string WithoutDotSegments(const std::string& path) {
  std::string out;
  out.reserve(path.size());
  size_t i = 0;
  while (i < path.size()) {
    if (path[i] != '/') {
      out.push_back(path[i++]);
      continue;
    }
    while (i < path.size() && path[i] == '/') ++i;
    if (i < path.size() && path[i] == '.' && (i + 1 == path.size() || path[i + 1] == '/')) {
      ++i;
      continue;
    }
    out.push_back('/');
  }
  if (out.size() > 1 && out.back() == '/') out.pop_back();
  return out.empty() ? "/" : out;
}

std::vector<std::string> CompilerInstallDirs(const std::vector<std::string>& executables) {
  std::set<std::string> prefixes;
  for (const std::string& executable : executables) {
    const std::string normal = LexicalNormal(executable);
    const size_t libexec = normal.find("/libexec/");
    if (libexec != std::string::npos) {
      prefixes.insert(normal.substr(0, libexec));
      continue;
    }
    const std::string dir = util::DirName(normal);
    const std::string prefix = util::BaseName(dir) == "bin" ? util::DirName(dir) : dir;
    prefixes.insert(prefix == "/" ? "" : prefix);
  }
  std::vector<std::string> dirs;
  for (const std::string& prefix : prefixes) {
    for (const char* tree : {"/lib/gcc", "/lib64/gcc", "/lib32/gcc", "/libx32/gcc",
                             "/libexec/gcc", "/lib/clang"}) {
      AddWithTopLevelAliases(prefix + tree, &dirs);
    }
  }
  return Sorted(std::move(dirs));
}

std::vector<std::string> SearchPathDirs(const std::string& search_path) {
  std::vector<std::string> dirs;
  for (const std::string& entry : util::Split(search_path, ':')) {
    if (!entry.empty() && entry.front() == '/') dirs.push_back(LexicalNormal(entry));
  }
  return dirs;
}

bool IsCxxTraceNoise(const std::string& path, const CxxTraceFilter& filter) {
  return IsNoiseUnderNormalFilter(path, LexicalNormal(path), NormalFilter(filter));
}

CxxTraceInputs SelectCxxTraceInputs(const TraceRecords& records, const CxxTraceFilter& filter) {
  const CxxTraceFilter normal_filter = NormalFilter(filter);
  const auto select = [&](const std::vector<std::string>& from) {
    std::vector<std::string> kept;
    for (const std::string& path : from) {
      if (!IsNoiseUnderNormalFilter(path, LexicalNormal(path), normal_filter)) {
        kept.push_back(WithoutDotSegments(path));
      }
    }
    return Sorted(std::move(kept));
  };
  const auto spelled = [](const std::vector<std::string>& from) {
    std::vector<std::string> out;
    out.reserve(from.size());
    for (const std::string& path : from) out.push_back(WithoutDotSegments(path));
    return Sorted(std::move(out));
  };
  CxxTraceInputs selected;
  selected.reads = select(records.reads);
  selected.all_reads = spelled(records.reads);
  selected.written = spelled(records.written);
  // Probed and then found is a read: gcc probes the name of its own
  // intermediate before creating it, and a header can be created mid-compile.
  // Compared as spelled: through a symlinked directory, a/../b and b differ.
  for (std::string& path : select(records.absent)) {
    if (std::binary_search(selected.reads.begin(), selected.reads.end(), path)) continue;
    const std::string normal = LexicalNormal(path);
    if (Contains(normal_filter.search_path_dirs, util::DirName(normal)) &&
        Contains(normal_filter.search_path_names, util::BaseName(normal))) {
      continue;
    }
    selected.absent.push_back(std::move(path));
  }
  return selected;
}

CxxAbsentList ParseCxxAbsentList(const std::string& text, const std::string& cwd) {
  CxxAbsentList list;
  for (const std::string& line : util::Split(text, '\n', /*skip_empty=*/true)) {
    const std::string path = CxxAbsolutePath(cwd, line);
    struct stat st;
    if (!list.existing_path && ::lstat(path.c_str(), &st) == 0) list.existing_path = path;
    list.paths.push_back(WithoutDotSegments(path));
  }
  list.paths = Sorted(std::move(list.paths));
  return list;
}

std::optional<std::string> FindUntracedFile(const std::vector<ManifestFile>& files,
                                            const RootMap& roots, const CxxTraceInputs& trace) {
  for (const ManifestFile& file : files) {
    const std::string local = WithoutDotSegments(roots.Localize(file.first));
    if (!std::binary_search(trace.all_reads.begin(), trace.all_reads.end(), local)) return local;
  }
  return std::nullopt;
}

void AddCxxTraceInputs(const CxxTraceInputs& trace, const RootMap& roots, const std::string& cwd,
                       int64_t started_ns, const std::string& memo_dir,
                       RecordedInputs* recorded) {
  std::set<std::string> listed;
  for (const ManifestFile& file : recorded->files) {
    listed.insert(WithoutDotSegments(roots.Localize(file.first)));
  }
  const auto decline = [&](std::string why) {
    recorded->files.clear();
    recorded->absent.clear();
    recorded->declined = std::move(why);
  };
  for (const std::string& path : trace.reads) {
    if (!listed.insert(path).second) continue;
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
      if (std::binary_search(trace.written.begin(), trace.written.end(), path)) continue;
      return decline(path + " was read by the compile and is gone");
    }
    if (S_ISDIR(st.st_mode)) continue;
    if (!S_ISREG(st.st_mode)) return decline(path + " is not a regular file");
    const std::optional<std::string> digest = hash::HashFileMemoized(path, memo_dir);
    const std::optional<int64_t> changed = LastChangeNs(path);
    if (!digest || !changed) return decline("cannot read " + path);
    if (*changed >= started_ns) return decline(path + " changed while it was being compiled");
    recorded->files.emplace_back(roots.Canonicalize(path), *digest);
  }
  // Below a directory that does not exist, one stat of the directory stands
  // for every probe inside it: nothing can appear there without it. gcc probes
  // each header in each -I directory, so this cuts its list to what clang,
  // which skips a missing directory, records anyway. The walk stops at "..",
  // which would cross a symlink lexically, and at the working directory and
  // every root: above a root, the path would name one tree's location, and a
  // tree elsewhere would check that instead of its own.
  std::set<std::string> walk_stops = {WithoutDotSegments(cwd)};
  for (const Root& root : roots.roots()) walk_stops.insert(root.path);
  std::map<std::string, bool> exists;
  const auto present = [&](const std::string& dir) {
    auto [it, inserted] = exists.try_emplace(dir, false);
    if (inserted) {
      struct stat st;
      it->second = ::lstat(dir.c_str(), &st) == 0;
    }
    return it->second;
  };
  for (const std::string& path : trace.absent) {
    std::string highest = path;
    if (highest.find("/../") == std::string::npos) {
      for (std::string parent = util::DirName(highest);
           walk_stops.count(highest) == 0 && parent.size() > 1 && parent != highest &&
           !present(parent);
           parent = util::DirName(parent)) {
        highest = parent;
      }
    }
    recorded->absent.push_back(roots.Canonicalize(highest));
  }
  recorded->absent = Sorted(std::move(recorded->absent));
}

}  // namespace vcache::core
