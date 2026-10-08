// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// Peak-RSS and wall-time observations for compiles and links.
//
// The cache key answers "is this the same compilation?". This answers "how
// heavy was it?", so a later admission decision can reserve memory before
// the compiler is spawned. Content is deliberately not an input: a miss on
// new text of the same file should still have an estimate. The record is
// informational and is not mixed into any cache key.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/roots.h"
#include "util/subprocess.h"

namespace vcache::core {

// One cost file: the identity of the work plus the last few runs of it.
// Observations are oldest first, newest last.
struct CompileCost {
  std::string operation;  // "compile", "rustc" or "link"
  std::string source;     // canonical source, or the link output path
  std::string language;

  struct Observation {
    uint64_t max_rss_kb = 0;
    uint64_t wall_ms = 0;
    uint64_t recorded_at_unix = 0;
  };
  std::vector<Observation> observations;
};

// BLAKE3 of the operation, the canonical source (or link output) path, the
// language, and the codegen flag class. Include paths, defines and path-like
// arguments are left out, so two checkouts of one file share an estimate.
std::string ComputeCostKey(std::string_view operation, const std::string& source_path,
                           std::string_view language,
                           const std::vector<std::string>& key_args, const RootMap& roots);

// Reads `<cache>/costs/<cost_key>`. A missing or unparseable file is an empty
// record, which the next successful write replaces.
CompileCost LoadCompileCost(const std::string& cache_dir, const std::string& cost_key);

// Appends one observation and keeps the last eight. A failure to write is
// logged and swallowed: losing a statistic must not fail the build. Two
// processes recording the same key can lose one observation, because the
// update is a single atomic replace with no lock.
void RecordCompileCost(const std::string& cache_dir, std::string_view operation,
                       const std::string& source_path, std::string_view language,
                       const std::vector<std::string>& key_args, const RootMap& roots,
                       const util::ProcResult& proc);

// Highest recorded max_rss_kb, or nullopt when there is no usable observation.
// The caller supplies the per-operation default for the absent case.
std::optional<uint64_t> EstimateMaxRssKb(const std::string& cache_dir,
                                         const std::string& cost_key);

// Appends the two meta lines a stored blob carries. Meta is not part of a key.
void AppendCostMeta(std::string* meta, uint64_t max_rss_kb, uint64_t wall_ms);

// Text for `vcache --show-costs`.
std::string FormatCosts(const std::string& cache_dir);

// Deletes cost files whose newest observation is more than 30 days old.
// Called from an explicit trim, not from the trim a store may trigger.
void PruneStaleCostFiles(const std::string& cache_dir);

}  // namespace vcache::core
