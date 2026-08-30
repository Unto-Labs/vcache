// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// Caching the link step.
//
// A link is a pure function of its inputs -- measured, not assumed: two LTO
// links of identical inputs produce a byte-identical binary despite -flto=auto
// fanning out to parallel backends. What makes it hard to cache is not
// determinism but that the input set is discovered rather than declared.
//
// So the key is built in two stages, the same shape the dependency-scan path
// already uses:
//
//   pre-key   what the command line says: toolchain identity, key-affecting
//             flags, and the digests of the inputs named explicitly.
//   manifest  what the previous run actually touched: every file read, every
//             path probed and absent, every executable that ran.
//
// A hit requires both that every recorded input still hashes the same AND that
// every recorded absent path is still absent. The second is not optional:
// -lfoo resolved to /usr/lib/libfoo.so only because /opt/lib/libfoo.so was
// missing, and an entry that records only what it read would hit after someone
// installs it and hand back a binary linked against the wrong library.
//
// Validating the absent set is cheap enough that no cleverness is warranted:
// re-statting all 243 absent paths from a real LTO link takes 0.45 ms against a
// 2270 ms link.
#pragma once

#include <string>
#include <vector>

#include "core/config.h"
#include "core/roots.h"
#include "storage/chain.h"

namespace vcache::core {

// Handles `argv` as a link. Returns the exit code to propagate.
//
// `handled` is set false when the command line is not a link at all, which
// leaves the caller free to try the compile path instead. A link vcache
// declines to cache is still `handled` -- it runs the linker and returns.
int RunLink(const std::vector<std::string>& argv, const Config& config,
            const RootMap& roots, storage::CacheChain* cache, bool* handled);

}  // namespace vcache::core
