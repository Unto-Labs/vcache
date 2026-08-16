// SPDX-License-Identifier: GPL-3.0-or-later
// Persistent hit/miss counters.
//
// Kept in a single file under the cache directory, updated under an advisory
// lock. A parallel build has many vcache processes bumping counters at once, so
// each update is a short read-modify-write while holding flock(2); the window is
// microseconds and never overlaps compilation.
#pragma once

#include <cstdint>
#include <string>

namespace vcache::core {

enum class Counter {
  kHitDisk = 0,
  kHitS3,
  kMiss,
  kUncacheable,
  kCompileFailed,
  kStored,
  kStoreFailed,
  kPreprocessFailed,
  kCount,  // sentinel
};

struct Stats {
  uint64_t values[static_cast<size_t>(Counter::kCount)] = {};

  uint64_t Get(Counter c) const { return values[static_cast<size_t>(c)]; }
  void Add(Counter c, uint64_t n = 1) { values[static_cast<size_t>(c)] += n; }
};

// Increments one counter in the on-disk stats file. Failures are silent: losing
// a statistic must never disturb a build.
void RecordCounter(const std::string& cache_dir, Counter counter);

Stats ReadStats(const std::string& cache_dir);

bool ZeroStats(const std::string& cache_dir);

// Formats stats for `vcache --show-stats`.
std::string FormatStats(const Stats& stats, const std::string& cache_dir,
                        uint64_t cache_bytes, uint64_t max_bytes);

}  // namespace vcache::core
