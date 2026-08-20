// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "args/compiler_args.h"

#include <algorithm>
#include <string_view>
#include <unordered_set>

#include "core/roots.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/str.h"

namespace vcache::args {
namespace {

using util::StartsWith;

// Options that consume the following argv entry as their value. Anything not
// listed here that starts with '-' is treated as self-contained, which is the
// correct default for gcc's -f/-m/-O/-W families.
const std::unordered_set<std::string>& SeparateValueOptions() {
  static const auto* kSet = new std::unordered_set<std::string>{
      "-o",          "-I",          "-isystem",    "-iquote",
      "-idirafter",  "-iprefix",    "-iwithprefix", "-iwithprefixbefore",
      "-isysroot",   "-imultilib",  "-imacros",    "-include",
      "-L",          "-D",          "-U",          "-MF",
      "-MT",         "-MQ",         "-x",          "-Xpreprocessor",
      "-Xassembler", "-Xlinker",    "-Xclang",     "-arch",
      "-aux-info",   "-B",          "--param",     "-T",
      "-u",          "-z",          "-target",     "--sysroot",
      "-iframework", "-F",          "-system-header-prefix",
  };
  return *kSet;
}

// Flags that make the compiler write a file besides the object.
//
// gcc and clang both have them, and they are the one class of flag where
// caching is actively harmful rather than merely imprecise: the object comes
// back correct and the companion file is simply missing, so a coverage run
// reports no data and a compilation database ends up with holes, long after
// the compile that "succeeded".
bool IsSideOutputFlag(std::string_view arg) {
  // Coverage instrumentation writes a .gcno next to the object (both drivers).
  if (arg == "--coverage" || arg == "-ftest-coverage" || arg == "-fprofile-arcs" ||
      StartsWith(arg, "-fprofile-note=")) {
    return true;
  }
  // -fstack-usage writes .su, -fcallgraph-info writes .ci (gcc; clang has the
  // former). -fdump-* covers gcc's tree/ipa/rtl dump family.
  if (arg == "-fstack-usage" || StartsWith(arg, "-fcallgraph-info") ||
      StartsWith(arg, "-fdump-")) {
    return true;
  }
  // Split DWARF puts the debug info in a companion .dwo, so a replayed object
  // would reference debug info that was never written. Only the splitting
  // forms: -gsplit-dwarf=single keeps the sections inside the object and is
  // perfectly cacheable, which is verified rather than assumed.
  if (arg == "-gsplit-dwarf" || arg == "-gsplit-dwarf=split") return true;
  // clang: -ftime-trace writes <output>.json, and the optimization-record
  // flags write a .opt.yaml.
  if (arg == "-ftime-trace" || StartsWith(arg, "-ftime-trace=") ||
      StartsWith(arg, "-ftime-trace-granularity=") ||
      arg == "-fsave-optimization-record" ||
      StartsWith(arg, "-fsave-optimization-record=") ||
      StartsWith(arg, "-foptimization-record-file=")) {
    return true;
  }
  // gcc: -fopt-info-<kind>=FILE writes the optimisation report to that file.
  // Spelled without an `=` it goes to stderr instead, which vcache captures and
  // replays on a hit, so only the file-writing forms are a problem.
  if (StartsWith(arg, "-fopt-info") && arg.find('=') != std::string_view::npos) {
    return true;
  }
  // gcc: -aux-info FILE writes a listing of every declaration seen.
  if (arg == "-aux-info") return true;
  // clang, value-taking: a compilation-database fragment, a serialized
  // diagnostics file, and a directory of cdb fragments.
  if (arg == "-MJ" || arg == "-serialize-diagnostics" ||
      arg == "--serialize-diagnostics" || arg == "-gen-cdb-fragment-path") {
    return true;
  }
  return false;
}

// Which of the above take their value as the following argument.
bool SideOutputFlagTakesValue(std::string_view arg) {
  return arg == "-MJ" || arg == "-serialize-diagnostics" ||
         arg == "--serialize-diagnostics" || arg == "-gen-cdb-fragment-path" ||
         arg == "-aux-info";
}

// Preprocessor-facing options whose entire effect is already visible in the
// preprocessed output. Hashing them would break cross-directory hits, because
// -I/home/a/proj/inc and -I/work/b/proj/inc differ textually while producing
// identical preprocessed text once paths are canonicalised.
const std::unordered_set<std::string>& PreprocessorOnlyOptions() {
  static const auto* kSet = new std::unordered_set<std::string>{
      "-I",         "-isystem",   "-iquote",    "-idirafter",
      "-iprefix",   "-iwithprefix", "-iwithprefixbefore",
      "-imacros",   "-include",   "-D",         "-U",
      "-iframework", "-F",        "-system-header-prefix",
  };
  return *kSet;
}

bool IsPreprocessorOnlyJoined(std::string_view arg) {
  static constexpr std::string_view kPrefixes[] = {
      "-I", "-D", "-U", "-isystem", "-iquote", "-idirafter", "-imacros",
      "-include", "-F", "-iframework",
  };
  for (std::string_view p : kPrefixes) {
    if (arg.size() > p.size() && StartsWith(arg, p)) return true;
  }
  return false;
}

// Flags of the form `-fxxx=FILE` where the *contents* of FILE decide what code
// the compiler emits. The preprocessed text cannot stand in for them: clang
// reads these files in the middle end, long after preprocessing has finished,
// so two compiles that differ only in what the file says preprocess
// identically. Returns the file part, or nullopt if `arg` is not one of them.
//
// A flag whose file may in turn name further files vcache cannot see does not
// belong here -- see the module flags, which are declined instead.
std::optional<std::string> KeyedFileFlagValue(std::string_view arg) {
  static constexpr std::string_view kPrefixes[] = {
      "-fsanitize-blacklist=",  // the pre-clang-13 spelling of the next one
      "-fsanitize-ignorelist=",
      "-fsanitize-system-ignorelist=",
      "-fsanitize-coverage-allowlist=",
      "-fsanitize-coverage-ignorelist=",
      "-fprofile-list=",
      "-fprofile-sample-use=",
      "-fprofile-remapping-file=",
      "-fxray-attr-list=",
      "-fxray-always-instrument=",
      "-fxray-never-instrument=",
      "-fplugin=",
  };
  for (std::string_view p : kPrefixes) {
    if (StartsWith(arg, p)) return std::string(arg.substr(p.size()));
  }
  return std::nullopt;
}

// Dependency-generation flags. They control side-channel output rather than
// code generation, so they are handled explicitly and kept out of the key.
bool IsDepFlag(std::string_view arg) {
  static constexpr std::string_view kExact[] = {
      "-M", "-MM", "-MD", "-MMD", "-MG", "-MP", "-MF", "-MT", "-MQ",
  };
  for (std::string_view f : kExact) {
    if (arg == f) return true;
  }
  return StartsWith(arg, "-MF") || StartsWith(arg, "-MT") || StartsWith(arg, "-MQ");
}

std::string Extension(const std::string& path) {
  const std::string base = util::BaseName(path);
  size_t dot = base.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= base.size()) return "";
  return base.substr(dot + 1);
}

Language LanguageFromXFlag(const std::string& value) {
  if (value == "c") return Language::kC;
  if (value == "c++") return Language::kCxx;
  if (value == "objective-c") return Language::kObjC;
  if (value == "objective-c++") return Language::kObjCxx;
  if (value == "assembler-with-cpp") return Language::kAssemblerWithCpp;
  if (value == "assembler") return Language::kAssembler;
  return Language::kUnknown;
}

// Flags the compiler resolves against the CPU it is running on. The flag text
// is the same everywhere, the generated code is not, so the key needs the
// resolved target as well -- see ResolveNativeTarget in core/compile.cc.
bool IsNativeTargetFlag(std::string_view arg) {
  static constexpr std::string_view kNative[] = {
      "-march=native", "-mtune=native", "-mcpu=native",
  };
  for (std::string_view p : kNative) {
    if (arg == p) return true;
  }
  return false;
}

std::string DefaultOutputFor(const std::string& source, bool compile_only,
                             bool assemble_only) {
  const std::string base = util::BaseName(source);
  size_t dot = base.find_last_of('.');
  const std::string stem = (dot == std::string::npos) ? base : base.substr(0, dot);
  if (assemble_only) return stem + ".s";
  if (compile_only) return stem + ".o";
  return "a.out";
}

}  // namespace

