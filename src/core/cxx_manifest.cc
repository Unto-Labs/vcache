// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "core/cxx_manifest.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <set>

#include "core/preprocessed.h"
#include "hash/file_memo.h"
#include "hash/hasher.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/str.h"

namespace vcache::core {
namespace {

constexpr std::string_view kCxxManifestHeader = "vcache-cxxmanifest-1";

std::string AbsoluteAgainst(const std::string& cwd, const std::string& path) {
  if (!path.empty() && path.front() == '/') return path;
  return cwd + "/" + path;
}

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
std::string CanonicalizeArg(const std::string& arg, const RootMap& roots) {
  static constexpr std::string_view kPathPrefixes[] = {
      "-isystem", "-iquote", "-idirafter", "-include", "-imacros", "-isysroot",
      "-iframework", "--sysroot=", "-I", "-F", "-B",
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
// RenderManifestFile line per file.
std::string RenderCxxManifest(const std::vector<CxxManifestState>& states) {
  std::string out(kCxxManifestHeader);
  out.push_back('\n');
  for (const CxxManifestState& state : states) {
    out += "state " + state.result_key + "\n";
    for (const ManifestFile& file : state.files) {
      out += RenderManifestFile(file);
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

std::vector<std::string> IncludeDirs(const args::CompilerArgs& parsed, const std::string& cwd) {
  static constexpr std::string_view kFlags[] = {"-iquote", "-I", "-isystem", "-idirafter"};
  std::vector<std::string> dirs;
  const std::vector<std::string>& argv = parsed.base_args;
  for (size_t i = 0; i < argv.size(); ++i) {
    for (std::string_view flag : kFlags) {
      if (argv[i] == flag && i + 1 < argv.size()) {
        dirs.push_back(AbsoluteAgainst(cwd, argv[++i]));
        break;
      }
      if (argv[i].size() > flag.size() && util::StartsWith(argv[i], flag)) {
        dirs.push_back(AbsoluteAgainst(cwd, argv[i].substr(flag.size())));
        break;
      }
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
      for (const std::string& dir : IncludeDirs(parsed, cwd)) {
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
      add(AbsoluteAgainst(cwd, argv[++i]));
    } else if (arg == "-Xclang" && i + 3 < argv.size() && argv[i + 1] == "-include-pch" &&
               argv[i + 2] == "-Xclang") {
      add(AbsoluteAgainst(cwd, argv[i + 3]));
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

RecordedInputs RecordDepfileInputs(const DepFile& dep, const std::string& cwd,
                                   const RootMap& roots, int64_t started_ns,
                                   bool scan_incbin) {
  RecordedInputs recorded;
  std::set<std::string> seen;
  for (const DepRule& rule : dep.rules) {
    for (const std::string& prereq : rule.prerequisites) {
      const std::string path = AbsoluteAgainst(cwd, prereq);
      if (!seen.insert(path).second) continue;
      const std::optional<int64_t> changed = LastChangeNs(path);
      const std::optional<std::string> bytes = util::ReadFile(path);
      if (!changed || !bytes) {
        recorded.declined = "cannot read " + path;
        return recorded;
      }
      if (*changed >= started_ns) {
        recorded.declined = path + " changed while it was being compiled";
        return recorded;
      }
      if (const auto macro = FindTimeMacro(*bytes)) {
        recorded.declined = path + " uses " + std::string(*macro);
        return recorded;
      }
      if (scan_incbin && ContainsIncbin(*bytes)) {
        recorded.declined = path + " contains .incbin";
        return recorded;
      }
      recorded.files.emplace_back(roots.Canonicalize(path), hash::HashString(*bytes));
    }
  }
  return recorded;
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

}  // namespace vcache::core
