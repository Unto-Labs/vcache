// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
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
//
// rustc's dep-info also ends with `# env-dep:NAME[=VALUE]` lines naming the
// environment variables the crate read through env!/option_env!. Those are not
// files, so they are kept apart from the rules.
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

struct DepEnv {
  std::string name;
  std::optional<std::string> value;  // nullopt: read while unset
};

struct DepFile {
  std::vector<DepRule> rules;
  std::vector<DepEnv> env_deps;  // in file order; value verbatim, as rustc escaped it
};

// Parses Makefile-fragment dependency text. Returns nullopt on malformed input.
std::optional<DepFile> ParseDepFile(const std::string& text);

// Renders back to Makefile syntax, re-escaping as gcc does, one line per rule,
// followed by any env-dep lines exactly as rustc wrote them.
std::string RenderDepFile(const DepFile& dep);

// Rewrites every target and prerequisite through the root mapping, plus the
// values of the env deps named in `path_env_vars`. Other env-dep values are left
// alone: the cache key holds them raw, so a hit already has the local value.
// `direction` decides which way: kCanonicalize for storing, kLocalize for
// restoring into the current working tree, where a listed env dep takes this
// process's own value of the variable when that canonicalises to the stored one.
enum class MapDirection { kCanonicalize, kLocalize };
void RemapDepFile(DepFile* dep, const RootMap& roots, MapDirection direction,
                  const std::vector<std::string>& path_env_vars = {});

}  // namespace vcache::core
