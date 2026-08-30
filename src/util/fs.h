// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// Filesystem helpers. Everything here reports failure via bool/optional rather
// than throwing, because vcache must degrade to "just run the compiler" on any
// cache-side error instead of failing the user's build.
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vcache::util {

bool FileExists(const std::string& path);
bool IsDirectory(const std::string& path);

// Creates `path` and all missing parents. Returns true if it exists afterwards.
bool MakeDirs(const std::string& path);

// 0666 masked by the process umask, cached on first use. Exposed so callers
// that add permission bits to a file can stay inside what the umask allows
// rather than publishing more than the user asked for.
mode_t DefaultFileMode();

std::optional<std::string> ReadFile(const std::string& path);

// Writes via a temporary file in the same directory followed by rename(2), so a
// reader never observes a partially written cache entry.
bool WriteFileAtomic(const std::string& path, std::string_view contents);

// Hard-links `from` to `to`, falling back to a copy when the link fails (for
// example across filesystems). Used to materialise cached objects cheaply.
bool LinkOrCopy(const std::string& from, const std::string& to);

// Copies `from` to `to` without pulling the file through user space, and
// without sharing an inode with the source.
//
// Tries a reflink first, which is near-instant on a filesystem that supports
// copy-on-write, then an in-kernel copy, then a bounded streaming loop. A
// hardlink is deliberately not attempted: the destination is a build artifact a
// linker may later truncate in place, and sharing an inode would corrupt the
// cached copy along with it.
//
// The write goes to a temporary beside `to` and is renamed into place, so a
// reader never observes a half-written file.
bool CloneFile(const std::string& from, const std::string& to);

std::optional<uint64_t> FileSize(const std::string& path);

// Modification time in nanoseconds since the epoch, or nullopt if the file
// does not exist. Used to tell a rebuilt file from the one that was there
// before when its size happens to be unchanged.
std::optional<int64_t> FileMtime(const std::string& path);

// Returns the absolute, symlink-resolved path, or nullopt if it does not exist.
std::optional<std::string> RealPath(const std::string& path);

// Lexical absolute path: prepends the cwd if needed and removes `.`/`..`
// components without touching the filesystem. Unlike RealPath this works for
// paths that do not exist yet.
std::string AbsoluteLexical(const std::string& path);

std::string DirName(const std::string& path);
std::string BaseName(const std::string& path);

std::string CurrentDir();

// Creates a uniquely named directory under the system temp dir and returns it.
std::optional<std::string> MakeTempDir(const std::string& prefix);

bool RemoveRecursive(const std::string& path);
bool RemoveFile(const std::string& path);

struct FileEntry {
  std::string path;
  uint64_t size = 0;
  // Taken from mtime rather than atime: relatime mounts do not reliably update
  // atime on read, so the disk cache refreshes mtime explicitly on each hit and
  // uses it as the LRU timestamp.
  int64_t lru_time = 0;
};

// Recursively lists regular files under `root`. Used by the disk-cache trimmer.
std::vector<FileEntry> ListFilesRecursive(const std::string& root);

// Resolves `name` against $PATH, skipping entries that resolve back to
// `exclude_realpath` (used to avoid a vcache symlink invoking itself).
std::optional<std::string> FindInPath(const std::string& name,
                                      const std::string& exclude_realpath);

}  // namespace vcache::util
