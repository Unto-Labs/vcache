// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// The manifest that lets a C or C++ compile skip the preprocessor on a hit.
//
// The ordinary key is a digest of the preprocessed text, so every lookup runs
// the preprocessor, and that run is most of what a hit costs. The manifest is
// keyed on what is known before the preprocessor runs: the source's path and
// contents, the whole command line, the compiler, the roots, and the identity
// of any precompiled header the command names. Each state remembers the files
// an earlier compile read, with their digests, the paths it probed and did not
// find, and the key that compile was stored under. A state is served only while
// every one of those files still hashes the same and every one of those paths
// is still absent. The files come from the compile's depfile, as in ccache's
// depend mode, and, where vcache-fstrace.so can watch the compile, from what
// it saw the compiler open; the absent paths from the same trace, or from the
// compiler's own -fabsent-deps list when it has the patch that writes one.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "args/compiler_args.h"
#include "core/depfile.h"
#include "core/link_trace.h"
#include "core/manifest.h"
#include "core/roots.h"

namespace vcache::core {

struct CxxManifestState {
  // The preprocessor-mode key when the compile could be preprocessed, so both
  // modes share entries; a key derived from this state's digests when not.
  std::string result_key;
  std::vector<ManifestFile> files;
  // Canonical paths the compile probed and did not find. A header created at
  // one would shadow a recorded file, or answer a __has_include differently.
  std::vector<std::string> absent;
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

// The variables the compiler driver itself reads that change what it compiles
// or which tools it runs: the SDK and toolchain selection Apple's drivers take
// from the environment, clang's CCC_OVERRIDE_OPTIONS (which edits the command
// line), and gcc's GCC_EXEC_PREFIX and COMPILER_PATH (which choose cc1, as and
// the headers beside them). Every `*_DEPLOYMENT_TARGET` counts too.
inline constexpr const char* kDriverEnvVars[] = {
    "SDKROOT", "DEVELOPER_DIR", "TOOLCHAINS", "CCC_OVERRIDE_OPTIONS",
    "GCC_EXEC_PREFIX", "COMPILER_PATH",
};

// Those of the driver variables that are set, as sorted name/value pairs.
// Unset ones are left out, so a key that feeds this list does not change for an
// environment that sets none of them.
std::vector<std::pair<std::string, std::string>> DriverEnvironment();

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
  // Canonical, sorted; only a traced compile, or one that listed them,
  // records any.
  std::vector<std::string> absent;
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

// What in the trace of a compile is not an input of it.
struct CxxTraceFilter {
  // This compile's scratch directory: the object, the depfile, the trace.
  std::string scratch_dir;
  std::string tracer;
  // The precompiled headers the manifest key already identifies. Their bytes
  // can differ between trees whose PCHs share one identity.
  std::vector<std::string> keyed_pchs;
  // The executables that ran and the shared objects they loaded.
  std::vector<std::string> compiler_files;
  // The compiler's own trees, from CompilerInstallDirs.
  std::vector<std::string> compiler_dirs;
  // Every directory the preprocessor searches, the compiler's own included.
  // Nothing under one is dropped as the compiler's: a header appearing in or
  // vanishing from /usr/include changes what the preprocessor finds.
  std::vector<std::string> include_dirs;
  // Directories on PATH, and the names of the compiler's executables. gcc's
  // driver looks for itself and its tools along PATH, and which ones run is
  // the compiler identity's business, so an absent path that is one of those
  // names directly in one of those directories is dropped.
  std::vector<std::string> search_path_dirs;
  std::vector<std::string> search_path_names;
};

// The absolute directories on `search_path` (PATH's format).
std::vector<std::string> SearchPathDirs(const std::string& search_path);

// The trees of the compiler whose processes ran `executables`: under each
// one's prefix (the parent of its bin/, or what precedes /libexec/), its
// lib*/gcc, libexec/gcc and lib/clang. The compiler fingerprint in the key
// stands for these. A tree reached through a symlink at the top of the
// filesystem (/lib64 on a merged /usr) is listed under that spelling too.
std::vector<std::string> CompilerInstallDirs(const std::vector<std::string>& executables);

// True for a path in a compile's trace that says nothing about its inputs: the
// link cache's IsIgnoredPath, the tracer, the keyed PCHs, the scratch
// directory, the compiler's files, and its trees outside every include
// directory.
bool IsCxxTraceNoise(const std::string& path, const CxxTraceFilter& filter);

// `path` with "." segments and doubled separators dropped, which never change
// what it names. ".." stays: resolving it could cross a symlink.
std::string WithoutDotSegments(const std::string& path);

struct CxxTraceInputs {
  // Absolute, spelled as WithoutDotSegments gives them, sorted.
  std::vector<std::string> reads;
  // The same, without any path that was also read.
  std::vector<std::string> absent;
  // Every path any traced process read, noise included: the evidence that the
  // process which preprocessed was traced at all.
  std::vector<std::string> all_reads;
  // Every path opened for writing only.
  std::vector<std::string> written;
};

// The files a traced compile read and the paths it found absent, with the
// noise dropped. Executables and the shared objects they loaded are the
// compiler's own, and a file only written is an output, so none is a read.
CxxTraceInputs SelectCxxTraceInputs(const TraceRecords& records, const CxxTraceFilter& filter);

// The first of `files` (canonical) that no traced process read, as a local
// path, or nullopt. A compiler process that did not load the tracer (static,
// another ELF class, setuid, a wrapper that drops LD_PRELOAD) records nothing,
// so a state taken from its trace would miss every absent path; the files its
// depfile names are what show it was traced.
std::optional<std::string> FindUntracedFile(const std::vector<ManifestFile>& files,
                                            const RootMap& roots, const CxxTraceInputs& trace);

// True for the executable of a compiler front end, the process that runs the
// preprocessor: gcc's cc1 family, or clang (clang, clang++, clang-18, ...),
// which preprocesses in its own -cc1 process.
bool IsCompilerFrontEnd(const std::string& executable);

// True when some process of the trace is a compiler front end. Without one the
// include search did not run under the tracer -- `ccache gcc` serving a hit of
// its own reads every header and writes the depfile, but probes nothing -- so
// the trace has no absent paths to offer however complete its reads look.
bool TraceShowsFrontEnd(const TraceRecords& records);

// The first of `include_dirs` (absolute) that does not exist and that no
// traced process was seen probing, or nullopt. gcc and clang stat every
// directory on the include path once; a missing one absent from the trace means
// the tracer missed the call that looked, and with it the path whose creation
// would make a later hit stale.
std::optional<std::string> FindUnprobedIncludeDir(const std::vector<std::string>& include_dirs,
                                                  const TraceRecords& records);

// Adds to `recorded` every file of `trace` it does not list yet, by digest,
// and every absent path, as the highest of its ancestors that does not exist
// either, stopping at `cwd` and at every root. Declines, emptying `recorded`,
// when such a file is gone (unless the trace wrote it too: the compiler's own
// intermediate), is neither a file nor a directory, or changed at or after
// `started_ns`. A directory is skipped: what it held is in the trace. Unlike a
// depfile's headers these files were not preprocessed, so a time macro in one
// changes nothing.
void AddCxxTraceInputs(const CxxTraceInputs& trace, const RootMap& roots, const std::string& cwd,
                       int64_t started_ns, const std::string& memo_dir,
                       RecordedInputs* recorded);

// What a compiler's `-fabsent-deps=FILE` list says: the devtools gcc and clang
// patch writes every path the preprocessor looked for and did not find, one
// per line, relative ones against the compile's working directory.
struct CxxAbsentList {
  // Absolute, spelled as WithoutDotSegments gives them, sorted.
  std::vector<std::string> paths;
  // A listed path that exists, or whose absence lstat cannot show, as listed.
  // A state holding it would never be served, and a list naming a path that
  // exists cannot be trusted to be whole.
  std::optional<std::string> existing_path;
};

// Parses `text`, skipping empty lines and dropping a trailing carriage return,
// and lstats every path.
CxxAbsentList ParseCxxAbsentList(const std::string& text, const std::string& cwd);

// The directories a compiler searches for <...> includes with no -I given, as
// `<compiler> -E -v` lists them, lexically normalised. Empty when the list
// cannot be read.
std::vector<std::string> ParseSystemIncludeDirs(const std::string& verbose_stderr);

// Where SystemIncludeDirs in compile.cc memoises the list, and the sweep
// `--trim` runs over it: memos not rewritten for `max_age_seconds` go.
std::string SystemIncludeMemoDir(const std::string& cache_dir);
size_t PruneSystemIncludeMemos(const std::string& cache_dir, int64_t max_age_seconds);

// True when `path`, lexically normalised, lies inside one of `dirs`.
bool UnderAnyDir(const std::string& path, const std::vector<std::string>& dirs);

// A .gch in any of `dirs` other than those in `keyed`, or nullopt. gcc uses a
// .gch beside a header an #include names without listing either in the
// depfile, so a state recorded beside one would not notice it being rebuilt.
std::optional<std::string> FindUnkeyedGch(const std::vector<std::string>& dirs,
                                          const std::vector<std::string>& keyed);

}  // namespace vcache::core
