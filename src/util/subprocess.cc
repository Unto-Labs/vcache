// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "util/subprocess.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vcache::util {
namespace {

// Drains both pipes concurrently. Reading them one after the other would
// deadlock as soon as the child fills the pipe we are not reading.
void PumpPipes(int out_fd, int err_fd, std::string* out, std::string* err) {
  struct pollfd fds[2];
  int nfds = 0;
  if (out_fd >= 0) fds[nfds++] = {out_fd, POLLIN, 0};
  if (err_fd >= 0) fds[nfds++] = {err_fd, POLLIN, 0};

  char buf[65536];
  int open_count = nfds;
  while (open_count > 0) {
    if (::poll(fds, nfds, -1) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (int i = 0; i < nfds; ++i) {
      if (fds[i].fd < 0 || (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
        continue;
      }
      ssize_t n = ::read(fds[i].fd, buf, sizeof(buf));
      if (n > 0) {
        std::string* sink = (fds[i].fd == out_fd) ? out : err;
        if (sink != nullptr) sink->append(buf, static_cast<size_t>(n));
      } else if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
        fds[i].fd = -1;
        --open_count;
      }
    }
  }
}

uint64_t WallElapsedMs(const struct timespec& start, const struct timespec& end) {
  int64_t sec = static_cast<int64_t>(end.tv_sec) - static_cast<int64_t>(start.tv_sec);
  int64_t nsec = static_cast<int64_t>(end.tv_nsec) - static_cast<int64_t>(start.tv_nsec);
  if (nsec < 0) {
    --sec;
    nsec += 1000000000L;
  }
  if (sec < 0) return 0;
  return static_cast<uint64_t>(sec) * 1000u +
         static_cast<uint64_t>(nsec / 1000000L);
}

}  // namespace

uint64_t RssKbFromRuMaxrss(long ru_maxrss, bool ru_maxrss_is_bytes) {
  if (ru_maxrss <= 0) return 0;
  const auto raw = static_cast<uint64_t>(ru_maxrss);
  return ru_maxrss_is_bytes ? raw / 1024u : raw;
}

ProcResult Run(const std::vector<std::string>& argv, const ProcOptions& opts) {
  ProcResult result;
  if (argv.empty()) return result;

  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  const bool want_out_pipe = opts.capture_stdout && opts.stdout_file.empty();
  if (want_out_pipe && ::pipe(out_pipe) != 0) return result;
  if (opts.capture_stderr && ::pipe(err_pipe) != 0) {
    if (out_pipe[0] >= 0) { ::close(out_pipe[0]); ::close(out_pipe[1]); }
    return result;
  }

  // Wall time covers the whole child, including descendants the driver
  // waits for, not just the time spent in exec.
  struct timespec wall_start {};
  ::clock_gettime(CLOCK_MONOTONIC, &wall_start);

  pid_t pid = ::fork();
  if (pid < 0) {
    if (out_pipe[0] >= 0) { ::close(out_pipe[0]); ::close(out_pipe[1]); }
    if (err_pipe[0] >= 0) { ::close(err_pipe[0]); ::close(err_pipe[1]); }
    return result;
  }

  if (pid == 0) {
    // Child. Only async-signal-safe work from here to execvp.
    if (!opts.stdout_file.empty()) {
      int fd = ::open(opts.stdout_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd < 0) ::_exit(127);
      ::dup2(fd, STDOUT_FILENO);
      ::close(fd);
    } else if (want_out_pipe) {
      ::dup2(out_pipe[1], STDOUT_FILENO);
    }
    if (opts.capture_stderr) ::dup2(err_pipe[1], STDERR_FILENO);

    if (out_pipe[0] >= 0) { ::close(out_pipe[0]); ::close(out_pipe[1]); }
    if (err_pipe[0] >= 0) { ::close(err_pipe[0]); ::close(err_pipe[1]); }

    std::vector<char*> c_argv;
    c_argv.reserve(argv.size() + 1);
    for (const std::string& a : argv) c_argv.push_back(const_cast<char*>(a.c_str()));
    c_argv.push_back(nullptr);
    // setenv is not on the async-signal-safe list, but vcache is
    // single-threaded, so the child's heap cannot be observed mid-mutation by
    // another thread. The alternative, building an envp by hand, would have to
    // allocate here anyway.
    for (const auto& [name, value] : opts.env) {
      ::setenv(name.c_str(), value.c_str(), 1);
    }
    ::execvp(c_argv[0], c_argv.data());
    ::_exit(127);
  }

  // Parent.
  if (opts.on_spawn) opts.on_spawn(pid);
  if (out_pipe[1] >= 0) ::close(out_pipe[1]);
  if (err_pipe[1] >= 0) ::close(err_pipe[1]);
  PumpPipes(out_pipe[0], err_pipe[0], &result.stdout_data, &result.stderr_data);
  if (out_pipe[0] >= 0) ::close(out_pipe[0]);
  if (err_pipe[0] >= 0) ::close(err_pipe[0]);

  int status = 0;
  struct rusage usage {};
  while (::wait4(pid, &status, 0, &usage) < 0 && errno == EINTR) {
  }
  struct timespec wall_end {};
  ::clock_gettime(CLOCK_MONOTONIC, &wall_end);
  result.wall_ms = WallElapsedMs(wall_start, wall_end);
  // macOS counts bytes; Linux counts kibibytes. The field above is kibibytes.
#ifdef __APPLE__
  constexpr bool kRuMaxrssIsBytes = true;
#else
  constexpr bool kRuMaxrssIsBytes = false;
#endif
  result.max_rss_kb = RssKbFromRuMaxrss(usage.ru_maxrss, kRuMaxrssIsBytes);
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.signalled = true;
    result.exit_code = 128 + WTERMSIG(status);
  }
  return result;
}

}  // namespace vcache::util
