// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// File digests memoised by file identity, the way ccache's inode cache works.
//
// A rustc lookup hashes every --extern, and those are rlibs and rmetas of
// hundreds of megabytes that rarely change between builds. Hashing them in full
// on every lookup dominated the cost of a hit, so their digests are remembered
// under <cache_dir>/filehash, keyed by what stat(2) says about the file.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace vcache::hash {

std::string FileDigestMemoDir(const std::string& cache_dir);

// Same answer as HashFile, read from `memo_dir` when the file's device, inode,
// size, mtime and ctime match a memo written earlier.
std::optional<std::string> HashFileMemoized(const std::string& path,
                                            const std::string& memo_dir);

// HashFileMemoized with the clock passed in, so a test can treat a file it has
// just written as settled.
std::optional<std::string> HashFileMemoizedAt(const std::string& path,
                                              const std::string& memo_dir,
                                              int64_t now_unix_ns);

// Removes memos not rewritten for `max_age_seconds` and returns how many it
// removed. A memo is never refreshed on read, so a live one is rewritten from
// a full hash once per that period.
size_t PruneFileDigestMemos(const std::string& cache_dir, int64_t max_age_seconds);

}  // namespace vcache::hash
