// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: GPL-3.0-only
#include "util/fs.h"

#include <fcntl.h>
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
  if (path.empty()) return false;
  if (IsDirectory(path)) return true;
  std::error_code ec;
  fs::create_directories(path, ec);
  return IsDirectory(path);
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

  size_t written = 0;
  bool ok = true;
  while (written < contents.size()) {
    ssize_t n = ::write(fd, contents.data() + written, contents.size() - written);
    if (n <= 0) {
      if (errno == EINTR) continue;
      ok = false;
      break;
    }
    written += static_cast<size_t>(n);
  }
  // Cache entries must survive a crash intact, so flush before the rename.
  if (ok && ::fsync(fd) != 0) ok = false;
  ::close(fd);

  if (!ok || ::rename(tmp_path.c_str(), path.c_str()) != 0) {
    ::unlink(tmp_path.c_str());
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

std::optional<uint64_t> FileSize(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return std::nullopt;
  return static_cast<uint64_t>(st.st_size);
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
