// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// The manifest that lets a C or C++ compile skip the preprocessor on a hit.
//
// The ordinary key is a digest of the preprocessed text, so every lookup runs
// the preprocessor, and that run is most of what a hit costs. The manifest is
// keyed on what is known before the preprocessor runs: the source's path and
// contents, the whole command line, the compiler, the roots, and the identity
// of any precompiled header the command names. Each state remembers the files
// an earlier compile's depfile listed, with their digests, and the key that
// compile was stored under. A state is served only while every one of those
// files still hashes the same. This is ccache's depend mode on vcache's
// manifest machinery.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "args/compiler_args.h"
#include "core/depfile.h"
#include "core/manifest.h"
#include "core/roots.h"

namespace vcache::core {

struct CxxManifestState {
  // The preprocessor-mode key when the compile could be preprocessed, so both
  // modes share entries; a key derived from this state's digests when not.
  std::string result_key;
  std::vector<ManifestFile> files;
};

std::string RenderCxxManifest(const std::vector<CxxManifestState>& states);

// Returns false on any malformed line; the caller then starts a new manifest.
bool ParseCxxManifest(const std::string& text, std::vector<CxxManifestState>* states);

// The command line as the manifest key hashes it, canonicalised: everything the
// real compile is given except the source, the output and the dependency
// output, plus the dependency flags that change what the depfile says. Unlike
// the preprocessor key, -I, -D, -include and friends stay in, since there is no
// preprocessed text to stand in for them. Linker-only flags are dropped unless
// `keep_link_args`, for the reason ComputeKey drops them.
std::vector<std::string> CxxManifestKeyArgs(const args::CompilerArgs& parsed,
                                            const RootMap& roots, bool keep_link_args);

// `path` made absolute against `cwd`, spelled otherwise as given: no `.` or
// `..` is resolved, since that could cross a symlink the compiler followed.
std::string CxxAbsolutePath(const std::string& cwd, const std::string& path);

// Every directory the command line puts on the include path (-iquote, -I,
// -isystem, -idirafter, separate or joined), then those CPATH, C_INCLUDE_PATH,
// CPLUS_INCLUDE_PATH, OBJC_INCLUDE_PATH and OBJCPLUS_INCLUDE_PATH add, absolute
// against `cwd`.
std::vector<std::string> CxxIncludeDirs(const args::CompilerArgs& parsed,
                                        const std::string& cwd);

// The environment variables the preprocessor reads extra include directories
// from.
inline constexpr const char* kIncludePathEnvVars[] = {
    "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
    "OBJCPLUS_INCLUDE_PATH",
};

struct ConsumedPchs {
  // Absolute, in command-line order, without duplicates.
  std::vector<std::string> paths;
  // Set when the command uses a PCH form the manifest cannot identify.
  std::string unsupported;
};

// The precompiled headers a command consumes. Neither compiler's depfile names
// them -- clang lists the PCH's headers but not the .pch, gcc lists neither --
// so they reach the manifest key from the command line: -include-pch F,
// -Xclang -include-pch -Xclang F, and -include H where H.pch or H.gch sits
// beside H in the working directory or a directory on the include path. A .gch
// directory holds several PCHs gcc chooses between, and is unsupported.
ConsumedPchs FindConsumedPchs(const args::CompilerArgs& parsed, const std::string& cwd);

// What stands for one PCH in the manifest key, in order of preference:
//   "sum <digest>"         of <pch>.sum, when it is not older than the PCH. The
//                          build writes it for ccache's pch_external_checksum,
//                          from the PCH's inputs rather than its bytes, which
//                          vary between identical clang builds.
//   "vcache-key <digest>"  of <pch>.vcache-key, under the same age rule. vcache
//                          writes it beside a PCH it caches.
//   "bytes <digest>"       of the PCH itself, memoised in `memo_dir`.
// A side file older than the PCH describes an earlier build of it and is
// ignored. nullopt when the PCH cannot be read.
std::optional<std::string> PchIdentity(const std::string& pch, const std::string& memo_dir);

// The first of __DATE__, __TIME__ or __TIMESTAMP__ in `text`, if any. A file
// that uses one makes a manifest hit wrong: the digest stays the same while the
// expansion changes, which the preprocessor key would have seen.
std::optional<std::string_view> FindTimeMacro(std::string_view text);

struct RecordedInputs {
  std::vector<ManifestFile> files;
  // Local paths of the files whose bytes use __has_include or
  // __has_include_next, when the caller asked for them.
  std::vector<std::string> probing_files;
  // Set when the state must not be recorded, saying why.
  std::string declined;
};

// Reads every prerequisite `dep` lists, relative ones against `cwd`, and
// returns each one's canonical path and digest. Declines when a file cannot be
// read, uses a time macro, was modified at or after `started_ns` (it may not be
// the version the compile read), or, with `scan_incbin`, contains `.incbin`.
// With `find_probes`, also lists the files that use __has_include.
RecordedInputs RecordDepfileInputs(const DepFile& dep, const std::string& cwd,
                                   const RootMap& roots, int64_t started_ns,
                                   bool scan_incbin, bool find_probes = false);

// The directories a compiler searches for <...> includes with no -I given, as
// `<compiler> -E -v` lists them, lexically normalised. Empty when the list
// cannot be read.
std::vector<std::string> ParseSystemIncludeDirs(const std::string& verbose_stderr);

// True when `path`, lexically normalised, lies inside one of `dirs`.
bool UnderAnyDir(const std::string& path, const std::vector<std::string>& dirs);

// A .gch in any of `dirs` other than those in `keyed`, or nullopt. gcc uses a
// .gch beside a header an #include names without listing either in the
// depfile, so a state recorded beside one would not notice it being rebuilt.
std::optional<std::string> FindUnkeyedGch(const std::vector<std::string>& dirs,
                                          const std::vector<std::string>& keyed);

}  // namespace vcache::core
