// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// The C/C++ compile pipeline: parse, canonicalise, hash, look up, and either
// replay a cached result or compile and store one.
#pragma once

#include <string>
#include <vector>

#include "core/config.h"
#include "core/roots.h"
#include "storage/chain.h"

namespace vcache::core {

// Identity of the compiler, mixed into every cache key so that upgrading the
// toolchain invalidates entries. Resolved once per process and memoised in the
// cache directory, because the underlying check spawns `<compiler> -v`.
struct CompilerId {
  std::string fingerprint;
  std::string description;  // human-readable, for --show-stats and logs

  // True when the driver is clang. It differs from gcc in ways that change the
  // command line vcache builds, not just the key -- see BuildPreprocessCommand.
  bool is_clang = false;
};

// `check_mode` is one of "version" (default), "content" or "mtime":
//   version -- hash `<compiler> -v` output. Stable across machines, so it is
//              the right default once a shared S3 layer is in play.
//   content -- hash the driver binary itself.
//   mtime   -- hash path, size and mtime. Fastest, but machine-specific.
//
// `roots` is applied to whatever the mode hashes, because a compiler can live
// *inside* a mapped root -- a build that compiles its runtime libraries with
// the compiler it just built is the normal case, not an exotic one. Both the
// path (`mtime`) and the `-v` banner (`version`, which reports clang's
// `InstalledDir:` and the source repository it was configured from) then carry
// the checkout's location, and hashing that verbatim gives two checkouts of one
// revision different compiler identities and so a cold cache. Canonicalising
// first is the same rule vcache already applies to source paths.
CompilerId ResolveCompilerId(const std::string& compiler,
                             const std::string& check_mode,
                             const std::string& cache_dir,
                             const RootMap& roots);

// Builds the root mapping for this invocation from configuration, adding the
// current directory when it would otherwise be left unmapped.
RootMap BuildRootMap(const Config& config, std::vector<std::string>* warnings);

// Exit status used when error_on_cache_media_failure is set and a layer was
// broken. Deliberately outside the range compilers use, so a build log can
// tell "the cache is broken" apart from "the code does not compile".
constexpr int kCacheMediaFailureExit = 90;

// Handles the failures a cache lookup or store reported. Warns on stderr --
// unconditionally, because a silently degraded cache is the failure mode this
// exists to prevent -- and counts them. Returns true if any were reported, so
// the caller can fail the invocation when configured to.
//
// Note that this is about the *media*, not the entry: a miss reports nothing.
bool ReportCacheMediaErrors(const std::vector<std::string>& errors,
                            const std::string& cache_dir);

// Runs one compilation. `argv` starts at the compiler. Returns the exit code to
// propagate. Falls back to executing the compiler unchanged whenever caching is
// impossible, so the build always makes progress.
//
// A -M/-MM invocation, which emits a dependency list rather than an object, is
// routed to RunDepScan instead.
int RunCompile(const std::vector<std::string>& argv, const Config& config,
               const RootMap& roots, storage::CacheChain* cache);

// Runs one dependency-only (-M/-MM) invocation.
//
// These cannot be keyed the way a compile is. Deriving that key means running
// the preprocessor, and for a -M run the preprocessor *is* the work -- measured
// on a 115-header translation unit, `-E` costs 20 ms against the 16 ms `-M`
// itself, so the cache would be slower than the compiler. The key is therefore
// derived from the command line and the source alone, and the entry carries a
// manifest of every file the scan read. A hit re-hashes those files (about 1 ms
// for the same unit) and is only served if they all still match.
int RunDepScan(const std::vector<std::string>& argv, const Config& config,
               const RootMap& roots, storage::CacheChain* cache);

}  // namespace vcache::core
