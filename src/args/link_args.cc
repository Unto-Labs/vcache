// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
#include "args/link_args.h"

#include <algorithm>
#include <string_view>

#include "args/compiler_args.h"
#include "util/fs.h"
#include "util/str.h"

namespace vcache::args {
namespace {

bool HasSuffix(const std::string& s, std::string_view suffix) {
  return util::EndsWith(s, suffix);
}

// ".so", ".so.1", ".so.1.2.3" all name a shared library input.
bool LooksLikeSharedLib(const std::string& p) {
  const size_t so = p.find(".so");
  if (so == std::string::npos) return false;
  for (size_t i = so + 3; i < p.size(); ++i) {
    if (p[i] != '.' && !(p[i] >= '0' && p[i] <= '9')) return false;
  }
  return true;
}

// Source files. Their presence means the invocation compiles as well as links,
// which this path declines: the compile half is already cached by RunCompile,
// and build systems do not emit that shape for link steps.
bool LooksLikeSource(const std::string& p) {
  static const char* kExt[] = {".c",  ".cc", ".cpp", ".cxx", ".c++", ".C",
                               ".m",  ".mm", ".s",   ".S",   ".i",   ".ii"};
  for (const char* e : kExt) {
    if (HasSuffix(p, e)) return true;
  }
  return false;
}

}  // namespace

bool LooksLikeLinkInput(const std::string& p) {
  return HasSuffix(p, ".o") || HasSuffix(p, ".obj") || HasSuffix(p, ".a") ||
         HasSuffix(p, ".lo") || LooksLikeSharedLib(p);
}

LinkArgs ParseLink(const std::vector<std::string>& raw_argv) {
  LinkArgs r;
  if (raw_argv.empty()) {
    r.uncacheable = "empty command line";
    return r;
  }

  if (!ExpandResponseFiles(raw_argv, &r.argv)) {
    r.uncacheable = "malformed or too deeply nested @response-file";
    return r;
  }
  r.driver = r.argv[0];

  bool saw_input = false;
  bool saw_source = false;
  std::string pending_uncacheable;

  for (size_t i = 1; i < r.argv.size(); ++i) {
    const std::string& a = r.argv[i];

    // Phases that are not a link at all. Let the compile path own them.
    if (a == "-c" || a == "-E" || a == "-S" || a == "-M" || a == "-MM" ||
        a == "-fsyntax-only") {
      return r;  // is_link stays false
    }

    if (a == "-o") {
      if (i + 1 >= r.argv.size()) {
        r.uncacheable = "-o with no argument";
        return r;
      }
      r.output = r.argv[++i];
      continue;
    }
    if (util::StartsWith(a, "-o") && a.size() > 2 && a[2] != '-') {
      r.output = a.substr(2);
      continue;
    }

    // Flags that make the link write a second file. Replaying an entry without
    // reproducing these would return a correct binary while the companion
    // silently never appeared -- the same trap the compile path guards against.
    if ((a == "-Map" || a == "--Map" || a == "--dependency-file") &&
        i + 1 < r.argv.size()) {
      r.extra_outputs.push_back(r.argv[++i]);
      r.key_args.push_back(a);
      continue;
    }
    if (util::StartsWith(a, "-Map=") || util::StartsWith(a, "--Map=") ||
        util::StartsWith(a, "--dependency-file=") ||
        util::StartsWith(a, "-Wl,-Map=") ||
        util::StartsWith(a, "-Wl,--Map=")) {
      r.extra_outputs.push_back(a.substr(a.find('=') + 1));
      r.key_args.push_back(a);
      continue;
    }
    if (util::StartsWith(a, "-Wl,--dependency-file=")) {
      r.extra_outputs.push_back(a.substr(a.find('=') + 1));
      r.key_args.push_back(a);
      continue;
    }
    if (util::StartsWith(a, "-Wl,-Map,") ||
        util::StartsWith(a, "-Wl,--Map,") ||
        util::StartsWith(a, "-Wl,--dependency-file,")) {
      r.extra_outputs.push_back(a.substr(a.find(',', 4) + 1));
      r.key_args.push_back(a);
      continue;
    }

    // `-Xlinker` forwards exactly one following argument. Output-producing
    // linker options commonly use `-Xlinker -Map -Xlinker out.map`; treating
    // the second `-Xlinker` as the map filename loses the real side output and
    // can turn a cache hit into a successful command with a stale map file.
    if (a == "-Xlinker") {
      if (i + 1 >= r.argv.size()) {
        pending_uncacheable = "-Xlinker with no argument";
        continue;
      }
      const std::string forwarded = r.argv[++i];
      r.key_args.push_back(a);
      r.key_args.push_back(forwarded);
      if (forwarded == "--build-id=uuid") {
        pending_uncacheable =
            "--build-id=uuid produces a different binary every run";
        continue;
      }
      if (forwarded == "-Map" || forwarded == "--Map" ||
          forwarded == "--dependency-file") {
        if (i + 2 >= r.argv.size() || r.argv[i + 1] != "-Xlinker") {
          pending_uncacheable =
              "an output-producing -Xlinker option has no forwarded value";
          continue;
        }
        r.key_args.push_back(r.argv[++i]);
        r.extra_outputs.push_back(r.argv[++i]);
        r.key_args.push_back(r.extra_outputs.back());
        continue;
      }
      if (util::StartsWith(forwarded, "-Map=") ||
          util::StartsWith(forwarded, "--Map=") ||
          util::StartsWith(forwarded, "--dependency-file=")) {
        r.extra_outputs.push_back(forwarded.substr(forwarded.find('=') + 1));
      }
      continue;
    }

    // Deliberately not cached: the output is not a function of the inputs.
    // Recorded rather than returned immediately, because the invocation is
    // still a link and the caller has to know that: returning here with
    // is_link false would send it down the compile path instead.
    if (a == "-Wl,--build-id=uuid" || a == "--build-id=uuid") {
      pending_uncacheable =
          "--build-id=uuid produces a different binary every run";
      r.key_args.push_back(a);
      continue;
    }

    // Scripts and lists are inputs whose contents matter.
    if (a == "-T" && i + 1 < r.argv.size()) {
      r.inputs.push_back(r.argv[++i]);
      r.key_args.push_back(a);
      continue;
    }
    // The value starts immediately after the flag, so take it by the flag's
    // own length. Searching for the last '=' or ',' instead splits a path that
    // contains either character in the wrong place, and hands the pre-key a
    // path that does not exist.
    for (std::string_view flag : {std::string_view("-Wl,--version-script="),
                                  std::string_view("-Wl,--dynamic-list="),
                                  std::string_view("-Wl,-T,"),
                                  std::string_view("-Wl,--retain-symbols-file=")}) {
      if (util::StartsWith(a, flag) && a.size() > flag.size()) {
        r.inputs.push_back(a.substr(flag.size()));
        break;
      }
    }

    if (!a.empty() && a[0] == '-') {
      r.key_args.push_back(a);
      continue;
    }

    // A bare word: an input file.
    if (LooksLikeSource(a)) {
      saw_source = true;
      continue;
    }
    if (LooksLikeLinkInput(a)) {
      r.inputs.push_back(a);
      saw_input = true;
      continue;
    }
    // Something else positional. Keep it in the key; the tracer will tell us
    // whether it was actually read.
    r.key_args.push_back(a);
  }

  if (!saw_input && r.inputs.empty()) return r;  // not a link we recognise
  r.is_link = true;

  if (!pending_uncacheable.empty()) {
    r.uncacheable = pending_uncacheable;
    return r;
  }
  if (saw_source) {
    r.uncacheable = "compiles and links in one step";
    return r;
  }
  if (r.output.empty()) {
    r.uncacheable = "no -o; the default a.out target is not worth caching";
    return r;
  }
  if (r.output == "/dev/null" || r.output == "-") {
    r.uncacheable = "output is not a regular file";
    return r;
  }
  return r;
}

}  // namespace vcache::args
