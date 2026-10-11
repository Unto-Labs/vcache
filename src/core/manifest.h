// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// Pieces shared by the lookups that are verified against a manifest of files
// read on an earlier run rather than keyed on everything they read: the -M/-MM
// dependency scan and the Rust dep-info step.
//
// A manifest maps one key to several remembered states. Each state lists the
// files a previous run read with their digests, and is only trusted while every
// one of them still hashes the same. Nothing is trusted on mtime.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/roots.h"

namespace vcache::core {

// Canonical path, content digest.
using ManifestFile = std::pair<std::string, std::string>;

// How many states one manifest remembers. The key cannot include what it has
// not read yet, so one key has several possible answers, one per set of file
// contents seen. Without that, alternating between two branches would miss
// every time: the state for the branch you just left is the one that gets
// overwritten. Eight covers branch-switching; the cost of a stale tail entry is
// one wasted hash pass.
constexpr size_t kMaxManifestStates = 8;

// Returns nullopt while every file, read at its localised path, still hashes to
// its recorded digest; otherwise says which file is gone or changed.
std::optional<std::string> FindStaleManifestFile(const std::vector<ManifestFile>& files,
                                                 const RootMap& roots);

// "<hex digest> <canonical path>". The digest comes first so a path containing
// spaces still parses by taking the rest of the line.
std::string RenderManifestFile(const ManifestFile& file);
bool ParseManifestFile(std::string_view line, ManifestFile* file);

// Puts `fresh` first and keeps the older states behind it, dropping any that
// `fresh` supersedes (same `key` member) and anything past the cap. Newest
// first, so the states in active use stay ahead of the tail that gets dropped,
// and lookup checks the likely match before hashing for others.
template <typename State>
std::vector<State> PrependManifestState(State fresh, std::vector<State> older,
                                        std::string State::*key) {
  std::vector<State> updated;
  updated.push_back(std::move(fresh));
  for (State& state : older) {
    if (state.*key == updated.front().*key) continue;
    if (updated.size() >= kMaxManifestStates) break;
    updated.push_back(std::move(state));
  }
  return updated;
}

// Re-read wins over the list loaded at lookup. `fresh` is first, then the
// re-read order, then states that existed only in the loaded list. One copy
// of each key. The cap is the same one PrependManifestState applies.
template <typename State>
std::vector<State> MergeManifestStates(State fresh, std::vector<State> reread,
                                       const std::vector<State>& loaded,
                                       std::string State::*key) {
  for (const State& state : loaded) {
    if (state.*key == fresh.*key) continue;
    bool seen = false;
    for (const State& have : reread) {
      if (have.*key == state.*key) {
        seen = true;
        break;
      }
    }
    if (!seen) reread.push_back(state);
  }
  return PrependManifestState(std::move(fresh), std::move(reread), key);
}

// Blocks on the fifo in VCACHE_TEST_PAUSE_BEFORE_MANIFEST_PUT when that
// variable names one, for at most 60 seconds. A missing path, a regular file
// or a directory is ignored, so a stale setting cannot stop the build. The
// read is the re-read's predecessor, so a test can store another state before
// this process merges.
void PauseBeforeManifestPut();

}  // namespace vcache::core
