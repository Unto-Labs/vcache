// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "util/fs.h"

#include <fcntl.h>
#include <vector>
#if defined(__linux__)
#include <linux/fs.h>   // FICLONE, used under #ifdef below
#endif
#include <sys/ioctl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include "util/str.h"

// glibc gained the copy_file_range(2) wrapper in 2.27; older hosts (EL7) take the byte-copy fallback.
#if defined(__GLIBC__)
#define VCACHE_HAVE_COPY_FILE_RANGE __GLIBC_PREREQ(2, 27)
#else
#define VCACHE_HAVE_COPY_FILE_RANGE 1
#endif

namespace fs = std::filesystem;

namespace vcache::util {
namespace {

// POSIX spells the nanosecond timestamps st_mtim; Apple spells them
// st_mtimespec and predates the standardised name. One accessor rather than an
// #ifdef at each use.
inline struct timespec StatMtime(const struct stat& st) {
#if defined(__APPLE__)
  return st.st_mtimespec;
#else
  return st.st_mtim;
#endif
}

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

namespace {

// Appends until read(2) returns 0. A /proc file reports st_size 0 and still
// has bytes, so the sized read below is not enough on its own.
bool ReadToEof(int fd, std::string* out) {
  char buf[8192];
  while (true) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return true;
    out->append(buf, static_cast<size_t>(n));
  }
}

}  // namespace

std::optional<std::string> ReadFile(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::nullopt;

  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return std::nullopt;
  }

  std::string contents;
  if (S_ISREG(st.st_mode) && st.st_size > 0) {
    contents.resize(static_cast<size_t>(st.st_size));
    size_t got = 0;
    while (got < contents.size()) {
      const ssize_t n = ::read(fd, contents.data() + got, contents.size() - got);
      if (n < 0) {
        if (errno == EINTR) continue;
        ::close(fd);
        return std::nullopt;
      }
      if (n == 0) {
        contents.resize(got);
        break;
      }
      got += static_cast<size_t>(n);
    }
    // The file may have grown past the size fstat reported.
    if (got == static_cast<size_t>(st.st_size) && !ReadToEof(fd, &contents)) {
      ::close(fd);
      return std::nullopt;
    }
  } else if (!ReadToEof(fd, &contents)) {
    ::close(fd);
    return std::nullopt;
  }
  ::close(fd);
  return contents;
}

