// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "util/fs.h"

#include <fcntl.h>
#include <vector>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "util/str.h"

namespace fs = std::filesystem;

namespace vcache::util {
namespace {

// The mode a normally-created file gets: 0666 masked by the process umask,
// which is what fopen, gcc and rustc all end up with. Read once -- umask(2)
// only reads by setting, so the value is restored immediately. vcache is
// single-threaded and spawns the compiler well after this runs, so the brief
// window cannot leak a zero umask into a child.
}  // namespace

mode_t DefaultFileMode() {
  static const mode_t kMode = [] {
    const mode_t previous = ::umask(0);
    ::umask(previous);
    return static_cast<mode_t>(0666 & ~previous);
  }();
  return kMode;
}

namespace {

// std::filesystem throws on many operations; every call here goes through a
// noexcept wrapper so a surprising errno never aborts the user's build.
template <typename Fn>
auto Guarded(Fn fn, decltype(fn()) fallback) -> decltype(fn()) {
  std::error_code ec;
  (void)ec;
  try {
    return fn();
  } catch (const std::exception&) {
    return fallback;
  }
}

}  // namespace

bool FileExists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

bool IsDirectory(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool MakeDirs(const std::string& path) {
  if (path.empty()) {
    errno = EINVAL;
    return false;
  }
  if (IsDirectory(path)) return true;
  std::error_code ec;
  fs::create_directories(path, ec);
  if (IsDirectory(path)) return true;
  // std::filesystem reports through the error_code, leaving errno to whatever
  // ran last. Callers print strerror(errno), so translate before returning.
  errno = ec ? ec.value() : ENOENT;
  return false;
}

std::optional<std::string> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::string contents((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  if (in.bad()) return std::nullopt;
  return contents;
}

bool WriteFileAtomic(const std::string& path, std::string_view contents) {
  const std::string dir = DirName(path);
  if (!dir.empty() && !MakeDirs(dir)) return false;

  std::string tmpl = dir.empty() ? std::string(".vcache-tmp-XXXXXX")
                                 : dir + "/.vcache-tmp-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  int fd = ::mkstemp(buf.data());
  if (fd < 0) return false;
  const std::string tmp_path(buf.data());

  // mkstemp always creates at 0600, which is right for a private temp file and
  // wrong for everything vcache goes on to rename into place: a replayed object
  // must look exactly like one the compiler wrote, and the compiler writes at
  // 0666 & ~umask. The difference is invisible while one uid both writes and
  // reads, and breaks the moment they differ -- a container building as root
  // and a CI user hashing the results afterwards is the case that found this.
  if (::fchmod(fd, DefaultFileMode()) != 0) {
    ::close(fd);
    ::unlink(tmp_path.c_str());
    return false;
  }

  // Callers report std::strerror(errno) when this returns false, so every
  // failure path must hand back the errno of the call that actually failed.
  // close(), unlink() and rename() all run after the interesting failure and
  // are free to overwrite it -- which is how a write that failed with EACCES
  // came to be reported as ENOENT.
  int saved_errno = 0;
  size_t written = 0;
  bool ok = true;
  while (written < contents.size()) {
    ssize_t n = ::write(fd, contents.data() + written, contents.size() - written);
    if (n <= 0) {
      if (errno == EINTR) continue;
      saved_errno = errno;
      ok = false;
      break;
    }
    written += static_cast<size_t>(n);
  }
  // Cache entries must survive a crash intact, so flush before the rename.
  if (ok && ::fsync(fd) != 0) {
    saved_errno = errno;
    ok = false;
  }
  ::close(fd);

  if (ok && ::rename(tmp_path.c_str(), path.c_str()) != 0) {
    saved_errno = errno;
    ok = false;
  }
  if (!ok) {
    ::unlink(tmp_path.c_str());
    errno = saved_errno;
    return false;
  }
  return true;
}

bool LinkOrCopy(const std::string& from, const std::string& to) {
  ::unlink(to.c_str());
  if (::link(from.c_str(), to.c_str()) == 0) return true;

  auto data = ReadFile(from);
  if (!data) return false;
  return WriteFileAtomic(to, *data);
}

bool CloneFile(const std::string& from, const std::string& to) {
  const int src = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
  if (src < 0) return false;

  struct stat st;
  if (::fstat(src, &st) != 0) { ::close(src); return false; }

  const std::string tmp = to + ".tmp." + std::to_string(::getpid());
  const int dst =
      ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (dst < 0) { ::close(src); return false; }

  bool ok = false;
#ifdef FICLONE
  // Whole-file reflink. Instant and space-free where the filesystem supports
  // it, and the result is an independent inode.
  ok = ::ioctl(dst, FICLONE, src) == 0;
#endif
  if (!ok) {
    // Kernel-side copy: no user-space buffer, and the filesystem may still
    // share extents underneath.
    off_t remaining = st.st_size;
    ok = true;
    while (remaining > 0) {
      const ssize_t n = ::copy_file_range(src, nullptr, dst, nullptr,
                                          static_cast<size_t>(remaining), 0);
      if (n > 0) { remaining -= n; continue; }
      if (n < 0 && errno == EINTR) continue;
      ok = false;
      break;
    }
  }
  if (!ok) {
    // Last resort: stream through a fixed buffer rather than reading the whole
    // file into memory, which for a multi-gigabyte link output would cost more
    // than the link being cached.
    if (::lseek(src, 0, SEEK_SET) != (off_t)-1 &&
        ::lseek(dst, 0, SEEK_SET) != (off_t)-1 &&
        ::ftruncate(dst, 0) == 0) {
      std::vector<char> buf(1 << 20);
      ok = true;
      for (;;) {
        const ssize_t r = ::read(src, buf.data(), buf.size());
        if (r == 0) break;
        if (r < 0) { if (errno == EINTR) continue; ok = false; break; }
        ssize_t written = 0;
        while (written < r) {
          const ssize_t w = ::write(dst, buf.data() + written, r - written);
          if (w < 0) { if (errno == EINTR) continue; ok = false; break; }
          written += w;
        }
        if (!ok) break;
      }
    }
  }

  if (ok) ok = ::fchmod(dst, DefaultFileMode()) == 0;
  if (::close(dst) != 0) ok = false;
  ::close(src);
  if (!ok || ::rename(tmp.c_str(), to.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

std::optional<uint64_t> FileSize(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return std::nullopt;
  return static_cast<uint64_t>(st.st_size);
}

std::optional<int64_t> FileMtime(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return std::nullopt;
  return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000 +
         static_cast<int64_t>(st.st_mtim.tv_nsec);
}

std::optional<std::string> RealPath(const std::string& path) {
  char buf[PATH_MAX];
  if (::realpath(path.c_str(), buf) == nullptr) return std::nullopt;
  return std::string(buf);
}

std::string AbsoluteLexical(const std::string& path) {
  return Guarded(
      [&]() -> std::string {
        fs::path p(path);
        if (!p.is_absolute()) p = fs::path(CurrentDir()) / p;
        return p.lexically_normal().string();
      },
      path);
}

std::string DirName(const std::string& path) {
  size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) return "";
  if (pos == 0) return "/";
  return path.substr(0, pos);
}

std::string BaseName(const std::string& path) {
  size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) return path;
  return path.substr(pos + 1);
}

std::string CurrentDir() {
  char buf[PATH_MAX];
  if (::getcwd(buf, sizeof(buf)) == nullptr) return "";
  return std::string(buf);
}

std::optional<std::string> MakeTempDir(const std::string& prefix) {
  const char* base = std::getenv("TMPDIR");
  std::string dir = (base != nullptr && *base != '\0') ? base : "/tmp";
  std::string tmpl = dir + "/" + prefix + "XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  if (::mkdtemp(buf.data()) == nullptr) return std::nullopt;
  return std::string(buf.data());
}

bool RemoveRecursive(const std::string& path) {
  std::error_code ec;
  fs::remove_all(path, ec);
  return !ec;
}

bool RemoveFile(const std::string& path) { return ::unlink(path.c_str()) == 0; }

std::vector<FileEntry> ListFilesRecursive(const std::string& root) {
  std::vector<FileEntry> out;
  std::error_code ec;
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
  if (ec) return out;
  for (const auto& entry : it) {
    std::error_code entry_ec;
    if (!entry.is_regular_file(entry_ec) || entry_ec) continue;
    struct stat st;
    const std::string path = entry.path().string();
    if (::stat(path.c_str(), &st) != 0) continue;
    out.push_back(FileEntry{path, static_cast<uint64_t>(st.st_size),
                            static_cast<int64_t>(st.st_mtime)});
  }
  return out;
}

std::optional<std::string> FindInPath(const std::string& name,
                                      const std::string& exclude_realpath) {
  if (name.find('/') != std::string::npos) {
    // Already a path; nothing to resolve.
    return FileExists(name) ? std::optional<std::string>(name) : std::nullopt;
  }
  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr) return std::nullopt;

  for (const std::string& dir : Split(path_env, ':', /*skip_empty=*/true)) {
    std::string candidate = dir + "/" + name;
    if (::access(candidate.c_str(), X_OK) != 0) continue;
    // Skip a candidate that is really this vcache binary, otherwise a
    // `ln -s vcache g++` masquerade setup would recurse forever.
    if (!exclude_realpath.empty()) {
      auto real = RealPath(candidate);
      if (real && *real == exclude_realpath) continue;
    }
    return candidate;
  }
  return std::nullopt;
}

}  // namespace vcache::util
