// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// Parsing of link command lines.
//
// A link has no -E equivalent, so unlike a compile there is no canonical text
// that captures the effect of every flag. The key therefore has to be built
// from the command line plus the *observed* input set, and this parse does the
// command-line half:
//
//   1. Decide whether the invocation is a link vcache is willing to cache.
//   2. Locate the output, any additional outputs the flags ask for, and the
//      inputs named explicitly on the command line.
//   3. Split the remaining flags into the ones that belong in the key and the
//      ones that must not.
//
// What it deliberately does NOT do is predict the implicit inputs -- crt
// objects, the libraries the driver's specs add, which directory a -l resolved
// in. Predicting those is what ccache declined to do in 2012, on the grounds
// that it would mean reimplementing the driver. vcache observes them instead;
// see core/link_trace.h.
#pragma once

#include <string>
#include <vector>

namespace vcache::args {

struct LinkArgs {
  // argv as received, with @response-files expanded in place.
  std::vector<std::string> argv;

  std::string driver;  // argv[0]
  std::string output;  // -o target

  // Files named on the command line that the link consumes: objects, archives,
  // shared libraries, linker and version scripts. Not -l names, which are
  // resolved by search and only knowable by observation.
  std::vector<std::string> inputs;

  // Files the flags make the link *write* besides `output` -- a map file, for
  // instance. They have to be captured and replayed, or a hit silently fails to
  // produce them.
  std::vector<std::string> extra_outputs;

  // Flags that affect the produced bytes, with paths left local; the caller
  // canonicalises them through the root map before hashing.
  std::vector<std::string> key_args;

  bool is_link = false;

  // Non-empty means "run the real linker and do not cache", with the reason.
  std::string uncacheable;
};

// `argv[0]` is the driver or linker. Never throws; an unparseable command line
// comes back with `uncacheable` set rather than a partial result.
LinkArgs ParseLink(const std::vector<std::string>& argv);

// True for a path that looks like something a link consumes rather than
// compiles: .o .obj .a .so .so.N .lo .pic.o, and linker scripts named by -T.
bool LooksLikeLinkInput(const std::string& path);

}  // namespace vcache::args