bool WriteFileAtomic(const std::string& path, std::string_view contents, bool durable) {
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
  // A cost file is a statistic: losing it costs an estimate, not a wrong object.
  if (ok && durable && ::fsync(fd) != 0) {
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
  // it, and the result is an independent inode. APFS has the same trick behind
  // clonefile(2); wiring that up is worth doing once there is a Mac to test it
  // on. Until then macOS falls through to the byte copy below, which is correct
  // and merely slower.
  ok = ::ioctl(dst, FICLONE, src) == 0;
#endif
#if defined(__linux__) && VCACHE_HAVE_COPY_FILE_RANGE
  if (!ok) {
    // Kernel-side copy: no user-space buffer, and the filesystem may still
    // share extents underneath. Linux-only.
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
#endif
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
  const struct timespec mtime = StatMtime(st);
  return static_cast<int64_t>(mtime.tv_sec) * 1000000000 +
         static_cast<int64_t>(mtime.tv_nsec);
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

namespace {

std::optional<std::string> MakeTempDirUnder(const std::string& dir, const std::string& prefix) {
  std::string tmpl = dir + "/" + prefix + "XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  if (::mkdtemp(buf.data()) == nullptr) return std::nullopt;
  return std::string(buf.data());
}

std::string ScratchRoot(const std::string& cache_dir) { return cache_dir + "/tmp"; }

// A dead owner's compiler can outlive it by a little when only vcache was killed.
constexpr int64_t kDeadOwnerScratchGraceSeconds = 10 * 60;
constexpr size_t kScratchTagLength = 8;
constexpr size_t kScratchRandomLength = 6;

// Names the pid namespace and the boot a pid belongs to. A sweep in another
// container sharing the store, or after a reboot, cannot ask the kernel about an
// owner pid from here, and a tag mismatch sends it back to the age rule.
std::string ScratchOwnerTag() {
  char pid_ns[64];
  const ssize_t pid_ns_length = ::readlink("/proc/self/ns/pid", pid_ns, sizeof(pid_ns));
  const auto boot_id = ReadFile("/proc/sys/kernel/random/boot_id");
  if (pid_ns_length <= 0 || !boot_id || boot_id->empty()) return "";
  uint32_t fnv1a = 2166136261u;
  for (const char c : std::string(pid_ns, static_cast<size_t>(pid_ns_length)) + *boot_id) {
    fnv1a = (fnv1a ^ static_cast<unsigned char>(c)) * 16777619u;
  }
  char tag[kScratchTagLength + 1];
  std::snprintf(tag, sizeof(tag), "%08x", fnv1a);
  return tag;
}

struct ScratchOwner {
  pid_t pid;
  std::string tag;
};

// Parses `<prefix><pid>.<tag>-XXXXXX`; an older `<prefix>XXXXXX` name yields nothing.
std::optional<ScratchOwner> ParseScratchOwner(const std::string& name) {
  if (name.size() < kScratchTagLength + kScratchRandomLength + 4) return std::nullopt;
  const size_t tag_end = name.size() - kScratchRandomLength - 1;
  const size_t tag_begin = tag_end - kScratchTagLength;
  if (name[tag_end] != '-' || name[tag_begin - 1] != '.') return std::nullopt;
  const size_t pid_end = tag_begin - 1;
  size_t pid_begin = pid_end;
  while (pid_begin > 0 && name[pid_begin - 1] >= '0' && name[pid_begin - 1] <= '9') --pid_begin;
  if (pid_begin == pid_end || pid_end - pid_begin > 9) return std::nullopt;
  const auto pid = static_cast<pid_t>(std::stol(name.substr(pid_begin, pid_end - pid_begin)));
  if (pid <= 0) return std::nullopt;
  return ScratchOwner{pid, name.substr(tag_begin, kScratchTagLength)};
}

bool ProcessAlive(pid_t pid) { return ::kill(pid, 0) == 0 || errno == EPERM; }

}  // namespace

std::optional<std::string> MakeTempDir(const std::string& prefix) {
  const char* base = std::getenv("TMPDIR");
  return MakeTempDirUnder((base != nullptr && *base != '\0') ? base : "/tmp", prefix);
}

std::optional<std::string> MakeScratchDir(const std::string& cache_dir, const std::string& prefix) {
  const std::string tag = ScratchOwnerTag();
  const std::string owned_prefix =
      tag.empty() ? prefix : prefix + std::to_string(::getpid()) + "." + tag + "-";
  if (!cache_dir.empty() && MakeDirs(ScratchRoot(cache_dir))) {
    if (auto dir = MakeTempDirUnder(ScratchRoot(cache_dir), owned_prefix)) return dir;
  }
  return MakeTempDir(owned_prefix);
}

size_t RemoveStaleScratchDirs(const std::string& cache_dir, int64_t max_age_seconds) {
  std::error_code ec;
  fs::directory_iterator it(ScratchRoot(cache_dir), ec);
  if (ec) return 0;
  const int64_t now = static_cast<int64_t>(::time(nullptr));
  const std::string own_tag = ScratchOwnerTag();
  size_t removed = 0;
  for (const auto& entry : it) {
    const auto owner = ParseScratchOwner(entry.path().filename().string());
    const bool owner_visible = owner && !own_tag.empty() && owner->tag == own_tag;
    // A running compile does not touch its directory's mtime while the compiler
    // rewrites files in it, so age alone cannot tell a long compile from a dead one.
    if (owner_visible && ProcessAlive(owner->pid)) continue;
    struct stat st{};
    if (::lstat(entry.path().c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    const int64_t limit_seconds = owner_visible ? kDeadOwnerScratchGraceSeconds : max_age_seconds;
    if (now - static_cast<int64_t>(st.st_mtime) <= limit_seconds) continue;
    if (RemoveRecursive(entry.path().string())) ++removed;
  }
  return removed;
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

std::optional<std::string> SelfPath() {
#if defined(__APPLE__)
  // Darwin has no procfs. _NSGetExecutablePath reports the buffer it needs
  // through the same out-parameter, and the path it gives back may be relative
  // or contain symlinks, so it still goes through realpath.
  uint32_t size = 0;
  ::_NSGetExecutablePath(nullptr, &size);
  if (size == 0) return std::nullopt;
  std::string buf(size, '\0');
  if (::_NSGetExecutablePath(buf.data(), &size) != 0) return std::nullopt;
  buf.resize(std::strlen(buf.c_str()));
  return RealPath(buf);
#else
  return RealPath("/proc/self/exe");
#endif
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
