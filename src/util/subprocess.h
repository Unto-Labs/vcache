// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// Process execution. vcache never goes through a shell: argv vectors are passed
// straight to execvp so paths containing spaces or quotes cannot be misparsed.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace vcache::util {

struct ProcResult {
  int exit_code = -1;
  bool signalled = false;   // true when the child died from a signal
  std::string stdout_data;  // empty unless capture_stdout was requested
  std::string stderr_data;  // empty unless capture_stderr was requested

  // Peak resident set of the waited-for child, in kibibytes, and wall time
  // from just before fork to just after wait. Linux reports ru_maxrss in
  // kibibytes already; macOS reports bytes. Both land here as kibibytes.
  // For a compiler driver this is the largest descendant it waited for
  // (cc1/cc1plus, or the linker and its LTO jobs), which is the peak that
  // matters. A spawn that never happened leaves both at zero.
  uint64_t max_rss_kb = 0;
  uint64_t wall_ms = 0;
};

struct ProcOptions {
  bool capture_stdout = false;
  bool capture_stderr = false;
  // When set, the child's stdout is redirected into this file instead of being
  // captured in memory. Used for large preprocessor output.
  std::string stdout_file;

  // Variables to set in the child only. The link tracer needs LD_PRELOAD and
  // its log path to reach the linker's whole process tree without vcache's own
  // environment being modified, which would leak into anything else it runs.
  std::vector<std::pair<std::string, std::string>> env;
  std::function<void(int)> on_spawn;
};

// Runs argv[0] with `argv`. Returns exit_code == -1 if the process could not be
// spawned at all.
ProcResult Run(const std::vector<std::string>& argv, const ProcOptions& opts = {});

// Converts a wait4 ru_maxrss into kibibytes. `ru_maxrss_is_bytes` is true on
// macOS, where the kernel counts bytes, and false on Linux, where the same
// field is already kibibytes. A non-positive reading is zero.
uint64_t RssKbFromRuMaxrss(long ru_maxrss, bool ru_maxrss_is_bytes);

}  // namespace vcache::util
