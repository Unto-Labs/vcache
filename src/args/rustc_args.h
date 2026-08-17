// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// Parsing of rustc command lines.
//
// Rust has no preprocessor, so the C/C++ strategy of hashing preprocessed text
// does not apply. The equivalent inputs are the crate root plus every module it
// reaches (obtained from `--emit=dep-info`), the compiler version, the flags,
// and the contents of every --extern dependency.
//
// The path problem is identical to C/C++ though, and so is the fix: rustc's
// --remap-path-prefix is the analogue of -ffile-prefix-map, and with it rlibs
// become bit-identical across build directories.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace vcache::args {

struct ExternCrate {
  std::string name;
  std::string path;  // rlib/rmeta this crate resolves to; may be empty
};

struct RustcArgs {
  std::vector<std::string> argv;
  std::string compiler;   // argv[0]
  std::string source;     // the crate root .rs file
  std::string out_dir;    // --out-dir
  std::string crate_name;

  std::vector<std::string> emit_kinds;  // parsed from --emit
  std::vector<ExternCrate> externs;

  // Flags that belong in the cache key. Search paths and the output directory
  // are excluded: they change between checkouts without changing the result,
  // and the --extern contents already pin what was actually linked.
  std::vector<std::string> key_args;

  // Command line minus --out-dir, --emit and incoming remap flags; vcache
  // supplies those itself.
  std::vector<std::string> base_args;

  std::vector<std::string> incoming_prefix_maps;

  std::optional<std::string> uncacheable;
  bool cacheable() const { return !uncacheable.has_value(); }
};

RustcArgs ParseRustc(const std::vector<std::string>& argv);

// True if argv[0] names rustc (possibly a wrapper or absolute path).
bool LooksLikeRustc(const std::string& program);

}  // namespace vcache::args
