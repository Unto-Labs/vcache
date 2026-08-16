// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: GPL-3.0-only
// The rustc compile pipeline.
//
// Structure mirrors the C/C++ path, but the "what did this compilation read"
// step is different: instead of preprocessing, vcache asks rustc for
// `--emit=dep-info` and hashes every file it names, plus the contents of each
// --extern dependency.
//
// Outputs are captured by compiling into a temporary directory and recording
// everything that appears there, which avoids having to model rustc's naming
// rules for rlib/rmeta/dSYM artifacts.
#pragma once

#include <string>
#include <vector>

#include "core/config.h"
#include "core/roots.h"
#include "storage/chain.h"

namespace vcache::rust {

int RunRustCompile(const std::vector<std::string>& argv,
                   const core::Config& config, const core::RootMap& roots,
                   storage::CacheChain* cache);

}  // namespace vcache::rust
