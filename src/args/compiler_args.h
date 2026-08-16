// SPDX-License-Identifier: GPL-3.0-or-later
// Parsing of gcc/clang command lines.
//
// The parse has three jobs:
//   1. Decide whether the invocation is cacheable at all.
//   2. Locate the single source input, the object output, and any dependency
//      file, since those are the artifacts vcache stores and replays.
//   3. Split the remaining flags into the ones that must go into the cache key
//      and the ones that must not.
//
// (3) is what makes cross-directory hits possible. In preprocessor mode the
// effect of every -I/-D/-include flag is already visible in the preprocessed
// text, so hashing those flags verbatim would make /home/a/proj and
// /work/b/proj miss each other even though their preprocessed output is
// identical. They are therefore deliberately excluded from the key.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace vcache::args {

enum class Language {
  kUnknown,
  kC,
  kCxx,
  kObjC,
  kObjCxx,
  kAssemblerWithCpp,  // .S -- preprocessed, so cacheable
  kAssembler,         // .s -- not preprocessed
};

std::string LanguageName(Language lang);

struct CompilerArgs {
  // argv exactly as vcache received it, with the compiler at index 0 and any
  // @response-files expanded in place.
  std::vector<std::string> argv;

  std::string compiler;  // argv[0]
  std::string source;    // the single input file
  std::string output;    // -o target, or the implied default
  Language language = Language::kUnknown;

  bool compile_only = false;      // -c
  bool preprocess_only = false;   // -E
  bool assemble_only = false;     // -S
  bool generates_debug_info = false;

  // -M or -MM with no -c: the invocation produces a dependency list and nothing
  // else. It is cached by a different route from a compile -- see RunDepScan in
  // core/compile.cc -- because preprocessing to derive a key would cost more
  // than the run being cached.
  bool dep_only = false;

  // Dependency-file generation. `depfile` is the path make will read; it needs
  // canonicalising on store and reverse-mapping on restore, because gcc does
  // not apply -ffile-prefix-map to dependency output.
  bool generates_deps = false;
  std::string depfile;
  bool depfile_from_flag = false;  // true if -MF gave it explicitly

  // True if the caller pinned the dependency-rule target with -MT/-MQ. When it
  // is false vcache supplies the target itself, because the real compile writes
  // to a temporary path that must not end up in the .d file.
  bool dep_target_explicit = false;

  // Prefix-mapping flags the caller passed in. vcache owns path rewriting, so
  // these are removed from the command line it eventually runs.
  std::vector<std::string> incoming_prefix_maps;

  // -march=native / -mtune=native / -mcpu=native, if any were given. The
  // compiler resolves these against whatever CPU it happens to be running on,
  // so the flag text does not determine the code that comes out. They stay on
  // the command line and in key_args; core/compile.cc additionally asks the
  // compiler what it selected and mixes that into the key.
  std::vector<std::string> native_flags;

  // Flags that belong in the cache key, in their original spelling. The caller
  // runs them through RootMap::Canonicalize before hashing; this layer has no
  // knowledge of roots.
  std::vector<std::string> key_args;

  // The command line with the source, -o, dependency flags and incoming prefix
  // maps removed; vcache rebuilds the real invocation from this.
  std::vector<std::string> base_args;

  // Dependency-generation flags (-MD/-MMD/-MP/-MG/-MT...), held separately so
  // they are re-emitted for the real compile but never for preprocessing --
  // gcc rejects -MP or -MF without -M/-MM, which would break every -E run.
  std::vector<std::string> dep_args;

  // Set when the invocation cannot be cached; holds a human-readable reason.
  std::optional<std::string> uncacheable;

  bool cacheable() const { return !uncacheable.has_value(); }
};

// Parses `argv` (including the compiler at index 0). Never throws; anything it
// cannot handle is reported through CompilerArgs::uncacheable so the caller can
// fall back to running the compiler directly.
CompilerArgs Parse(const std::vector<std::string>& argv);

// Expands @file response-file arguments recursively. Returns false if a file
// could not be read or nesting got implausibly deep.
bool ExpandResponseFiles(const std::vector<std::string>& in,
                         std::vector<std::string>* out);

// Infers the language from a source file extension.
Language LanguageFromExtension(const std::string& path);

}  // namespace vcache::args
