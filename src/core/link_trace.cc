// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "core/link_trace.h"

#include <algorithm>

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

bool ParseTraceLog(const std::string& log_path,
                   const std::vector<std::string>& outputs,
                   const std::string& cwd, LinkTrace* out) {
  auto text = util::ReadFile(log_path);
  if (!text) return false;

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

  std::vector<std::string> reads, absent, tools;
  for (const std::string& line : util::Split(*text, '\n', /*skip_empty=*/true)) {
    // A malformed line is a missing dependency record. Partial traces cannot
    // be trusted: skipping one can turn a required input into a false hit.
    if (line.size() < 3 || line[1] != '\t') return false;
    const char kind = line[0];
    std::string path = line.substr(2);
    if (path.empty()) continue;
    path = util::AbsoluteLexical(path[0] == '/' ? path : cwd + "/" + path);
    if (IsIgnoredPath(path)) continue;
    if (is_output(path)) continue;

    switch (kind) {
      case 'R': reads.push_back(std::move(path)); break;
      case 'M': absent.push_back(std::move(path)); break;
      case 'P': tools.push_back(std::move(path)); break;
      case 'D': break;  // listings are covered by the paths probed within them
      default: return false;
    }
  }

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
