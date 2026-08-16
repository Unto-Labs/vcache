// SPDX-License-Identifier: GPL-3.0-or-later
// Dependency-file (.d) handling.
//
// gcc deliberately does *not* apply -ffile-prefix-map to dependency output --
// make needs paths that actually exist on this machine. That means a .d file is
// the one cached artifact that is inherently directory-specific, so vcache
// canonicalises it on store and reverse-maps it on restore.
//
// The Makefile fragment gcc emits uses line continuations, backslash-escaped
// spaces and `$$` for a literal dollar, which is enough structure to be worth a
// real grammar; it is parsed with Boost.Spirit X3.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace vcache::core {

class RootMap;

struct DepRule {
  std::vector<std::string> targets;        // unescaped
  std::vector<std::string> prerequisites;  // unescaped
};

struct DepFile {
  std::vector<DepRule> rules;
};

// Parses Makefile-fragment dependency text. Returns nullopt on malformed input.
std::optional<DepFile> ParseDepFile(const std::string& text);

// Renders back to Makefile syntax, re-escaping as gcc does and wrapping with
// the same one-prerequisite-per-continued-line layout.
std::string RenderDepFile(const DepFile& dep);

// Rewrites every target and prerequisite through the root mapping.
// `direction` decides which way: kCanonicalize for storing, kLocalize for
// restoring into the current working tree.
enum class MapDirection { kCanonicalize, kLocalize };
void RemapDepFile(DepFile* dep, const RootMap& roots, MapDirection direction);

}  // namespace vcache::core