bool IsLinkOnlyFlag(std::string_view arg) {
  // Deliberately narrow. -static, -shared, -pie and -pthread read like link
  // flags and are not reliably so: -shared implies PIC on some targets and
  // -pthread defines _REENTRANT. A missed hit costs a compile; a wrong object
  // costs a great deal more, so anything not provably linker-only stays keyed.
  static constexpr std::string_view kExact[] = {
      "-rdynamic",      "-s",             "-nostdlib",         "-nostdlib++",
      "-nodefaultlibs", "-nostartfiles",  "-static-libgcc",    "-shared-libgcc",
      "-static-libstdc++",
  };
  for (std::string_view f : kExact) {
    if (arg == f) return true;
  }
  if (StartsWith(arg, "-Wl,")) return true;
  // Joined -lm and -L/usr/lib; the bare spellings take the next argument.
  if (arg.size() > 2 && (StartsWith(arg, "-l") || StartsWith(arg, "-L"))) return true;
  return LinkOnlyFlagTakesValue(arg);
}

bool LinkOnlyFlagTakesValue(std::string_view arg) {
  return arg == "-l" || arg == "-L" || arg == "-Xlinker" || arg == "-T" ||
         arg == "-u" || arg == "-z" || arg == "-e";
}

std::string LanguageName(Language lang) {
  switch (lang) {
    case Language::kC: return "c";
    case Language::kCxx: return "c++";
    case Language::kObjC: return "objective-c";
    case Language::kObjCxx: return "objective-c++";
    case Language::kAssemblerWithCpp: return "assembler-with-cpp";
    case Language::kAssembler: return "assembler";
    case Language::kUnknown: break;
  }
  return "unknown";
}

