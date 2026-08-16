// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: GPL-3.0-only
#include "util/log.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>

namespace vcache::util {
namespace {

std::string* g_log_path = nullptr;
bool g_to_stderr = false;
bool g_enabled = false;
std::mutex g_mutex;

std::string Timestamp() {
  struct timespec ts;
  ::clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tm_buf;
  ::localtime_r(&ts.tv_sec, &tm_buf);
  char buf[64];
  size_t n = ::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
  return std::string(buf, n) + "." +
         std::to_string(ts.tv_nsec / 1000000);
}

}  // namespace

void InitLogging() {
  const char* env = std::getenv("VCACHE_LOG");
  if (env == nullptr || *env == '\0') {
    g_enabled = false;
    return;
  }
  g_enabled = true;
  if (std::string(env) == "stderr") {
    g_to_stderr = true;
  } else {
    g_log_path = new std::string(env);
  }
}

bool LoggingEnabled() { return g_enabled; }

void LogLine(const std::string& message) {
  if (!g_enabled) return;
  // The pid prefix keeps parallel-make logs readable when many vcache
  // processes append to the same file.
  const std::string line =
      Timestamp() + " [" + std::to_string(::getpid()) + "] " + message + "\n";

  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_to_stderr) {
    ::fwrite(line.data(), 1, line.size(), stderr);
    return;
  }
  // Reopened per line in append mode: concurrent vcache processes each write
  // whole lines without needing a shared lock across processes.
  FILE* f = ::fopen(g_log_path->c_str(), "a");
  if (f == nullptr) return;
  ::fwrite(line.data(), 1, line.size(), f);
  ::fclose(f);
}

void LogKV(const std::string& key, const std::string& value) {
  LogLine(key + ": " + value);
}

}  // namespace vcache::util
