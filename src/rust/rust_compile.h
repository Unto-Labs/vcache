// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// The rustc compile pipeline.
//
// Structure mirrors the C/C++ path, but the "what did this compilation read"
// step is different: instead of preprocessing, vcache asks rustc for
// `--emit=dep-info` and hashes every file it names, plus the contents of each
// --extern dependency. That run expands every macro, so a lookup first tries
// the manifest of earlier runs (rust_manifest.h) and only asks rustc when no
// remembered state still matches.
//
// Outputs are captured by compiling into a temporary directory and recording
// everything that appears there, which avoids having to model rustc's naming
// rules for rlib/rmeta/dSYM artifacts. A miss moves them into the output
// directory by rename. With a disk cache and no S3, an output of 8 MiB or more
// is stored beside the entry as a content-addressed sidecar and restored by
// reflink, instead of travelling through the entry and memory.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/config.h"
#include "core/roots.h"
#include "storage/chain.h"

namespace vcache::rust {

// Hash of `rustc -vV`. The memo that avoids a probe per crate is local; what
// enters a cache key is only the banner, so two machines still share entries.
std::string ResolveRustcFingerprint(const std::string& rustc,
                                    const std::string& cache_dir);

// False when a stored output name could write outside the output directory:
// empty, a NUL, absolute, or any `..` component. A `.` component is allowed.
bool IsSafeOutputName(std::string_view name);

// False when the stage dir holds a symlink or any other non-regular file.
// `unparsed_dep_info` names a .d left as rustc wrote it because it did not
// parse; such a set may be placed but never stored. A file other than a .d of
// at least `sidecar_min_bytes` is not read: it goes to `sidecar_files`, by
// name, with no digest yet.
bool CaptureOutputs(const std::string& dir, const core::RootMap& roots,
                    const std::vector<std::string>& path_env_vars,
                    uint64_t sidecar_min_bytes, std::vector<storage::BlobFile>* files,
                    std::vector<storage::BlobSidecarFile>* sidecar_files,
                    std::string* unparsed_dep_info);

// `placed`, when given, counts the files written before any failure.
bool RestoreOutputs(const std::vector<storage::BlobFile>& files, const std::string& out_dir,
                    const core::RootMap& roots,
                    const std::vector<std::string>& path_env_vars, size_t* placed = nullptr);

struct ScannedOutput {
  std::string digest;
  uint64_t size = 0;
  // Index into the patterns of the first one found.
  std::optional<size_t> found_pattern;
};

// Hashes a file and searches it for `patterns` in one streaming read, so a
// large output is never held in memory. A pattern split across two reads is
// still found. nullopt when the file cannot be read whole, including when its
// size changes under the read.
std::optional<ScannedOutput> HashAndScanFile(const std::string& path,
                                             const std::vector<std::string>& patterns,
                                             size_t chunk_bytes = 1 << 20);

int RunRustCompile(const std::vector<std::string>& argv,
                   const core::Config& config, const core::RootMap& roots,
                   storage::CacheChain* cache);

}  // namespace vcache::rust
