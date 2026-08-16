// SPDX-License-Identifier: GPL-3.0-only
#include "core/roots.h"

#include <algorithm>
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

std::string ReplaceAll(std::string text, const std::string& from,
                       const std::string& to) {
  if (from.empty()) return text;
  std::string out;
  out.reserve(text.size());
  size_t pos = 0;
  while (true) {
    size_t hit = text.find(from, pos);
    if (hit == std::string::npos) {
      out.append(text, pos, std::string::npos);
      break;
    }
    out.append(text, pos, hit - pos);
    out.append(to);
    pos = hit + from.size();
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
