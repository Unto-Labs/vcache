// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "core/link_trace.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <optional>

#include "util/fs.h"
#include "util/str.h"

namespace vcache::core {
namespace {

bool HasPrefix(const std::string& p, std::string_view prefix) {
  return util::StartsWith(p, prefix);
}

}  // namespace

bool IsIgnoredPath(const std::string& path) {
  if (path.empty() || path[0] != '/') return true;  // relative junk from argv

  // Process and kernel state. Never an input; always changing.
  if (HasPrefix(path, "/proc/") || path == "/proc") return true;
  if (HasPrefix(path, "/sys/") || path == "/sys") return true;
  // Character devices used for diagnostics or entropy are not stable file
  // inputs and must never be hashed. Do not exclude all of /dev: /dev/shm is a
  // common build location and can contain real objects and linker scripts.
  if (path == "/dev/null" || path == "/dev/tty" || path == "/dev/random" ||
      path == "/dev/urandom" || HasPrefix(path, "/dev/pts/")) return true;

  // Temporary files are deliberately NOT excluded by prefix. Two reasons, and
  // they point opposite ways:
  //
  //   Excluding /tmp is unsound. TMPDIR is frequently not /tmp -- on the host
  //   this was developed against it is under $HOME -- so a prefix rule misses
  //   the driver's real intermediates, and CI that builds inside /tmp would
  //   have every one of its objects silently dropped from the input set.
  //
  //   Excluding them is also unnecessary. The driver's intermediates are gone
  //   by the time the trace is classified, so they cannot be hashed and are
  //   dropped on that basis in core/link.cc. A temporary that still exists is
  //   indistinguishable from a real input, and is treated as one.

  // Directory self-references the search-path logic produces by the dozen
  // ("/usr/lib/../lib/."). The directory itself is not an input; what was found
  // or not found inside it is, and those are recorded separately.
  if (util::EndsWith(path, "/.") || util::EndsWith(path, "/..")) return true;

  return false;
}

bool ReadTraceRecords(const std::string& log_path, const std::string& cwd, TraceRecords* out) {
  auto text = util::ReadFile(log_path);
  if (!text) return false;
  for (const std::string& line : util::Split(*text, '\n', /*skip_empty=*/true)) {
    // A malformed line is a missing dependency record. Partial traces cannot
    // be trusted: skipping one can turn a required input into a false hit.
    if (line.size() < 3 || line[1] != '\t') return false;
    const char kind = line[0];
    std::string path = line.substr(2);
    if (path[0] != '/') path = cwd + "/" + path;
    switch (kind) {
      case 'R': out->reads.push_back(std::move(path)); break;
      case 'M': out->absent.push_back(std::move(path)); break;
      case 'P': out->tools.push_back(std::move(path)); break;
      case 'L': out->loaded.push_back(std::move(path)); break;
      case 'W': out->written.push_back(std::move(path)); break;
      case 'D': break;  // listings are covered by the paths probed within them
      default: return false;
    }
  }
  return true;
}

bool TraceMarkedIncomplete(const std::string& log_path) {
  struct stat st;
  return ::stat((log_path + ".incomplete").c_str(), &st) == 0;
}

std::string TracerPath() {
  const char* override_path = std::getenv("VCACHE_TRACER");
  if (override_path != nullptr && *override_path != '\0') return override_path;
  auto self = util::SelfPath();
  if (!self) return "";
  return util::DirName(*self) + "/vcache-fstrace.so";
}

bool TracerLoadsHere(const std::string& tracer) {
  // e_ident's class and data bytes, e_type and e_machine all sit in the first
  // 20 bytes, at the same offsets in both classes.
  const auto header = [](const std::string& path) -> std::optional<std::string> {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    std::string bytes(20, '\0');
    const ssize_t got = ::read(fd, bytes.data(), bytes.size());
    ::close(fd);
    if (got != static_cast<ssize_t>(bytes.size()) || bytes.compare(0, 4, "\x7f" "ELF") != 0) {
      return std::nullopt;
    }
    return bytes;
  };
  const std::optional<std::string> self = util::SelfPath();
  if (!self) return false;
  const std::optional<std::string> ours = header(*self);
  const std::optional<std::string> theirs = header(tracer);
  if (!ours || !theirs) return false;
  constexpr char kSharedObjectType = 3;  // ET_DYN
  const bool little_endian = (*theirs)[5] == 1;
  const char type = (*theirs)[little_endian ? 16 : 17];
  return (*ours)[4] == (*theirs)[4] && (*ours)[5] == (*theirs)[5] &&
         ours->compare(18, 2, *theirs, 18, 2) == 0 && type == kSharedObjectType;
}

std::string WithoutPreloadRefusals(const std::string& stderr_text, const std::string& tracer,
                                   bool* removed) {
  // ld.so: "ERROR: ld.so: object '<path>' from LD_PRELOAD cannot be preloaded
  // (<why>): ignored."
  const std::string refusal = "'" + tracer + "' " + std::string(kPreloadIgnoredMessage);
  *removed = false;
  std::string kept;
  kept.reserve(stderr_text.size());
  size_t begin = 0;
  while (begin < stderr_text.size()) {
    size_t end = stderr_text.find('\n', begin);
    end = end == std::string::npos ? stderr_text.size() : end + 1;
    const std::string_view line(stderr_text.data() + begin, end - begin);
    if (line.find(refusal) != std::string_view::npos) {
      *removed = true;
    } else {
      kept.append(line);
    }
    begin = end;
  }
  return kept;
}

bool ParseTraceLog(const std::string& log_path,
                   const std::vector<std::string>& outputs,
                   const std::string& cwd, LinkTrace* out) {
  TraceRecords records;
  if (!ReadTraceRecords(log_path, cwd, &records)) return false;

  std::vector<std::string> abs_outputs;
  abs_outputs.reserve(outputs.size());
  for (const std::string& o : outputs) {
    abs_outputs.push_back(util::AbsoluteLexical(
        o.empty() || o[0] == '/' ? o : cwd + "/" + o));
  }
  const auto is_output = [&](const std::string& p) {
    return std::find(abs_outputs.begin(), abs_outputs.end(), p) !=
           abs_outputs.end();
  };

  // A linker's own implementation is a shared library -- ld.bfd's logic lives
  // in libbfd, lld's in libLLVM -- so what the processes loaded is an input.
  // A file written is kept as one too, as it was before the tracer told the
  // two apart: an intermediate is gone by the time it would be hashed.
  std::vector<std::string> reads, absent, tools;
  const auto keep = [&](const std::vector<std::string>& from, std::vector<std::string>* to) {
    for (const std::string& raw : from) {
      std::string path = util::AbsoluteLexical(raw);
      if (IsIgnoredPath(path) || is_output(path)) continue;
      to->push_back(std::move(path));
    }
  };
  keep(records.reads, &reads);
  keep(records.loaded, &reads);
  keep(records.written, &reads);
  keep(records.absent, &absent);
  keep(records.tools, &tools);

  const auto sort_unique = [](std::vector<std::string>* v) {
    std::sort(v->begin(), v->end());
    v->erase(std::unique(v->begin(), v->end()), v->end());
  };
  sort_unique(&reads);
  sort_unique(&absent);
  sort_unique(&tools);

  // A path that was both probed-absent and later read is an input, not an
  // absence: the search found it on a later attempt, or it was created during
  // the link. Keeping it in both sets would make the entry unhittable.
  std::vector<std::string> only_absent;
  std::set_difference(absent.begin(), absent.end(), reads.begin(), reads.end(),
                      std::back_inserter(only_absent));

  out->inputs = std::move(reads);
  out->absent = std::move(only_absent);
  out->tools = std::move(tools);
  return true;
}

}  // namespace vcache::core
