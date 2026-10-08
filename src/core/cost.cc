// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "core/cost.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "hash/hasher.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/str.h"

namespace vcache::core {
namespace {

constexpr size_t kMaxCostObservations = 8;
constexpr size_t kLargestCostKeys = 10;

// Cargo rewrites `-C metadata` and `-C extra-filename` on a lockfile or feature
// change. Keeping every flag that is not a path would split one crate's cost
// estimate across those hashes. The class is therefore an allow-list of the
// flags that actually change how much memory the compiler uses.
bool IsKeptCxxFlag(std::string_view arg) {
  if (util::StartsWith(arg, "-O") || util::StartsWith(arg, "-g") ||
      util::StartsWith(arg, "-march=") || util::StartsWith(arg, "-mtune=") ||
      util::StartsWith(arg, "-std=")) {
    return true;
  }
  if (!util::StartsWith(arg, "-f")) return false;
  if (util::StartsWith(arg, "-fdebug-prefix-map") ||
      util::StartsWith(arg, "-ffile-prefix-map") ||
      util::StartsWith(arg, "-fmacro-prefix-map")) {
    return false;
  }
  return true;
}

bool IsKeptRustCValue(std::string_view value) {
  return util::StartsWith(value, "opt-level") || util::StartsWith(value, "debuginfo") ||
         util::StartsWith(value, "codegen-units") || util::StartsWith(value, "lto") ||
         util::StartsWith(value, "target-cpu");
}

bool IsKeptLinkFlag(std::string_view arg) {
  return util::StartsWith(arg, "-O") || util::StartsWith(arg, "-flto") ||
         util::StartsWith(arg, "-fuse-ld=");
}

// `--edition`, `--crate-type` and `--target`, either joined (`--edition=2021`)
// or as the flag plus its following argument. `--target-cpu` does not match
// `--target`: the next character would be `-`, not `=`.
bool TakeNamedFlag(const std::vector<std::string>& args, size_t* i, std::string_view name,
                   std::vector<std::string>* kept) {
  const std::string& arg = args[*i];
  if (arg == name) {
    kept->push_back(arg);
    if (*i + 1 < args.size()) kept->push_back(args[++*i]);
    return true;
  }
  if (util::StartsWith(arg, std::string(name) + "=")) {
    kept->push_back(arg);
    return true;
  }
  return false;
}

std::vector<std::string> CostFlagClass(std::string_view operation,
                                       const std::vector<std::string>& key_args) {
  std::vector<std::string> kept;
  for (size_t i = 0; i < key_args.size(); ++i) {
    const std::string& arg = key_args[i];
    if (operation == "rustc") {
      if (arg == "-C" && i + 1 < key_args.size() && IsKeptRustCValue(key_args[i + 1])) {
        kept.push_back(arg);
        kept.push_back(key_args[++i]);
        continue;
      }
      if (arg.size() > 2 && util::StartsWith(arg, "-C") &&
          IsKeptRustCValue(std::string_view(arg).substr(2))) {
        kept.push_back(arg);
        continue;
      }
      if (TakeNamedFlag(key_args, &i, "--edition", &kept) ||
          TakeNamedFlag(key_args, &i, "--crate-type", &kept) ||
          TakeNamedFlag(key_args, &i, "--target", &kept)) {
        continue;
      }
      continue;
    }
    if (operation == "link") {
      if (IsKeptLinkFlag(arg)) kept.push_back(arg);
      continue;
    }
    if (IsKeptCxxFlag(arg)) kept.push_back(arg);
  }
  return kept;
}

std::optional<uint64_t> ParseU64(std::string_view text) {
  if (text.empty()) return std::nullopt;
  uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10u) return std::nullopt;
    value = value * 10u + digit;
  }
  return value;
}

bool ParseObservation(std::string_view line, CompileCost::Observation* out) {
  std::string_view fields[3];
  size_t count = 0;
  size_t i = 0;
  while (i < line.size() && count < 3) {
    while (i < line.size() && line[i] == ' ') ++i;
    if (i >= line.size()) break;
    const size_t start = i;
    while (i < line.size() && line[i] != ' ') ++i;
    fields[count++] = line.substr(start, i - start);
  }
  while (i < line.size() && line[i] == ' ') ++i;
  if (count != 3 || i != line.size()) return false;
  const auto rss = ParseU64(fields[0]);
  const auto wall = ParseU64(fields[1]);
  const auto when = ParseU64(fields[2]);
  if (!rss || !wall || !when) return false;
  out->max_rss_kb = *rss;
  out->wall_ms = *wall;
  out->recorded_at_unix = *when;
  return true;
}

bool TakeHeader(std::string_view line, std::string_view key, std::string* dest,
                bool* seen) {
  const std::string prefix = std::string(key) + " ";
  if (line != key && !util::StartsWith(line, prefix)) return false;
  if (*seen) return false;
  *seen = true;
  *dest = line == key ? std::string() : std::string(line.substr(prefix.size()));
  return true;
}

