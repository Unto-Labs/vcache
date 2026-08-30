// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// Turning a tracer log into the two sets a link cache entry needs.
//
// The tracer is deliberately indiscriminate -- it reports every path the
// process tree touched. Most of that is noise for caching purposes: the output
// being written, temporary files, /proc, locale data. Classification is where
// the noise is dropped, and it is security-relevant as well as correctness-
// relevant: a path wrongly kept as an input makes entries invalidate for no
// reason, and a path wrongly dropped makes a hit unsound.
#pragma once

#include <string>
#include <vector>

namespace vcache::core {

struct LinkTrace {
  // Paths read, whose content is an input. Sorted and deduplicated.
  std::vector<std::string> inputs;

  // Paths probed and absent, whose continued absence is an input.
  std::vector<std::string> absent;

  // Executables that ran, whose content is part of the toolchain identity.
  std::vector<std::string> tools;
};

// Parses a tracer log. `outputs` are the files this link writes; they are
// excluded from every set, since an output is not an input to itself.
// `cwd` resolves relative paths as the tracer recorded them.
//
// Returns false when the log cannot be read or any record is malformed. A
// partially readable trace is not a usable input set: the missing record may be
// the only evidence that a later hit should be invalidated.
bool ParseTraceLog(const std::string& log_path,
                   const std::vector<std::string>& outputs,
                   const std::string& cwd, LinkTrace* out);

// True for a path whose state must not enter a cache key: process and kernel
// state, temporary directories, locale and gconv data, the terminal.
//
// Exposed for testing, because the exclusion list is exactly the part where a
// mistake is silent in both directions.
bool IsIgnoredPath(const std::string& path);

}  // namespace vcache::core
