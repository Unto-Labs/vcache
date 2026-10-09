// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "rust/rust_manifest.h"

#include <algorithm>
#include <cstdlib>

#include "hash/hasher.h"
#include "util/str.h"

namespace vcache::rust {
namespace {

constexpr std::string_view kRustManifestHeader = "vcache-rustmanifest-1";

}  // namespace

// A header line, then per state a "state <key>" line followed by
//   "F <digest> <canonical path>"   a source file
//   "X <digest> <name>"             an --extern with a path
//   "N <name>"                      an --extern without one
//   "E <name>=<escaped value>"      an env-dep that was set
//   "U <name>"                      an env-dep that was unset
// rustc escapes newlines in env-dep values, so every record stays on one line.
std::string RenderRustManifest(const std::vector<RustManifestState>& states) {
  std::string out(kRustManifestHeader);
  out.push_back('\n');
  for (const RustManifestState& state : states) {
    out += "state " + state.key + "\n";
    for (const core::ManifestFile& file : state.files) {
      out += "F " + core::RenderManifestFile(file) + "\n";
    }
    for (const RustExtern& ext : state.externs) {
      out += ext.digest.empty() ? "N " + ext.name + "\n"
                                : "X " + ext.digest + " " + ext.name + "\n";
    }
    for (const core::DepEnv& env : state.env_deps) {
      out += env.value ? "E " + env.name + "=" + *env.value + "\n" : "U " + env.name + "\n";
    }
  }
  return out;
}

bool ParseRustManifest(const std::string& text, std::vector<RustManifestState>* states) {
  const auto lines = util::Split(text, '\n', /*skip_empty=*/true);
  if (lines.empty() || lines.front() != kRustManifestHeader) return false;
  for (size_t i = 1; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    if (util::StartsWith(line, "state ")) {
      RustManifestState state;
      state.key = line.substr(6);
      if (state.key.size() != hash::kDigestHexLen) return false;
      states->push_back(std::move(state));
      continue;
    }
    if (states->empty() || line.size() < 3 || line[1] != ' ') return false;
    RustManifestState& state = states->back();
    const std::string rest = line.substr(2);
    switch (line[0]) {
      case 'F': {
        core::ManifestFile file;
        if (!core::ParseManifestFile(rest, &file)) return false;
        state.files.push_back(std::move(file));
        break;
      }
      case 'X': {
        if (rest.size() < hash::kDigestHexLen + 2 || rest[hash::kDigestHexLen] != ' ') {
          return false;
        }
        state.externs.push_back(
            {rest.substr(hash::kDigestHexLen + 1), rest.substr(0, hash::kDigestHexLen)});
        break;
      }
      case 'N':
        state.externs.push_back({rest, ""});
        break;
      case 'E': {
        const size_t eq = rest.find('=');
        if (eq == std::string::npos || eq == 0) return false;
        state.env_deps.push_back({rest.substr(0, eq), rest.substr(eq + 1)});
        break;
      }
      case 'U':
        state.env_deps.push_back({rest, std::nullopt});
        break;
      default:
        return false;
    }
  }
  return true;
}

std::string EscapeEnvDepValue(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    switch (c) {
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\\': out += "\\\\"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

std::optional<std::string> KeyedEnvDepValue(const std::string& name,
                                            std::optional<std::string> escaped_value,
                                            const std::vector<std::string>& path_env_vars,
                                            const core::RootMap& roots) {
  if (escaped_value &&
      std::find(path_env_vars.begin(), path_env_vars.end(), name) != path_env_vars.end()) {
    return roots.Canonicalize(*escaped_value);
  }
  return escaped_value;
}

std::optional<std::string> FindRustStateMismatch(const RustManifestState& state,
                                                 const std::vector<RustExtern>& externs,
                                                 const core::RootMap& roots,
                                                 const std::vector<std::string>& path_env_vars) {
  if (state.externs.size() != externs.size()) return "the --extern set differs";
  for (size_t i = 0; i < externs.size(); ++i) {
    if (state.externs[i].name != externs[i].name) return "the --extern set differs";
    if (state.externs[i].digest != externs[i].digest) {
      return "extern " + externs[i].name + " changed";
    }
  }
  for (const core::DepEnv& env : state.env_deps) {
    const char* raw = std::getenv(env.name.c_str());
    const std::optional<std::string> now = KeyedEnvDepValue(
        env.name,
        raw != nullptr ? std::optional<std::string>(EscapeEnvDepValue(raw)) : std::nullopt,
        path_env_vars, roots);
    if (now != env.value) {
      return "env " + env.name + " is " + (now ? "'" + *now + "'" : std::string("unset")) +
             ", was " + (env.value ? "'" + *env.value + "'" : std::string("unset"));
    }
  }
  return core::FindStaleManifestFile(state.files, roots);
}

std::vector<RustManifestState> RecordRustState(RustManifestState fresh,
                                               std::vector<RustManifestState> older) {
  return core::PrependManifestState(std::move(fresh), std::move(older),
                                    &RustManifestState::key);
}

}  // namespace vcache::rust
