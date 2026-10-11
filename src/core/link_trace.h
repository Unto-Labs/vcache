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
#include <string_view>
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

// The records of a tracer log by kind, each path made absolute against `cwd`
// and otherwise spelled as the traced process passed it.
struct TraceRecords {
  std::vector<std::string> reads;   // R
  std::vector<std::string> absent;  // M
  std::vector<std::string> tools;   // P
  std::vector<std::string> loaded;   // L: shared objects the processes loaded
  std::vector<std::string> written;  // W: opened for writing only
};

// False when the log cannot be read or any record is malformed, for the reason
// ParseTraceLog gives.
bool ReadTraceRecords(const std::string& log_path, const std::string& cwd, TraceRecords* out);

// True when the tracer could not record something and said so beside the log.
bool TraceMarkedIncomplete(const std::string& log_path);

// VCACHE_TRACER when set, else vcache-fstrace.so beside this binary; empty
// when this binary cannot locate itself.
std::string TracerPath();

// True when `tracer` is an ELF shared object of this binary's class, byte
// order and machine. ld.so refuses to preload anything else, and says so on
// the traced process's stderr, which a cached entry would replay.
bool TracerLoadsHere(const std::string& tracer);

// The message ld.so writes to stderr when it ignores an LD_PRELOAD entry.
inline constexpr std::string_view kPreloadIgnoredMessage = "from LD_PRELOAD cannot be preloaded";

// `stderr_text` without the lines carrying kPreloadIgnoredMessage. Sets
// `removed` when there was one: some process of the traced tree did not load
// the tracer, and a cached entry must not replay what preloading it caused.
std::string WithoutPreloadRefusals(const std::string& stderr_text, bool* removed);

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
