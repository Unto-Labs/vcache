// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// Allocates <MiB> mebibytes, touches every page, and exits. Used by the
// util::Run rusage test: ru_maxrss is a high-water mark, so the pages have
// to be faulted in or the kernel reports a near-empty process.
#include <cstddef>
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: vcache_test_alloc <MiB>\n";
    return 2;
  }
  char* end = nullptr;
  const long mib = std::strtol(argv[1], &end, 10);
  if (end == argv[1] || *end != '\0' || mib <= 0) return 2;
  const size_t bytes = static_cast<size_t>(mib) << 20;
  // volatile: without it the compiler deletes the stores, malloc never faults
  // a page, and ru_maxrss stays the size of the process image.
  auto* pages = static_cast<volatile unsigned char*>(std::malloc(bytes));
  if (pages == nullptr) return 1;
  for (size_t i = 0; i < bytes; i += 4096) pages[i] = 1;
  pages[bytes - 1] = 1;
  if (pages[0] == 0xff) return 3;
  return 0;
}
