#include "core/preprocessed.h"

#include <cctype>
#include <cstdio>
#include <vector>

namespace vcache::core {
namespace {

// Un-escapes a C string body as the preprocessor writes it. On Linux this is
// almost always a no-op, but a path containing a backslash or quote would
// otherwise round-trip incorrectly.
std::string Unescape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      out.push_back(s[++i]);
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

std::string Escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\\' || c == '"') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

}  // namespace

std::string NormalizeLinemarker(const std::string& line, const RootMap& roots) {
  // Shape: `# <number> "<path>"[ flags...]`. Anything else passes through.
  if (line.empty() || line[0] != '#') return line;

  size_t i = 1;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  if (i >= line.size() || std::isdigit(static_cast<unsigned char>(line[i])) == 0) {
    return line;
  }
  while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  if (i >= line.size() || line[i] != '"') return line;

  const size_t open_quote = i;
  size_t j = open_quote + 1;
  while (j < line.size()) {
    if (line[j] == '\\') {
      j += 2;
      continue;
    }
    if (line[j] == '"') break;
    ++j;
  }
  if (j >= line.size()) return line;  // unterminated; leave alone

  const std::string raw = line.substr(open_quote + 1, j - open_quote - 1);
  const std::string mapped = roots.Canonicalize(Unescape(raw));

  std::string out;
  out.reserve(line.size() + mapped.size());
  out.append(line, 0, open_quote + 1);
  out.append(Escape(mapped));
  out.append(line, j, std::string::npos);
  return out;
}

bool HashNormalizedPreprocessedOutput(const std::string& path,
                                      const RootMap& roots,
                                      hash::Hasher* hasher) {
  FILE* f = ::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;

  std::vector<char> buf(1 << 20);
  std::string pending;  // partial trailing line carried between reads
  bool ok = true;

  auto flush_line = [&](const std::string& line, bool with_newline) {
    // Only lines starting with '#' can be linemarkers; everything else is
    // forwarded without inspection, which keeps this loop cheap.
    if (!line.empty() && line[0] == '#') {
      const std::string normalized = NormalizeLinemarker(line, roots);
      hasher->Update(normalized);
    } else {
      hasher->Update(line);
    }
    if (with_newline) hasher->Update("\n");
  };

  while (true) {
    size_t n = ::fread(buf.data(), 1, buf.size(), f);
    if (n == 0) {
      if (::ferror(f) != 0) ok = false;
      break;
    }
    size_t start = 0;
    for (size_t i = 0; i < n; ++i) {
      if (buf[i] != '\n') continue;
      if (pending.empty()) {
        flush_line(std::string(buf.data() + start, i - start), true);
      } else {
        pending.append(buf.data() + start, i - start);
        flush_line(pending, true);
        pending.clear();
      }
      start = i + 1;
    }
    if (start < n) pending.append(buf.data() + start, n - start);
  }

  if (ok && !pending.empty()) flush_line(pending, false);
  ::fclose(f);
  return ok;
}

}  // namespace vcache::core