Language LanguageFromExtension(const std::string& path) {
  const std::string ext = Extension(path);
  if (ext == "c") return Language::kC;
  if (ext == "cc" || ext == "cpp" || ext == "cxx" || ext == "c++" ||
      ext == "C" || ext == "CPP" || ext == "cp" || ext == "CC") {
    return Language::kCxx;
  }
  if (ext == "m") return Language::kObjC;
  if (ext == "mm" || ext == "M") return Language::kObjCxx;
  if (ext == "S" || ext == "sx") return Language::kAssemblerWithCpp;
  if (ext == "s") return Language::kAssembler;
  if (ext == "i") return Language::kC;
  if (ext == "ii") return Language::kCxx;
  return Language::kUnknown;
}

bool ExpandResponseFiles(const std::vector<std::string>& in,
                         std::vector<std::string>* out) {
  // Bounded recursion: a cyclic @file chain must not hang the build.
  constexpr int kMaxDepth = 8;

  struct Expander {
    int depth = 0;
    bool ok = true;

    void Run(const std::vector<std::string>& args, std::vector<std::string>* sink) {
      for (const std::string& arg : args) {
        if (arg.size() < 2 || arg[0] != '@') {
          sink->push_back(arg);
          continue;
        }
        if (depth >= kMaxDepth) {
          ok = false;
          return;
        }
        auto contents = util::ReadFile(arg.substr(1));
        if (!contents) {
          // gcc treats an unreadable @file as a literal argument; match that
          // rather than failing, and let the compiler report the problem.
          sink->push_back(arg);
          continue;
        }
        std::vector<std::string> tokens;
        if (!Tokenize(*contents, &tokens)) {
          ok = false;
          return;
        }
        ++depth;
        Run(tokens, sink);
        --depth;
      }
    }

    // GNU response-file lexing: whitespace separates, quotes group, backslash
    // escapes the next character.
    static bool Tokenize(const std::string& text, std::vector<std::string>* tokens) {
      std::string current;
      bool in_token = false;
      char quote = '\0';
      for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quote != '\0') {
          if (c == '\\' && i + 1 < text.size()) {
            current.push_back(text[++i]);
          } else if (c == quote) {
            quote = '\0';
          } else {
            current.push_back(c);
          }
          continue;
        }
        if (c == '\'' || c == '"') {
          quote = c;
          in_token = true;
        } else if (c == '\\' && i + 1 < text.size()) {
          current.push_back(text[++i]);
          in_token = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
          if (in_token) {
            tokens->push_back(current);
            current.clear();
            in_token = false;
          }
        } else {
          current.push_back(c);
          in_token = true;
        }
      }
      if (quote != '\0') return false;  // unterminated quote
      if (in_token) tokens->push_back(current);
      return true;
    }
  };

  Expander expander;
  expander.Run(in, out);
  return expander.ok;
}

