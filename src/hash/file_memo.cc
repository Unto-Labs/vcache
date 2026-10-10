// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "hash/file_memo.h"

#include <sys/stat.h>

#include <ctime>

#include "hash/hasher.h"
#include "util/fs.h"

namespace vcache::hash {
namespace {

// A writer can still change a file within the granularity of its timestamps
// without moving them, so a file touched this recently is hashed but not
// memoised; a later lookup memoises it once it has settled.
constexpr int64_t kUnsettledWindowNs = 2'000'000'000;

int64_t ToNs(const struct timespec& ts) {
  return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + static_cast<int64_t>(ts.tv_nsec);
}

struct FileIdentity {
  uint64_t device = 0;
  uint64_t inode = 0;
  uint64_t size = 0;
  int64_t mtime_ns = 0;
  int64_t ctime_ns = 0;

  bool operator==(const FileIdentity&) const = default;
};

std::optional<FileIdentity> StatIdentity(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return std::nullopt;
  FileIdentity id;
  id.device = static_cast<uint64_t>(st.st_dev);
  id.inode = static_cast<uint64_t>(st.st_ino);
  id.size = static_cast<uint64_t>(st.st_size);
#if defined(__APPLE__)
  id.mtime_ns = ToNs(st.st_mtimespec);
  id.ctime_ns = ToNs(st.st_ctimespec);
#else
  id.mtime_ns = ToNs(st.st_mtim);
  id.ctime_ns = ToNs(st.st_ctim);
#endif
  return id;
}

std::string MemoKey(const FileIdentity& id) {
  Hasher key;
  key.UpdateDelimited("file-digest-memo-v1");
  key.UpdateU64(id.device);
  key.UpdateU64(id.inode);
  key.UpdateU64(id.size);
  key.UpdateU64(static_cast<uint64_t>(id.mtime_ns));
  key.UpdateU64(static_cast<uint64_t>(id.ctime_ns));
  return key.Hex();
}

int64_t NowUnixNs() {
  struct timespec now {};
  ::clock_gettime(CLOCK_REALTIME, &now);
  return ToNs(now);
}

}  // namespace

std::string FileDigestMemoDir(const std::string& cache_dir) { return cache_dir + "/filehash"; }

std::optional<std::string> HashFileMemoized(const std::string& path,
                                            const std::string& memo_dir) {
  return HashFileMemoizedAt(path, memo_dir, NowUnixNs());
}

std::optional<std::string> HashFileMemoizedAt(const std::string& path,
                                              const std::string& memo_dir,
                                              int64_t now_unix_ns) {
  const std::optional<FileIdentity> before = StatIdentity(path);
  if (!before) return HashFile(path);

  const std::string memo_path = memo_dir + "/" + MemoKey(*before);
  if (auto cached = util::ReadFile(memo_path)) {
    if (cached->size() == kDigestHexLen) return *cached;
  }

  auto digest = HashFile(path);
  if (!digest) return std::nullopt;

  const bool settled = now_unix_ns - before->mtime_ns >= kUnsettledWindowNs &&
                       now_unix_ns - before->ctime_ns >= kUnsettledWindowNs;
  // A path renamed onto, or a file rewritten, while it was being hashed would
  // otherwise store the new contents' digest under the old identity.
  if (settled && StatIdentity(path) == before) {
    util::WriteFileAtomic(memo_path, *digest);
  }
  return digest;
}

size_t PruneFileDigestMemos(const std::string& cache_dir, int64_t max_age_seconds) {
  const std::string dir = FileDigestMemoDir(cache_dir);
  if (!util::IsDirectory(dir)) return 0;
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  size_t removed = 0;
  for (const util::FileEntry& entry : util::ListFilesRecursive(dir)) {
    if (now - entry.lru_time > max_age_seconds && util::RemoveFile(entry.path)) ++removed;
  }
  return removed;
}

}  // namespace vcache::hash
