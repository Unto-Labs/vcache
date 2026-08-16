// SPDX-License-Identifier: GPL-3.0-only
#include "args/rustc_args.h"

#include <unordered_set>

#include "core/roots.h"
#include "util/fs.h"
#include "util/str.h"

namespace vcache::args {
namespace {

using util::StartsWith;

// rustc options that take a separate value.
const std::unordered_set<std::string>& SeparateValueOptions() {
  static const auto* kSet = new std::unordered_set<std::string>{
      "--crate-name", "--crate-type", "--edition",      "--emit",
      "--out-dir",    "-L",           "-l",             "--extern",
      "--target",     "-C",           "-Z",             "-W",
      "-A",           "-D",           "-F",             "--cfg",
      "--check-cfg",  "--error-format", "--json",       "--color",
      "--sysroot",    "--explain",    "-o",             "--print",
      "--cap-lints",  "--remap-path-scope",
  };
  return *kSet;
}

// Options whose value is a filesystem search path. Their location varies
// between checkouts while the resolved dependency contents do not, so they are
// deliberately kept out of the cache key.
bool IsSearchPathOption(const std::string& opt) {
  return opt == "-L" || opt == "--sysroot" || opt == "--out-dir";
}

// Splits "-Cdebuginfo=2" or "--emit=link" into option and value.
bool SplitJoined(const std::string& arg, std::string* opt, std::string* value) {
  // Long form: --opt=value
  if (StartsWith(arg, "--")) {
    size_t eq = arg.find('=');
    if (eq == std::string::npos) return false;
    *opt = arg.substr(0, eq);
    *value = arg.substr(eq + 1);
    return SeparateValueOptions().count(*opt) != 0;
  }
  // Short form: -Cfoo=bar, -Lpath, -Zflag
  if (arg.size() > 2 && arg[0] == '-') {
    const std::string candidate = arg.substr(0, 2);
    if (SeparateValueOptions().count(candidate) != 0) {
      *opt = candidate;
      *value = arg.substr(2);
      return true;
    }
  }
  return false;
}

}  // namespace

bool LooksLikeRustc(const std::string& program) {
  const std::string base = util::BaseName(program);
  return base == "rustc" || base == "rustc.exe" || StartsWith(base, "rustc-");
}

RustcArgs ParseRustc(const std::vector<std::string>& argv) {
  RustcArgs result;
  result.argv = argv;
  if (argv.empty()) {
    result.uncacheable = "empty command line";
    return result;
  }
  result.compiler = argv[0];

  std::vector<std::string> inputs;
  bool saw_explicit_output = false;

  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string& arg = argv[i];

    if (arg.empty() || arg[0] != '-') {
      inputs.push_back(arg);
      continue;
    }
    if (arg == "-") {
      result.uncacheable = "reads source from stdin";
      return result;
    }

    if (core::IsPrefixMapFlag(arg)) {
      result.incoming_prefix_maps.push_back(arg);
      if (arg == "--remap-path-prefix" && i + 1 < argv.size()) {
        result.incoming_prefix_maps.push_back(argv[++i]);
      }
      continue;
    }

    std::string opt = arg;
    std::string value;
    bool joined = SplitJoined(arg, &opt, &value);
    if (!joined && SeparateValueOptions().count(arg) != 0) {
      if (i + 1 >= argv.size()) {
        result.base_args.push_back(arg);
        continue;
      }
      value = argv[++i];
    } else if (!joined) {
      // Standalone flag such as -g, -O, --test, --verbose.
      result.base_args.push_back(arg);
      result.key_args.push_back(arg);
      continue;
    }

    // Options vcache manages itself, kept out of base_args.
    if (opt == "--out-dir") {
      result.out_dir = value;
      continue;
    }
    if (opt == "--emit") {
      result.emit_kinds = util::Split(value, ',', /*skip_empty=*/true);
      continue;
    }
    if (opt == "-o") {
      saw_explicit_output = true;
      result.base_args.push_back(opt);
      result.base_args.push_back(value);
      continue;
    }

    if (opt == "--extern") {
      ExternCrate ext;
      size_t eq = value.find('=');
      if (eq == std::string::npos) {
        ext.name = value;
      } else {
        ext.name = value.substr(0, eq);
        ext.path = value.substr(eq + 1);
      }
      result.externs.push_back(ext);
      // The crate name matters to the key; the path does not, because the
      // dependency's contents are hashed separately.
      result.key_args.push_back("--extern-name=" + ext.name);
      result.base_args.push_back(opt);
      result.base_args.push_back(value);
      continue;
    }

    if (opt == "--crate-name") result.crate_name = value;

    result.base_args.push_back(opt);
    result.base_args.push_back(value);
    if (!IsSearchPathOption(opt)) {
      result.key_args.push_back(opt);
      result.key_args.push_back(value);
    }
  }

  if (inputs.empty()) {
    result.uncacheable = "no input file";
    return result;
  }
  if (inputs.size() > 1) {
    result.uncacheable = "multiple input files";
    return result;
  }
  result.source = inputs[0];

  if (result.out_dir.empty()) {
    // Without --out-dir rustc writes into the cwd under names derived from the
    // crate, which vcache cannot capture reliably.
    result.uncacheable = "no --out-dir";
    return result;
  }
  if (saw_explicit_output) {
    result.uncacheable = "explicit -o is not supported";
    return result;
  }
  if (result.emit_kinds.empty()) {
    result.uncacheable = "no --emit; cannot predict outputs";
    return result;
  }
  for (const std::string& kind : result.emit_kinds) {
    // `--emit=asm=path` style targets write outside --out-dir.
    if (kind.find('=') != std::string::npos) {
      result.uncacheable = "--emit with an explicit path is not supported";
      return result;
    }
  }

  return result;
}

}  // namespace vcache::args