// Empty means "no usable record". A file that does not parse is the same as
// a file that is not there: the next record rewrites it.
CompileCost ParseCostFile(const std::string& text) {
  CompileCost cost;
  bool saw_op = false;
  bool saw_language = false;
  bool saw_source = false;
  size_t begin = 0;
  while (begin <= text.size()) {
    size_t end = text.find('\n', begin);
    if (end == std::string::npos) end = text.size();
    const std::string_view line(text.data() + begin, end - begin);
    begin = end + 1;
    if (line.empty()) {
      if (end == text.size()) break;
      continue;
    }
    if (TakeHeader(line, "op", &cost.operation, &saw_op) ||
        TakeHeader(line, "language", &cost.language, &saw_language) ||
        TakeHeader(line, "source", &cost.source, &saw_source)) {
      if (end == text.size()) break;
      continue;
    }
    CompileCost::Observation observation;
    if (!ParseObservation(line, &observation)) return {};
    cost.observations.push_back(observation);
    if (end == text.size()) break;
  }
  if (!saw_op || !saw_language || !saw_source) return {};
  return cost;
}

std::string RenderCostFile(const CompileCost& cost) {
  std::string out;
  out += "op " + cost.operation + "\n";
  out += "language " + cost.language + "\n";
  out += "source " + cost.source + "\n";
  for (const CompileCost::Observation& observation : cost.observations) {
    out += std::to_string(observation.max_rss_kb);
    out += " ";
    out += std::to_string(observation.wall_ms);
    out += " ";
    out += std::to_string(observation.recorded_at_unix);
    out += "\n";
  }
  return out;
}

std::string CostDir(const std::string& cache_dir) { return cache_dir + "/costs"; }

std::string CostPath(const std::string& cache_dir, const std::string& cost_key) {
  return CostDir(cache_dir) + "/" + cost_key;
}

uint64_t Percentile50(std::vector<uint64_t> values) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  return values[(values.size() - 1) / 2];
}

uint64_t MaxOf(const std::vector<uint64_t>& values) {
  uint64_t max = 0;
  for (uint64_t value : values) max = std::max(max, value);
  return max;
}

std::string OperationRow(const char* operation, const std::vector<uint64_t>& rss,
                         const std::vector<uint64_t>& wall) {
  std::string line = operation;
  line += " records ";
  line += std::to_string(rss.size());
  line += " max_rss_kb p50=";
  line += std::to_string(Percentile50(rss));
  line += " max=";
  line += std::to_string(MaxOf(rss));
  line += " wall_ms p50=";
  line += std::to_string(Percentile50(wall));
  line += " max=";
  line += std::to_string(MaxOf(wall));
  line += "\n";
  return line;
}

}  // namespace

std::string ComputeCostKey(std::string_view operation, const std::string& source_path,
                           std::string_view language,
                           const std::vector<std::string>& key_args, const RootMap& roots) {
  hash::Hasher hasher;
  hasher.UpdateDelimited(operation);
  // A relative source is spelled against the cwd the compiler saw. Without
  // making it absolute first, Canonicalize cannot see that it sits under a root.
  hasher.UpdateDelimited(roots.Canonicalize(util::AbsoluteLexical(source_path)));
  hasher.UpdateDelimited(language);
  for (const std::string& flag : CostFlagClass(operation, key_args)) {
    hasher.UpdateDelimited(flag);
  }
  return hasher.Hex();
}

CompileCost LoadCompileCost(const std::string& cache_dir, const std::string& cost_key) {
  auto text = util::ReadFile(CostPath(cache_dir, cost_key));
  if (!text) return {};
  return ParseCostFile(*text);
}

void RecordCompileCost(const std::string& cache_dir, std::string_view operation,
                       const std::string& source_path, std::string_view language,
                       const std::vector<std::string>& key_args, const RootMap& roots,
                       const util::ProcResult& proc) {
  const std::string canonical = roots.Canonicalize(util::AbsoluteLexical(source_path));
  const std::string key = ComputeCostKey(operation, source_path, language, key_args, roots);
  const std::string dir = CostDir(cache_dir);

  auto fail = [](const std::string& why) {
    VCACHE_LOG("cost: could not record (" + why + ")");
  };

  if (!util::MakeDirs(dir)) {
    fail(dir + ": " + std::strerror(errno));
    return;
  }

  CompileCost cost = LoadCompileCost(cache_dir, key);
  cost.operation = std::string(operation);
  cost.language = std::string(language);
  cost.source = canonical;

  const std::time_t now = std::time(nullptr);
  CompileCost::Observation observation;
  observation.max_rss_kb = proc.max_rss_kb;
  observation.wall_ms = proc.wall_ms;
  observation.recorded_at_unix = now < 0 ? 0 : static_cast<uint64_t>(now);
  cost.observations.push_back(observation);
  if (cost.observations.size() > kMaxCostObservations) {
    cost.observations.erase(cost.observations.begin(),
                            cost.observations.end() - kMaxCostObservations);
  }

  const std::string path = CostPath(cache_dir, key);
  // A lost cost file is a missing estimate, not a corrupt object, so skip the
  // fsync WriteFileAtomic does for cache entries.
  if (!util::WriteFileAtomic(path, RenderCostFile(cost), /*durable=*/false)) {
    fail(path + ": " + std::strerror(errno));
    return;
  }

  std::string line = "cost: ";
  line += operation;
  line += " max_rss_kb=";
  line += std::to_string(proc.max_rss_kb);
  line += " wall_ms=";
  line += std::to_string(proc.wall_ms);
  line += " key=";
  line += key.substr(0, std::min<size_t>(16, key.size()));
  line += " ";
  line += canonical;
  if (proc.signalled || proc.exit_code != 0) {
    line += " exit=";
    line += std::to_string(proc.exit_code);
  }
  VCACHE_LOG(line);
}

