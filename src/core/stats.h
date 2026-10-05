// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// Persistent hit/miss counters.
//
// Kept in a single file under the cache directory, updated under an advisory
// lock. A parallel build has many vcache processes bumping counters at once, so
// each update is a short read-modify-write while holding flock(2); the window is
// microseconds and never overlaps compilation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "core/reason.h"

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
  kCacheMediaError,
  kCount,  // sentinel
};

struct Stats {
  uint64_t values[static_cast<size_t>(Counter::kCount)] = {};

  // Keyed by reason name. Names this build does not know were written by a
  // newer one and are carried through unchanged.
  std::map<std::string, uint64_t, std::less<>> reasons;

  uint64_t Get(Counter c) const { return values[static_cast<size_t>(c)]; }
  void Add(Counter c, uint64_t n = 1) { values[static_cast<size_t>(c)] += n; }

  uint64_t GetReason(Reason reason) const;
  // Counts the reason and, when its outcome has one, the positional counter.
  void AddReason(Reason reason, uint64_t n = 1);
};

// The stats file: the counters one decimal per line, in Counter order,
// then one "reason<TAB>name<TAB>count" line per non-zero reason. Versions that
// predate the reason lines read only the leading counters.
Stats ParseStats(const std::string& text);
std::string RenderStats(const Stats& stats);

// Increments one counter in the on-disk stats file. Failures are silent: losing
// a statistic must never disturb a build.
void RecordCounter(const std::string& cache_dir, Counter counter);

// Logs the decision and counts it under its reason and outcome. Failures are
// silent, as for RecordCounter.
void RecordDecision(const std::string& cache_dir, const Decision& decision);

Stats ReadStats(const std::string& cache_dir);

bool ZeroStats(const std::string& cache_dir);

// Formats stats for `vcache --show-stats`.
std::string FormatStats(const Stats& stats, const std::string& cache_dir,
                        uint64_t cache_bytes, uint64_t max_bytes);

}  // namespace vcache::core
