// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// Debug logging, off unless VCACHE_LOG is set. The log is the primary tool for
// diagnosing "why did this miss", so every cache decision writes a line.
#pragma once

#include <chrono>
#include <string>

namespace vcache::util {

// Reads VCACHE_LOG: a file path, or "stderr". Called once from main().
void InitLogging();

bool LoggingEnabled();

void LogLine(const std::string& message);

// Convenience for the common "key: value" shape.
void LogKV(const std::string& key, const std::string& value);

// Monotonic milliseconds since `started`, to three decimals. Lines that carry
// it are summed over a whole build, where thousands of sub-millisecond spans
// printed as 0 would add up to nothing.
std::string MillisecondsSince(std::chrono::steady_clock::time_point started);

}  // namespace vcache::util

// Guarded so argument formatting is skipped entirely when logging is off.
#define VCACHE_LOG(expr)                                  \
  do {                                                    \
    if (::vcache::util::LoggingEnabled()) {               \
      ::vcache::util::LogLine((expr));                    \
    }                                                     \
  } while (0)
