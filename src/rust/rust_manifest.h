// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// The manifest that lets a Rust lookup skip rustc's dep-info run.
//
// Learning a crate's source set needs --emit=dep-info, which expands every
// macro and can cost seconds on a crate whose compile is then a cache hit. The
// manifest is keyed on what is known before rustc runs (toolchain, roots,
// flags, the crate root's path and contents, the extern names) and remembers,
// per state, what the last dep-info run reported together with the full key it
// produced. A state is used only while every recorded input still matches.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/depfile.h"
#include "core/manifest.h"
#include "core/roots.h"

namespace vcache::rust {

struct RustExtern {
  std::string name;
  std::string digest;  // of the rlib/rmeta; empty for an --extern without a path
};

struct RustManifestState {
  std::string key;                        // the full cache key these inputs produced
  std::vector<core::ManifestFile> files;  // every source dep-info listed
  std::vector<core::DepEnv> env_deps;     // values escaped as rustc writes them
  // In the state rather than the manifest key, so rebuilding a dependency adds
  // a state instead of forking a second manifest.
  std::vector<RustExtern> externs;        // sorted by name
};

std::string RenderRustManifest(const std::vector<RustManifestState>& states);

// Returns false on any malformed line; the caller then starts a new manifest.
bool ParseRustManifest(const std::string& text, std::vector<RustManifestState>* states);

// rustc escapes backslash, newline and carriage return in an env-dep value.
std::string EscapeEnvDepValue(const std::string& value);

// The form an env-dep value is keyed and compared in: canonicalised for a
// variable named in `path_env_vars`, raw for any other. `escaped_value` is as
// rustc writes it.
std::optional<std::string> KeyedEnvDepValue(const std::string& name,
                                            std::optional<std::string> escaped_value,
                                            const std::vector<std::string>& path_env_vars,
                                            const core::RootMap& roots);

// Returns nullopt while `state` still describes this invocation: the same
// extern digests, the same environment values (keyed as KeyedEnvDepValue does),
// and every recorded file hashing the same at its localised path. Otherwise
// says what differs. Cheap checks run first, so a rebuilt dependency is
// rejected before any file is hashed.
std::optional<std::string> FindRustStateMismatch(const RustManifestState& state,
                                                 const std::vector<RustExtern>& externs,
                                                 const core::RootMap& roots,
                                                 const std::vector<std::string>& path_env_vars);

}  // namespace vcache::rust