CompilerArgs Parse(const std::vector<std::string>& raw_argv) {
  CompilerArgs result;

  if (raw_argv.empty()) {
    result.uncacheable = "empty command line";
    return result;
  }

  if (!ExpandResponseFiles(raw_argv, &result.argv)) {
    result.uncacheable = "malformed or too deeply nested @response-file";
    result.argv = raw_argv;
    return result;
  }
  result.compiler = result.argv[0];

  std::vector<std::string> sources;
  std::string explicit_x;
  std::string dep_target_flagged;
  bool saw_dep_output_flag = false;   // -MD/-MMD: depfile is a side output
  bool saw_dep_only_flag = false;     // -M/-MM: dependencies replace compilation
  std::vector<std::string> pending_uncacheable;

  for (size_t i = 1; i < result.argv.size(); ++i) {
    const std::string& arg = result.argv[i];

    // Non-flag: an input file.
    if (arg.empty() || arg[0] != '-') {
      sources.push_back(arg);
      continue;
    }
    if (arg == "-") {
      pending_uncacheable.push_back("reads source from stdin");
      result.base_args.push_back(arg);
      continue;
    }

    if (core::IsPrefixMapFlag(arg)) {
      result.incoming_prefix_maps.push_back(arg);
      // A separated "--remap-path-prefix VALUE" also swallows its value.
      if (arg == "--remap-path-prefix" && i + 1 < result.argv.size()) {
        result.incoming_prefix_maps.push_back(result.argv[++i]);
      }
      continue;
    }

    // Dependency flags: recorded, then dropped from base_args. vcache reissues
    // them itself so it can canonicalise the resulting file.
    if (IsDepFlag(arg)) {
      if (arg == "-MD" || arg == "-MMD") {
        saw_dep_output_flag = true;
        result.generates_deps = true;
        result.dep_args.push_back(arg);
      } else if (arg == "-M" || arg == "-MM") {
        saw_dep_only_flag = true;
        result.dep_args.push_back(arg);
      } else if (arg == "-MF") {
        if (i + 1 < result.argv.size()) {
          result.depfile = result.argv[++i];
          result.depfile_from_flag = true;
        }
      } else if (StartsWith(arg, "-MF") && arg.size() > 3) {
        result.depfile = arg.substr(3);
        result.depfile_from_flag = true;
      } else if (arg == "-MT" || arg == "-MQ") {
        result.dep_target_explicit = true;
        if (i + 1 < result.argv.size()) {
          // The target name goes into the depfile verbatim, so it is part of
          // what gets cached and must be in the key.
          dep_target_flagged = result.argv[++i];
          result.dep_args.push_back(arg);
          result.dep_args.push_back(dep_target_flagged);
          result.key_args.push_back(arg);
          result.key_args.push_back(dep_target_flagged);
        }
      } else if (StartsWith(arg, "-MT") || StartsWith(arg, "-MQ")) {
        result.dep_target_explicit = true;
        result.dep_args.push_back(arg);
        result.key_args.push_back(arg);
      } else if (arg == "-MG") {
        // -MG lets a dependency name a header that does not exist yet, which a
        // manifest of file hashes cannot verify: the missing file reads the
        // same whether or not it was supposed to appear.
        pending_uncacheable.push_back("unsupported flag -MG");
        result.dep_args.push_back(arg);
      } else {
        // -MP: affects depfile content only.
        result.dep_args.push_back(arg);
        result.key_args.push_back(arg);
      }
      continue;
    }

    if (arg == "-c") {
      result.compile_only = true;
      continue;  // vcache re-adds -c when it runs the real compile
    }
    if (arg == "-E") {
      result.preprocess_only = true;
      result.base_args.push_back(arg);
      continue;
    }
    if (arg == "-S") {
      result.assemble_only = true;
      continue;
    }
    if (arg == "-o") {
      if (i + 1 < result.argv.size()) result.output = result.argv[++i];
      continue;
    }
    if (StartsWith(arg, "-o") && arg.size() > 2) {
      result.output = arg.substr(2);
      continue;
    }
    if (arg == "-x") {
      if (i + 1 < result.argv.size()) {
        explicit_x = result.argv[++i];
        result.base_args.push_back(arg);
        result.base_args.push_back(explicit_x);
        result.key_args.push_back("-x");
        result.key_args.push_back(explicit_x);
      }
      continue;
    }

    if (StartsWith(arg, "-g") && arg != "-g0") result.generates_debug_info = true;

    if (IsNativeTargetFlag(arg)) {
      result.native_flags.push_back(arg);
      result.base_args.push_back(arg);
      // The spelling belongs in the key in its own right: -march=native and an
      // explicit -march=znver4 can resolve to the same ISA yet differ in what
      // the compiler is allowed to assume.
      result.key_args.push_back(arg);
      continue;
    }

    // A flag naming a file the compiler reads after preprocessing. The flag
    // stays on the command line and in the key; the file goes into the key too,
    // so editing it in place cannot serve a stale object.
    if (auto keyed_file = KeyedFileFlagValue(arg)) {
      result.base_args.push_back(arg);
      result.key_args.push_back(arg);
      if (!keyed_file->empty()) result.key_files.push_back(*keyed_file);
      continue;
    }

    // Flags whose effect vcache cannot reproduce from a stored object.
    //
    // The module flags are here rather than in KeyedFileFlagValue because a
    // .pcm records the paths of the modules *it* imports: hashing the one file
    // named on the command line would not cover the transitive set, and the
    // preprocessor does not expand `import` the way it expands `#include`.
    if (arg == "-save-temps" || StartsWith(arg, "-save-temps=") ||
        arg == "-fsyntax-only" || StartsWith(arg, "-specs=") ||
        StartsWith(arg, "-fprofile-generate") || StartsWith(arg, "-fprofile-use") ||
        StartsWith(arg, "-fauto-profile") || arg == "-frepo" ||
        StartsWith(arg, "-fmodules") || StartsWith(arg, "-fmodule-file=") ||
        StartsWith(arg, "-fmodule-map-file=") ||
        StartsWith(arg, "-fprebuilt-module-path=") ||
        StartsWith(arg, "-fprofile-instr-use")) {
      pending_uncacheable.push_back("unsupported flag " + arg);
      result.base_args.push_back(arg);
      continue;
    }

    // Flags that write a *second* file besides the object. A cache entry holds
    // the object and the dependency file and nothing else, so replaying one of
    // these would leave the build looking successful while the extra output --
    // a .gcno, a .su, a compilation-database fragment -- silently never
    // appeared. That is worse than not caching, because the failure surfaces
    // later and somewhere else, so decline the entry instead.
    //
    // Split by which compiler emits them only for the reader's benefit; the
    // check does not care, and neither driver rejects the other's spelling
    // reliably enough to make the distinction load-bearing.
    if (IsSideOutputFlag(arg)) {
      pending_uncacheable.push_back("flag writes a second output file: " + arg);
      result.base_args.push_back(arg);
      // Value-taking forms must have their value consumed too, or it gets
      // mistaken for a source file and changes what the parse thinks it is
      // looking at. The invocation is uncacheable either way, but the parse
      // should still describe it accurately.
      if (SideOutputFlagTakesValue(arg) && i + 1 < result.argv.size()) {
        result.base_args.push_back(result.argv[++i]);
      }
      continue;
    }

    // Linker-only flags. They stay on the command line, because the real
    // compile must see what the caller passed, but are recorded apart from
    // key_args so ComputeKey can decide whether they belong in the key.
    if (IsLinkOnlyFlag(arg)) {
      result.base_args.push_back(arg);
      result.link_args.push_back(arg);
      if (LinkOnlyFlagTakesValue(arg) && i + 1 < result.argv.size()) {
        const std::string& value = result.argv[++i];
        result.base_args.push_back(value);
        result.link_args.push_back(value);
      }
      continue;
    }

    // Options taking a separate value.
    if (SeparateValueOptions().count(arg) != 0) {
      const bool preprocessor_only = PreprocessorOnlyOptions().count(arg) != 0;
      result.base_args.push_back(arg);
      if (i + 1 < result.argv.size()) {
        const std::string& value = result.argv[++i];
        result.base_args.push_back(value);
        if (!preprocessor_only) {
          result.key_args.push_back(arg);
          result.key_args.push_back(value);
        }
      }
      continue;
    }

    // Joined forms such as -Ifoo or -DBAR=1.
    if (IsPreprocessorOnlyJoined(arg)) {
      result.base_args.push_back(arg);
      continue;
    }

    result.base_args.push_back(arg);
    result.key_args.push_back(arg);
  }

  // ---- Validate the shape of the invocation -------------------------------

  if (sources.empty()) {
    result.uncacheable = "no input file";
    return result;
  }
  if (sources.size() > 1) {
    result.uncacheable =
        "multiple input files (" + std::to_string(sources.size()) + ")";
    return result;
  }
  result.source = sources[0];

  result.language = explicit_x.empty() ? LanguageFromExtension(result.source)
                                       : LanguageFromXFlag(explicit_x);

  if (result.preprocess_only) {
    result.uncacheable = "preprocess-only invocation (-E)";
    return result;
  }
  // -M/-MM without -c emits a dependency list instead of an object. That is a
  // cacheable shape, just not through the preprocessor: see RunDepScan.
  result.dep_only = saw_dep_only_flag && !result.compile_only;

  if (!result.dep_only && !result.compile_only && !result.assemble_only) {
    result.uncacheable = "not a compile-only invocation (no -c)";
    return result;
  }
  if (result.language == Language::kUnknown) {
    result.uncacheable = "unrecognised source language for " + result.source;
    return result;
  }
  if (result.language == Language::kAssembler) {
    result.uncacheable = "plain assembly is not preprocessed";
    return result;
  }
  if (!pending_uncacheable.empty()) {
    result.uncacheable = pending_uncacheable.front();
    return result;
  }

  if (result.dep_only) {
    // With -M the dependency list is the only output. -MF names it, -o names it
    // too, and with neither it goes to stdout -- which vcache can still replay,
    // so an empty depfile here means "write it to stdout".
    if (result.depfile.empty()) result.depfile = result.output;
    result.output.clear();
    result.generates_deps = true;
    if (result.depfile == "-" || result.depfile == "/dev/null") {
      result.uncacheable = "dependency output is not a regular file";
    }
    return result;
  }

  if (result.output.empty()) {
    result.output =
        DefaultOutputFor(result.source, result.compile_only, result.assemble_only);
  }
  if (result.output == "-" || result.output == "/dev/null") {
    result.uncacheable = "output is not a regular file";
    return result;
  }

  // gcc's implicit depfile name for -MD without -MF is the output stem with a
  // .d suffix, in the output's directory.
  if (saw_dep_output_flag && result.depfile.empty()) {
    const std::string dir = util::DirName(result.output);
    std::string base = util::BaseName(result.output);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    result.depfile = dir.empty() ? base + ".d" : dir + "/" + base + ".d";
  }
  if (!result.depfile.empty()) result.generates_deps = true;

  // The language is part of the key: the same preprocessed text compiled as C
  // and as C++ yields different objects.
  result.key_args.push_back("--vcache-lang=" + LanguageName(result.language));

  return result;
}

}  // namespace vcache::args