std::optional<uint64_t> EstimateMaxRssKb(const std::string& cache_dir,
                                         const std::string& cost_key) {
  const CompileCost cost = LoadCompileCost(cache_dir, cost_key);
  if (cost.observations.empty()) return std::nullopt;
  uint64_t max = 0;
  for (const CompileCost::Observation& observation : cost.observations) {
    max = std::max(max, observation.max_rss_kb);
  }
  return max;
}

void AppendCostMeta(std::string* meta, uint64_t max_rss_kb, uint64_t wall_ms) {
  if (meta == nullptr) return;
  if (!meta->empty() && meta->back() != '\n') meta->push_back('\n');
  *meta += "max_rss_kb: " + std::to_string(max_rss_kb) + "\n";
  *meta += "wall_ms: " + std::to_string(wall_ms) + "\n";
}

std::string FormatCosts(const std::string& cache_dir) {
  std::vector<uint64_t> compile_rss, compile_wall, rustc_rss, rustc_wall, link_rss, link_wall;
  struct Ranked {
    uint64_t max_rss_kb = 0;
    std::string key;
    std::string source;
  };
  std::vector<Ranked> ranked;

  const std::string dir = CostDir(cache_dir);
  if (util::IsDirectory(dir)) {
    for (const util::FileEntry& entry : util::ListFilesRecursive(dir)) {
      auto text = util::ReadFile(entry.path);
      if (!text) continue;
      const CompileCost cost = ParseCostFile(*text);
      if (cost.observations.empty()) continue;
      std::vector<uint64_t>* rss = nullptr;
      std::vector<uint64_t>* wall = nullptr;
      if (cost.operation == "compile") {
        rss = &compile_rss;
        wall = &compile_wall;
      } else if (cost.operation == "rustc") {
        rss = &rustc_rss;
        wall = &rustc_wall;
      } else if (cost.operation == "link") {
        rss = &link_rss;
        wall = &link_wall;
      }
      uint64_t max_rss = 0;
      for (const CompileCost::Observation& observation : cost.observations) {
        max_rss = std::max(max_rss, observation.max_rss_kb);
        if (rss != nullptr) {
          rss->push_back(observation.max_rss_kb);
          wall->push_back(observation.wall_ms);
        }
      }
      ranked.push_back({max_rss, util::BaseName(entry.path), cost.source});
    }
  }

  std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
    if (a.max_rss_kb != b.max_rss_kb) return a.max_rss_kb > b.max_rss_kb;
    return a.key < b.key;
  });
  if (ranked.size() > kLargestCostKeys) ranked.resize(kLargestCostKeys);

  std::string out;
  out += OperationRow("compile", compile_rss, compile_wall);
  out += OperationRow("rustc", rustc_rss, rustc_wall);
  out += OperationRow("link", link_rss, link_wall);
  out += "largest:\n";
  for (const Ranked& item : ranked) {
    out += item.key;
    out += " max_rss_kb=";
    out += std::to_string(item.max_rss_kb);
    out += " ";
    out += item.source;
    out += "\n";
  }
  return out;
}

void PruneStaleCostFiles(const std::string& cache_dir) {
  constexpr uint64_t kCostFileMaxAgeSeconds = 30ull * 24 * 60 * 60;
  const std::string dir = CostDir(cache_dir);
  if (!util::IsDirectory(dir)) return;
  const std::time_t now_clock = std::time(nullptr);
  if (now_clock < 0) return;
  const uint64_t now = static_cast<uint64_t>(now_clock);
  for (const util::FileEntry& entry : util::ListFilesRecursive(dir)) {
    auto text = util::ReadFile(entry.path);
    if (!text) continue;
    const CompileCost cost = ParseCostFile(*text);
    if (cost.observations.empty()) continue;
    const uint64_t newest = cost.observations.back().recorded_at_unix;
    if (newest >= now) continue;
    if (now - newest > kCostFileMaxAgeSeconds) util::RemoveFile(entry.path);
  }
}

}  // namespace vcache::core
