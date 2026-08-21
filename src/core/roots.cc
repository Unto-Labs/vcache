// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "core/roots.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "util/fs.h"
#include "util/log.h"
#include "util/str.h"

namespace vcache::core {
namespace {

using util::StartsWith;

constexpr std::string_view kDefaultCanonicalBase = "/vcache/";

std::string StripTrailingSlashes(std::string s) {
  while (s.size() > 1 && s.back() == '/') s.pop_back();
  return s;
}

// True if `path` is `root` itself or lies beneath it. Compares whole path
// components so that root "/a/b" does not match "/a/bc".
bool IsUnder(std::string_view path, std::string_view root) {
  if (path.size() < root.size()) return false;
  if (path.compare(0, root.size(), root) != 0) return false;
  if (path.size() == root.size()) return true;
  // "/" as a root already ends in a separator.
  if (root.size() == 1 && root[0] == '/') return true;
  return path[root.size()] == '/';
}

}  // namespace

bool ParseIncomingMapPolicy(std::string_view name, IncomingMapPolicy* out) {
  const std::string lower = util::AsciiLower(util::TrimWhitespace(name));
  if (lower == "error") {
    *out = IncomingMapPolicy::kError;
  } else if (lower == "strip") {
    *out = IncomingMapPolicy::kStrip;
  } else if (lower == "keep") {
    *out = IncomingMapPolicy::kKeep;
  } else {
    return false;
  }
  return true;
}

const char* IncomingMapPolicyName(IncomingMapPolicy policy) {
  switch (policy) {
    case IncomingMapPolicy::kError: return "error";
    case IncomingMapPolicy::kStrip: return "strip";
    case IncomingMapPolicy::kKeep: return "keep";
  }
  return "error";
}

bool IsPrefixMapFlag(std::string_view arg) {
  static constexpr std::string_view kFlags[] = {
      "-ffile-prefix-map=", "-fdebug-prefix-map=", "-fmacro-prefix-map=",
      "-fprofile-prefix-map=", "--remap-path-prefix=",
  };
  for (std::string_view f : kFlags) {
    if (StartsWith(arg, f)) return true;
  }
  // rustc also accepts the separated form `--remap-path-prefix VALUE`.
  return arg == "--remap-path-prefix";
}

RootMap RootMap::FromSpecs(const std::vector<std::string>& specs,
                           std::vector<std::string>* errors) {
  RootMap map;
  std::set<std::string> used_canonicals;

  for (const std::string& raw : specs) {
    const std::string spec = util::TrimWhitespace(raw);
    if (spec.empty()) continue;

    std::string path_part = spec;
    std::string target_part;
    // Split on the first '=' so that a target may itself contain '='.
    size_t eq = spec.find('=');
    if (eq != std::string::npos) {
      path_part = spec.substr(0, eq);
      target_part = spec.substr(eq + 1);
    }

    path_part = util::ExpandTilde(util::TrimWhitespace(path_part));
    if (path_part.empty()) {
      if (errors != nullptr) errors->push_back("empty root path in spec: " + spec);
      continue;
    }

    // Prefer the symlink-resolved path so that two spellings of the same tree
    // canonicalise identically; fall back to lexical for roots that do not
    // exist yet (a legitimate case for generated-source directories).
    std::string abs;
    if (auto real = util::RealPath(path_part)) {
      abs = *real;
    } else {
      abs = util::AbsoluteLexical(path_part);
    }
    abs = StripTrailingSlashes(std::move(abs));

    std::string canonical;
    target_part = util::TrimWhitespace(target_part);
    if (target_part.empty()) {
      canonical = std::string(kDefaultCanonicalBase) + util::BaseName(abs);
    } else if (target_part[0] == '/') {
      canonical = StripTrailingSlashes(target_part);
    } else {
      canonical = std::string(kDefaultCanonicalBase) + target_part;
    }

    if (canonical.empty() || canonical == "/") {
      if (errors != nullptr) errors->push_back("invalid canonical target in spec: " + spec);
      continue;
    }

    // Two roots sharing a canonical prefix would make the mapping ambiguous
    // and could alias distinct files onto one cache key.
    std::string unique = canonical;
    for (int i = 2; used_canonicals.count(unique) != 0; ++i) {
      unique = canonical + "-" + std::to_string(i);
    }
    if (unique != canonical && errors != nullptr) {
      errors->push_back("duplicate canonical prefix " + canonical + "; using " + unique);
    }
    used_canonicals.insert(unique);
    map.roots_.push_back(Root{abs, unique});
  }

  map.SortRoots();
  return map;
}

void RootMap::SortRoots() {
  // Most general first: shorter paths sort earlier. gcc's last-match-wins
  // resolution then makes the most specific root take effect.
  std::stable_sort(roots_.begin(), roots_.end(), [](const Root& a, const Root& b) {
    if (a.path.size() != b.path.size()) return a.path.size() < b.path.size();
    return a.path < b.path;
  });
}

bool RootMap::AddIfUncovered(const std::string& dir, const std::string& name) {
  std::string abs;
  if (auto real = util::RealPath(dir)) {
    abs = *real;
  } else {
    abs = util::AbsoluteLexical(dir);
  }
  abs = StripTrailingSlashes(std::move(abs));
  if (abs.empty() || abs == "/") return false;
  if (Covers(abs)) return false;

  std::string canonical = std::string(kDefaultCanonicalBase) + name;
  for (const Root& r : roots_) {
    if (r.canonical == canonical) return false;  // name already taken
  }
  roots_.push_back(Root{abs, canonical});
  SortRoots();
  return true;
}

bool RootMap::Covers(std::string_view path) const {
  for (const Root& r : roots_) {
    if (IsUnder(path, r.path)) return true;
  }
  return false;
}

std::vector<std::string> RootMap::PrefixMapArgs(PrefixMapStyle style) const {
  const std::string_view flag =
      (style == PrefixMapStyle::kRust) ? "--remap-path-prefix=" : "-ffile-prefix-map=";
  std::vector<std::string> args;
  args.reserve(roots_.size());
  for (const Root& r : roots_) {
    args.push_back(std::string(flag) + r.path + "=" + r.canonical);
  }
  return args;
}

std::string RootMap::Canonicalize(std::string_view path) const {
  // Iterate in reverse so the most specific (longest) root is tried first,
  // matching gcc's effective behaviour for the flags we emit.
  for (auto it = roots_.rbegin(); it != roots_.rend(); ++it) {
    if (!IsUnder(path, it->path)) continue;
    std::string out = it->canonical;
    out.append(path.substr(it->path.size()));
    return out;
  }
  return std::string(path);
}

namespace {

// True if `c` ends a path token in compiler diagnostic text, e.g. the ':' in
// "proj/a.cc:12" or the ')' in "(proj/a.cc)". POSIX allows almost any byte in
// a filename, so instead of enumerating characters that continue a name (an
// unbounded set that previously missed '~', '@', space, and all of UTF-8),
// this enumerates the small, known set of characters that terminate a path
// token in the text gcc/clang/rustc actually emit. Anything not in this set,
// including every byte above 0x7f, is treated as continuing the name, so a
// root followed by one of those is a different directory and must not be
// rewritten.
bool EndsPathToken(unsigned char c) {
  switch (c) {
    case ':': case ',': case ';':
    case '\'': case '"':
    case ')': case ']': case '}': case '>':
    case ' ': case '\t': case '\n': case '\r':
      return true;
    default:
      return false;
  }
}

// True if `c` can immediately precede a path token: the separators a path is
// quoted, bracketed, flagged or delimited with. Used walking backwards from a
// match, so it is the mirror of EndsPathToken() plus the opening halves.
bool PrecedesPathToken(unsigned char c) {
  switch (c) {
    case '(': case '[': case '{': case '<': case '=':
      return true;
    default:
      return EndsPathToken(c);
  }
}

// True if a match beginning at `hit` starts a path token, rather than sitting
// inside a longer one.
//
// IsPathTokenBoundary() guards the right edge; this guards the left. Without
// it a root still matches in the middle of an unrelated absolute path, because
// find() has no notion of where a path begins:
//
//   root "/home/u/proj"  in  "/opt/home/u/proj/x.c"  ->  "/opt/vcache/proj/x.c"
//
// The awkward part is that a mid-path match and a legitimate one are
// textually identical at the match itself -- both are an ordinary character
// followed by the root:
//
//   "/opt/home/u/proj/x.c"      the match is inside another path   reject
//   "-I/home/u/proj/include"    the match follows a compiler flag  accept
//
// What separates them is what lies further back. Walk left to the nearest
// token separator: if a '/' turns up first, the match is part of some longer
// path and must not be rewritten; if a separator or the start of the text
// turns up first, whatever sits between is a flag like "-I" or "--sysroot=",
// and the match does begin a path.
//
// This also makes the rule right rather than merely convenient: with root
// "/proj", the text "/home/u/proj/a.cc" is not under that root at all, and the
// same walk rejects it for the same reason.
//
// The walk stops at the first '/' or separator, so in real diagnostic text it
// covers a single path component or flag rather than scanning the whole line.
//
// Like the right edge, this is a rule about text and not a proof, and it is
// wrong in both directions for inputs that do not occur in compiler output:
//
//   /home/u/a=b/home/u/proj/x.c   a directory named with a separator ends the
//                                 walk early, so a mid-path match is accepted
//                                 and rewritten
//   file:///home/u/proj/a.cc      the walk meets the scheme's '/' first, so a
//   //home/u/proj/a.cc            legitimate match is declined and the local
//                                 path stays in the text
//
// The second pair is the direction that leaks, so it is the one to weigh: an
// unrewritten root is a local absolute path persisted into a shared cache
// entry. Neither shape appears in gcc, clang or rustc diagnostics, which is
// what makes the trade acceptable rather than merely convenient.
bool StartsPathToken(const std::string& text, size_t hit) {
  for (size_t i = hit; i > 0; --i) {
    const unsigned char c = static_cast<unsigned char>(text[i - 1]);
    if (c == '/') return false;
    if (PrecedesPathToken(c)) return true;
  }
  return true;  // the match begins the text
}

// Like a plain find-and-replace, but a match is only accepted at a path
// component boundary. Diagnostics and other free-form text can otherwise
// contain a root's path as a strict prefix of an unrelated sibling (e.g. root
// "/home/u/proj" inside "/home/u/projX/a.cc" or "/home/u/proj-old/a.cc"),
// which must not be rewritten.
//
// A match is accepted when the next character is '/', the text ends, or
// EndsPathToken() says the next character cannot start the same path
// component. '.' gets special treatment: it is a legal filename character
// (so "proj.old" must not be split), but it is also how a sentence describing
// the bare root ends ("... in /home/u/proj."). It is therefore only treated
// as a terminator when whitespace or the end of the text follows it;
// otherwise it is treated as continuing the name.
//
// This deliberately errs toward over-rewriting a pathological sibling name
// (e.g. a directory literally called "proj:" or "proj (old)") rather than
// under-rewriting the root itself: the former is a broken jump-to-file link,
// while the latter would leave a developer's local absolute path in
// diagnostic text that is persisted into the shared cache and replayed onto
// every machine that later hits it (see compile.cc / rust_compile.cc, which
// write CanonicalizeText()'s result into the cached blob).
//
// Note this only guards the right edge of a match; StartsPathToken() above
// guards the left. Both have to accept before ReplaceAll() rewrites a hit.
bool IsPathTokenBoundary(const std::string& text, size_t after) {
  if (after >= text.size()) return true;
  const unsigned char next = static_cast<unsigned char>(text[after]);
  if (next == '/') return true;
  if (next != '.') return EndsPathToken(next);
  // '.' terminates only when it is not itself continuing a name, i.e. when
  // nothing follows it or what follows is whitespace.
  if (after + 1 >= text.size()) return true;
  const unsigned char after_dot = static_cast<unsigned char>(text[after + 1]);
  return after_dot == ' ' || after_dot == '\t' || after_dot == '\n' ||
         after_dot == '\r';
}

// Like a plain find-and-replace, but a match is only accepted at a path
// component boundary on both sides; see StartsPathToken() and
// IsPathTokenBoundary() for the exact rules. If a root path still appears
// verbatim in the output afterward, that is a local absolute path about to be
// persisted into a shared cache entry, so it is logged (when VCACHE_LOG is
// enabled) rather than silently dropped -- this is diagnostic only and never
// changes the returned text or fails the build.
std::string ReplaceAll(std::string text, const std::string& from,
                       const std::string& to) {
  if (from.empty()) return text;
  std::string out;
  out.reserve(text.size());
  size_t pos = 0;
  bool any_unrewritten = false;
  while (true) {
    size_t hit = text.find(from, pos);
    if (hit == std::string::npos) {
      out.append(text, pos, std::string::npos);
      break;
    }
    out.append(text, pos, hit - pos);
    const size_t after = hit + from.size();
    const bool boundary_ok =
        StartsPathToken(text, hit) && IsPathTokenBoundary(text, after);
    if (!boundary_ok) any_unrewritten = true;
    out.append(boundary_ok ? to : from);
    pos = after;
  }
  if (any_unrewritten) {
    VCACHE_LOG("root path left unrewritten in text (component boundary "
                "declined match): " + from);
  }
  return out;
}

}  // namespace

std::string RootMap::CanonicalizeText(const std::string& text) const {
  // Most specific first so a nested root is not shadowed by its parent.
  std::string out = text;
  for (auto it = roots_.rbegin(); it != roots_.rend(); ++it) {
    out = ReplaceAll(std::move(out), it->path, it->canonical);
  }
  return out;
}

std::string RootMap::LocalizeText(const std::string& text) const {
  std::string out = text;
  std::vector<const Root*> by_canonical;
  by_canonical.reserve(roots_.size());
  for (const Root& r : roots_) by_canonical.push_back(&r);
  std::sort(by_canonical.begin(), by_canonical.end(),
            [](const Root* a, const Root* b) {
              return a->canonical.size() > b->canonical.size();
            });
  for (const Root* r : by_canonical) {
    out = ReplaceAll(std::move(out), r->canonical, r->path);
  }
  return out;
}

std::string RootMap::Localize(std::string_view path) const {
  // Longest canonical prefix first, mirroring Canonicalize.
  const Root* best = nullptr;
  for (const Root& r : roots_) {
    if (!IsUnder(path, r.canonical)) continue;
    if (best == nullptr || r.canonical.size() > best->canonical.size()) best = &r;
  }
  if (best == nullptr) return std::string(path);
  std::string out = best->path;
  out.append(path.substr(best->canonical.size()));
  return out;
}

std::string RootMap::Fingerprint() const {
  // Canonical targets only, sorted so the key does not depend on the order the
  // roots happened to be configured in.
  std::vector<std::string> canonicals;
  canonicals.reserve(roots_.size());
  for (const Root& r : roots_) canonicals.push_back(r.canonical);
  std::sort(canonicals.begin(), canonicals.end());

  std::string out;
  for (const std::string& c : canonicals) {
    out.append(c).push_back('\n');
  }
  return out;
}

std::string RootMap::DebugString() const {
  std::string out;
  for (const Root& r : roots_) {
    out.append(r.path).push_back('=');
    out.append(r.canonical).push_back('\n');
  }
  return out;
}

}  // namespace vcache::core
