// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// Allocates <MiB> mebibytes, touches every page, and exits. Used by the
// util::Run rusage test: ru_maxrss is a high-water mark, so the pages have
// to be faulted in or the kernel reports a near-empty process.
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 2 && argc != 5 && argc != 6) {
    std::cerr << "usage: vcache_test_alloc <MiB>\n";
    return 2;
  }
  char* end = nullptr;
  const long mib = std::strtol(argv[1], &end, 10);
  if (end == argv[1] || *end != '\0' || mib <= 0) return 2;
  const std::string mode = argc == 6 ? argv[5] : "";
  if (mode == "parallel") {
    int ready[2];
    if (::pipe(ready) != 0) return 2;
    std::vector<pid_t> children;
    for (int index = 0; index < 2; ++index) {
      const pid_t child = ::fork();
      if (child < 0) return 2;
      if (child == 0) {
        ::close(ready[0]);
        const size_t bytes = static_cast<size_t>(mib) << 20;
        auto* pages = static_cast<volatile unsigned char*>(std::malloc(bytes));
        if (!pages) ::_exit(2);
        for (size_t page = 0; page < bytes; page += 4096) pages[page] = 1;
        ::write(ready[1], "R", 1);
        pollfd stop{std::atoi(argv[4]), POLLIN, 0};
        ::poll(&stop, 1, 10000);
        ::_exit(pages[0] == 1 ? 0 : 2);
      }
      children.push_back(child);
    }
    ::close(ready[1]);
    for (int index = 0; index < 2; ++index) {
      pollfd child_ready{ready[0], POLLIN, 0};
      char byte = 0;
      if (::poll(&child_ready, 1, 1000) <= 0 || ::read(ready[0], &byte, 1) != 1) return 2;
    }
    ::close(ready[0]);
    ::write(std::atoi(argv[3]), "R", 1);
    for (const pid_t child : children) ::waitpid(child, nullptr, 0);
    return 0;
  }
  if (mode == "zombie") {
    const pid_t zombie = ::fork();
    if (zombie < 0) return 2;
    if (zombie == 0) ::_exit(0);
    siginfo_t exited{};
    if (::waitid(P_PID, zombie, &exited, WEXITED | WNOWAIT) != 0) return 2;
  }
  if (argc >= 5) {
    const int depth = std::atoi(argv[2]);
    if (depth < 0 || depth > 2) return 2;
    for (int level = 0; level < depth; ++level) {
      const pid_t child = ::fork();
      if (child < 0) return 2;
      if (child > 0) {
        int status = 0;
        ::waitpid(child, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 2;
      }
    }
  }
  const size_t bytes = static_cast<size_t>(mib) << 20;
  // volatile: without it the compiler deletes the stores, malloc never faults
  // a page, and ru_maxrss stays the size of the process image.
  auto* pages = static_cast<volatile unsigned char*>(std::malloc(bytes));
  if (pages == nullptr) return 1;
  for (size_t i = 0; i < bytes; i += 4096) pages[i] = 1;
  pages[bytes - 1] = 1;
  if (argc >= 5) {
    ::write(std::atoi(argv[3]), "R", 1);
    pollfd stop{std::atoi(argv[4]), POLLIN, 0};
    std::vector<void*> extra;
    while (::poll(&stop, 1, 10000) > 0 && mode == "grow") {
      char byte = 0;
      if (::read(stop.fd, &byte, 1) != 1) break;
      if (byte == 'S') {
        for (void* memory : extra) std::free(memory);
        extra.clear();
      } else if (byte == 'G') {
        void* memory = std::malloc(bytes);
        if (!memory) return 2;
        auto* grown = static_cast<volatile unsigned char*>(memory);
        for (size_t page = 0; page < bytes; page += 4096) grown[page] = 1;
        extra.push_back(memory);
      } else {
        break;
      }
    }
  }
  if (pages[0] == 0xff) return 3;
  return 0;
}
