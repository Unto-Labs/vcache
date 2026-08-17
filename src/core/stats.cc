// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "core/stats.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sstream>

#include "util/fs.h"
#include "util/str.h"

namespace vcache::core {
namespace {

constexpr const char* kNames[] = {
    "cache hit (disk)", "cache hit (s3)",   "cache miss",
    "uncacheable",      "compile failed",   "entries stored",
    "store failed",     "preprocess failed",
    "cache media errors",
};
static_assert(sizeof(kNames) / sizeof(kNames[0]) ==
                  static_cast<size_t>(Counter::kCount),
              "counter name table out of sync with the Counter enum");

std::string StatsPath(const std::string& cache_dir) {
  return cache_dir + "/stats";
}

// Text format, one decimal counter per line: trivially inspectable and
// forward-compatible, since missing trailing lines read as zero.
Stats ParseStats(const std::string& text) {
  Stats stats;
  size_t index = 0;
  for (const std::string& line : util::Split(text, '\n')) {
    if (index >= static_cast<size_t>(Counter::kCount)) break;
    const std::string trimmed = util::TrimWhitespace(line);
    if (trimmed.empty()) continue;
    stats.values[index++] = std::strtoull(trimmed.c_str(), nullptr, 10);
  }
  return stats;
}

std::string RenderStats(const Stats& stats) {
  std::string out;
  for (size_t i = 0; i < static_cast<size_t>(Counter::kCount); ++i) {
    out += std::to_string(stats.values[i]);
    out.push_back('\n');
  }
  return out;
}

// Runs `mutate` on the counters while holding an exclusive lock on the file.
bool WithLockedStats(const std::string& cache_dir,
                     const std::function<void(Stats*)>& mutate) {
  if (!util::MakeDirs(cache_dir)) return false;
  const std::string path = StatsPath(cache_dir);

  int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) return false;
  if (::flock(fd, LOCK_EX) != 0) {
    ::close(fd);
    return false;
  }

  std::string contents;
  char buf[4096];
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof(buf))) > 0) contents.append(buf, static_cast<size_t>(n));

  Stats stats = ParseStats(contents);
  mutate(&stats);
  const std::string rendered = RenderStats(stats);

  bool ok = ::lseek(fd, 0, SEEK_SET) == 0 && ::ftruncate(fd, 0) == 0;
  if (ok) {
    size_t written = 0;
    while (written < rendered.size()) {
      ssize_t w = ::write(fd, rendered.data() + written, rendered.size() - written);
      if (w <= 0) { ok = false; break; }
      written += static_cast<size_t>(w);
    }
  }
  ::flock(fd, LOCK_UN);
  ::close(fd);
  return ok;
}

}  // namespace

void RecordCounter(const std::string& cache_dir, Counter counter) {
  WithLockedStats(cache_dir, [counter](Stats* stats) { stats->Add(counter); });
}

Stats ReadStats(const std::string& cache_dir) {
  auto text = util::ReadFile(StatsPath(cache_dir));
  if (!text) return Stats{};
  return ParseStats(*text);
}

bool ZeroStats(const std::string& cache_dir) {
  return WithLockedStats(cache_dir, [](Stats* stats) { *stats = Stats{}; });
}

std::string FormatStats(const Stats& stats, const std::string& cache_dir,
                        uint64_t cache_bytes, uint64_t max_bytes) {
  const uint64_t hits =
      stats.Get(Counter::kHitDisk) + stats.Get(Counter::kHitS3);
  const uint64_t misses = stats.Get(Counter::kMiss);
  const uint64_t lookups = hits + misses;

  std::ostringstream out;
  out << "cache directory     " << cache_dir << "\n";
  for (size_t i = 0; i < static_cast<size_t>(Counter::kCount); ++i) {
    out.width(20);
    out.setf(std::ios::left);
    out << kNames[i];
    out.unsetf(std::ios::left);
    out << stats.values[i] << "\n";
  }
  out.width(20);
  out.setf(std::ios::left);
  out << "hit rate";
  out.unsetf(std::ios::left);
  if (lookups == 0) {
    out << "n/a\n";
  } else {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f%%",
                  100.0 * static_cast<double>(hits) / static_cast<double>(lookups));
    out << buf << "\n";
  }

  auto human = [](uint64_t bytes) {
    static const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    int unit = 0;
    while (v >= 1024.0 && unit < 4) { v /= 1024.0; ++unit; }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.2f %s", v, kUnits[unit]);
    return std::string(buf);
  };
  out.width(20);
  out.setf(std::ios::left);
  out << "cache size";
  out.unsetf(std::ios::left);
  out << human(cache_bytes) << " / " << human(max_bytes) << "\n";
  return out.str();
}

}  // namespace vcache::core
