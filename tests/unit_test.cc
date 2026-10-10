// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// Unit tests for vcache. Deliberately dependency-free: a tiny harness keeps the
// build to plain make, as the plan asks.

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "args/compiler_args.h"
#include "core/link_trace.h"
#include "args/link_args.h"
#include "args/rustc_args.h"
#include "core/compile.h"
#include "core/config.h"
#include "core/cost.h"
#include "core/depfile.h"
#include "core/preprocessed.h"
#include "core/roots.h"
#include "core/stats.h"
#include "daemon/jobserver.h"
#include "daemon/upload_queue.h"
#include "daemon/protocol.h"
#include "daemon/client.h"
#include "daemon/server.h"
#include "hash/hasher.h"
#include "hash/sha256.h"
#include "rust/rust_compile.h"
#include "rust/rust_manifest.h"
#include "storage/chain.h"
#include "storage/disk_storage.h"
#include "storage/s3_storage.h"
#include "storage/storage.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/str.h"
#include "util/subprocess.h"

namespace fs = std::filesystem;

namespace {

int g_pass = 0;
int g_fail = 0;
const char* g_section = "";

void Section(const char* name) {
  g_section = name;
  std::printf("\n\033[1m%s\033[0m\n", name);
}

void Check(bool condition, const std::string& what) {
  if (condition) {
    ++g_pass;
    std::printf("  \033[32mPASS\033[0m %s\n", what.c_str());
  } else {
    ++g_fail;
    std::printf("  \033[31mFAIL\033[0m %s\n", what.c_str());
  }
}

bool Contains(const std::vector<std::string>& haystack, const std::string& needle) {
  return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

void CheckEq(const std::string& actual, const std::string& expected,
             const std::string& what) {
  if (actual == expected) {
    ++g_pass;
    std::printf("  \033[32mPASS\033[0m %s\n", what.c_str());
  } else {
    ++g_fail;
    std::printf("  \033[31mFAIL\033[0m %s\n         expected: %s\n         actual:   %s\n",
                what.c_str(), expected.c_str(), actual.c_str());
  }
}

using namespace vcache;

core::RootMap MakeRoots(const std::vector<std::string>& specs) {
  std::vector<std::string> errors;
  return core::RootMap::FromSpecs(specs, &errors);
}

// ---------------------------------------------------------------------------

void TestLinkArgs() {
  Section("args::ParseLink");

  auto plain = args::ParseLink({"gcc", "a.o", "b.o", "-o", "app"});
  Check(plain.is_link && !plain.uncacheable, "a plain link is cacheable");
  CheckEq(plain.output, "app", "output is found");
  Check(plain.inputs.size() == 2, "both objects are inputs");

  // Phase flags belong to the compile path; the link parser must not claim them.
  Check(!args::ParseLink({"gcc", "-c", "a.c", "-o", "a.o"}).is_link,
        "-c is not a link");
  Check(!args::ParseLink({"gcc", "-E", "a.c"}).is_link, "-E is not a link");
  Check(!args::ParseLink({"gcc", "-S", "a.c"}).is_link, "-S is not a link");
  Check(!args::ParseLink({"gcc", "-M", "a.c"}).is_link, "-M is not a link");

  // Declines, each for a reason that would otherwise be a wrong answer.
  auto uuid = args::ParseLink(
      {"gcc", "a.o", "-Wl,--build-id=uuid", "-o", "app"});
  Check(uuid.is_link && uuid.uncacheable.has_value(),
        "--build-id=uuid is declined");
  auto mixed = args::ParseLink({"gcc", "a.o", "b.c", "-o", "app"});
  Check(mixed.is_link && mixed.uncacheable.has_value(),
        "compile-and-link is declined");
  auto devnull = args::ParseLink({"gcc", "a.o", "-o", "/dev/null"});
  Check(devnull.is_link && devnull.uncacheable.has_value(),
        "output to /dev/null is declined");
  auto noout = args::ParseLink({"gcc", "a.o"});
  Check(noout.is_link && noout.uncacheable.has_value(), "no -o is declined");

  // Second outputs have to be captured, or a hit returns the binary and
  // silently leaves the companion file missing.
  auto mapped = args::ParseLink(
      {"gcc", "a.o", "-Wl,-Map=out.map", "-o", "app"});
  Check(mapped.extra_outputs.size() == 1 && mapped.extra_outputs[0] == "out.map",
        "-Wl,-Map= is recorded as an extra output");
  auto depf = args::ParseLink(
      {"gcc", "a.o", "-Wl,--dependency-file=d.d", "-o", "app"});
  Check(depf.extra_outputs.size() == 1, "--dependency-file is an extra output");
  auto comma_map = args::ParseLink(
      {"gcc", "a.o", "-Wl,-Map,out.map", "-o", "app"});
  Check(comma_map.extra_outputs.size() == 1 &&
            comma_map.extra_outputs[0] == "out.map",
        "-Wl,-Map, is recorded as an extra output");
  auto xlinker_map = args::ParseLink(
      {"gcc", "a.o", "-Xlinker", "-Map", "-Xlinker", "out.map",
       "-o", "app"});
  Check(xlinker_map.extra_outputs.size() == 1 &&
            xlinker_map.extra_outputs[0] == "out.map",
        "-Xlinker -Map captures its forwarded output");
  auto xlinker_uuid = args::ParseLink(
      {"gcc", "a.o", "-Xlinker", "--build-id=uuid", "-o", "app"});
  Check(xlinker_uuid.is_link && xlinker_uuid.uncacheable.has_value(),
        "-Xlinker --build-id=uuid is declined");

  // Scripts are inputs whose contents matter.
  auto scripted = args::ParseLink(
      {"gcc", "a.o", "-T", "link.ld", "-o", "app"});
  Check(std::find(scripted.inputs.begin(), scripted.inputs.end(), "link.ld") !=
            scripted.inputs.end(),
        "a linker script is an input");

  // Shared libraries in all their spellings.
  Check(args::LooksLikeLinkInput("libfoo.so"), ".so is a link input");
  Check(args::LooksLikeLinkInput("libfoo.so.1"), ".so.1 is a link input");
  Check(args::LooksLikeLinkInput("libfoo.so.1.2.3"), ".so.1.2.3 is a link input");
  Check(args::LooksLikeLinkInput("foo.a"), ".a is a link input");
  Check(!args::LooksLikeLinkInput("foo.c"), ".c is not a link input");
  Check(!args::LooksLikeLinkInput("libfoo.solid"), ".solid is not a shared lib");

  // A value taken by the flag's length, not by searching for a separator:
  // a path containing '=' or ',' splits in the wrong place otherwise, and the
  // pre-key then hashes a path that does not exist.
  auto odd = args::ParseLink(
      {"gcc", "a.o", "-Wl,--version-script=/a=b/c,d/ver.map", "-o", "app"});
  Check(std::find(odd.inputs.begin(), odd.inputs.end(),
                  "/a=b/c,d/ver.map") != odd.inputs.end(),
        "a script path containing = and , survives intact");
  auto dashT = args::ParseLink({"gcc", "a.o", "-Wl,-T,/x,y/link.ld", "-o", "app"});
  Check(std::find(dashT.inputs.begin(), dashT.inputs.end(), "/x,y/link.ld") !=
            dashT.inputs.end(),
        "-Wl,-T, takes its value by flag length");

  // -o attached to the flag.
  CheckEq(args::ParseLink({"gcc", "a.o", "-oapp"}).output, "app",
          "-oapp attached form");
}

void TestLinkTraceClassification() {
  Section("core::link_trace");

  // Process and kernel state is never an input.
  Check(core::IsIgnoredPath("/proc/self/maps"), "/proc is ignored");
  Check(core::IsIgnoredPath("/sys/devices/x"), "/sys is ignored");
  Check(core::IsIgnoredPath("/dev/null"), "/dev/null is ignored");
  Check(core::IsIgnoredPath("/usr/lib/."), "a directory self-reference is ignored");
  Check(core::IsIgnoredPath("relative/path"), "a relative path is ignored");

  // Temporary directories are deliberately NOT ignored by prefix: TMPDIR is
  // frequently not /tmp, and a build running inside /tmp would otherwise have
  // its objects dropped from the input set.
  Check(!core::IsIgnoredPath("/tmp/build/foo.o"),
        "/tmp is not excluded by prefix");
  Check(!core::IsIgnoredPath("/dev/shm/build/foo.o"),
        "/dev/shm is not excluded with device nodes");
  Check(!core::IsIgnoredPath("/run/user/1000/build/foo.o"),
        "/run is not excluded by prefix");
  Check(!core::IsIgnoredPath("/home/u/proj/locale/messages.o"),
        "a project locale directory is not excluded by name");
  Check(!core::IsIgnoredPath("/usr/lib/x86_64-linux-gnu/libc.so.6"),
        "a real library is kept");
  Check(!core::IsIgnoredPath("/home/u/proj/a.o"), "a real object is kept");

  // A trace log becomes two sets, with the output excluded from both.
  const std::string dir = "/tmp/vcache-tracetest";
  util::RemoveRecursive(dir);
  Check(util::MakeDirs(dir), "trace test dir");
  const std::string log = dir + "/trace";
  util::WriteFileAtomic(log,
                        "P\t/usr/bin/ld\n"
                        "R\t/home/u/proj/a.o\n"
                        "M\t/opt/lib/libfoo.so\n"
                        "R\t/home/u/proj/app\n"       // the output
                        "R\t/proc/self/exe\n"          // ignored
                        "M\t/home/u/proj/app\n");       // the output again
  core::LinkTrace trace;
  Check(core::ParseTraceLog(log, {"/home/u/proj/app"}, "/home/u/proj", &trace),
        "a trace log parses");
  Check(trace.tools.size() == 1 && trace.tools[0] == "/usr/bin/ld",
        "the toolchain is captured");
  Check(trace.inputs.size() == 1 && trace.inputs[0] == "/home/u/proj/a.o",
        "the output is not an input to itself");
  Check(trace.absent.size() == 1 && trace.absent[0] == "/opt/lib/libfoo.so",
        "the absent set survives, without the output");

  // A path both probed-absent and later read is an input, not an absence;
  // keeping it in both sets would make the entry permanently unhittable.
  util::WriteFileAtomic(log, "P\t/usr/bin/ld\nM\t/a/b.so\nR\t/a/b.so\n");
  core::LinkTrace second;
  Check(core::ParseTraceLog(log, {}, "/", &second), "second trace parses");
  Check(second.absent.empty(), "a path later found is not left in the absent set");
  Check(second.inputs.size() == 1, "and is recorded as an input");

  Check(!core::ParseTraceLog(dir + "/missing", {}, "/", &trace),
        "a missing trace log is reported");
  util::WriteFileAtomic(log, "P\t/usr/bin/ld\ngarbage line\n");
  core::LinkTrace malformed;
  Check(!core::ParseTraceLog(log, {}, "/", &malformed),
        "a malformed trace is rejected rather than partially trusted");
  util::RemoveRecursive(dir);
}

void TestStringUtils() {
  Section("util::str");

  uint64_t size = 0;
  Check(util::ParseSize("10G", &size) && size == (10ull << 30), "ParseSize 10G");
  Check(util::ParseSize("512M", &size) && size == (512ull << 20), "ParseSize 512M");
  Check(util::ParseSize("1024", &size) && size == 1024, "ParseSize bare number");
  Check(!util::ParseSize("banana", &size), "ParseSize rejects garbage");
  Check(!util::ParseSize("", &size), "ParseSize rejects empty");
  Check(!util::ParseSize("99999999999999999999999G", &size),
        "ParseSize rejects overflow");

  auto parts = util::Split("a:b::c", ':', /*skip_empty=*/false);
  Check(parts.size() == 4 && parts[2].empty(), "Split keeps empty fields");
  parts = util::Split("a:b::c", ':', /*skip_empty=*/true);
  Check(parts.size() == 3, "Split can skip empty fields");

  CheckEq(util::HexEncode(reinterpret_cast<const unsigned char*>("\x00\xff\x10"), 3),
          "00ff10", "HexEncode");
}

void TestRootMap() {
  Section("core::RootMap");

  core::RootMap roots = MakeRoots({"/home/u/proj=proj"});
  CheckEq(roots.Canonicalize("/home/u/proj/src/a.cc"), "/vcache/proj/src/a.cc",
          "Canonicalize rewrites a path under a root");
  CheckEq(roots.Canonicalize("/elsewhere/a.cc"), "/elsewhere/a.cc",
          "Canonicalize leaves unrelated paths alone");
  CheckEq(roots.Localize("/vcache/proj/src/a.cc"), "/home/u/proj/src/a.cc",
          "Localize is the inverse of Canonicalize");

  // Component-boundary matching: /home/u/proj must not match /home/u/projX.
  CheckEq(roots.Canonicalize("/home/u/projX/a.cc"), "/home/u/projX/a.cc",
          "root matching respects path component boundaries");
  CheckEq(roots.Canonicalize("/home/u/proj"), "/vcache/proj",
          "the root itself canonicalises");

  // Nested roots: the most specific must win.
  core::RootMap nested = MakeRoots({"/a/b/c=inner", "/a=outer"});
  CheckEq(nested.Canonicalize("/a/b/c/f.h"), "/vcache/inner/f.h",
          "nested roots resolve most-specific-first");
  CheckEq(nested.Canonicalize("/a/b/x.h"), "/vcache/outer/b/x.h",
          "outer root still applies outside the inner one");

  // gcc resolves overlapping -ffile-prefix-map entries last-match-wins, so the
  // emitted flags must run most-general to most-specific.
  auto flags = nested.PrefixMapArgs(core::PrefixMapStyle::kC);
  Check(flags.size() == 2, "one prefix-map flag per root");
  Check(flags[0].find("/a=") != std::string::npos &&
            flags[1].find("/a/b/c=") != std::string::npos,
        "prefix-map flags are ordered general-to-specific");
  Check(util::StartsWith(flags[0], "-ffile-prefix-map="),
        "C style emits -ffile-prefix-map");
  auto rust_flags = nested.PrefixMapArgs(core::PrefixMapStyle::kRust);
  Check(util::StartsWith(rust_flags[0], "--remap-path-prefix="),
        "Rust style emits --remap-path-prefix");

  // The fingerprint must not depend on where the tree lives, or cross-directory
  // hits are impossible.
  core::RootMap a = MakeRoots({"/home/alice/checkout=proj"});
  core::RootMap b = MakeRoots({"/var/tmp/other/checkout=proj"});
  CheckEq(a.Fingerprint(), b.Fingerprint(),
          "fingerprint is independent of the local root path");
  Check(a.DebugString() != b.DebugString(),
        "DebugString does show the local root path");
  core::RootMap c = MakeRoots({"/home/alice/checkout=different"});
  Check(a.Fingerprint() != c.Fingerprint(),
        "fingerprint changes when the canonical target changes");

  // Ordering of specs must not change the fingerprint.
  core::RootMap order1 = MakeRoots({"/x=one", "/y/z=two"});
  core::RootMap order2 = MakeRoots({"/y/z=two", "/x=one"});
  CheckEq(order1.Fingerprint(), order2.Fingerprint(),
          "fingerprint is independent of spec ordering");

  // Duplicate canonical targets would alias distinct trees onto one key.
  std::vector<std::string> errors;
  core::RootMap dup = core::RootMap::FromSpecs({"/p/one=same", "/p/two=same"}, &errors);
  Check(!errors.empty(), "duplicate canonical targets are reported");
  Check(dup.roots().size() == 2 &&
            dup.roots()[0].canonical != dup.roots()[1].canonical,
        "duplicate canonical targets are disambiguated");

  // Text rewriting, used to replay diagnostics.
  CheckEq(roots.CanonicalizeText("error in /home/u/proj/src/a.cc:12"),
          "error in /vcache/proj/src/a.cc:12", "CanonicalizeText");
  CheckEq(roots.LocalizeText("error in /vcache/proj/src/a.cc:12"),
          "error in /home/u/proj/src/a.cc:12", "LocalizeText");

  // Text rewriting must respect the same component boundaries as
  // Canonicalize(): a sibling directory whose name merely extends the root's
  // must not be rewritten, even embedded in free-form diagnostic text.
  CheckEq(roots.CanonicalizeText("error in /home/u/projX/src/a.cc:12"),
          "error in /home/u/projX/src/a.cc:12",
          "CanonicalizeText respects path component boundaries");
  CheckEq(roots.CanonicalizeText("error in /home/u/proj-old/a.cc"),
          "error in /home/u/proj-old/a.cc",
          "CanonicalizeText does not match a hyphenated sibling");
  CheckEq(roots.LocalizeText("error in /vcache/projX/src/a.cc:12"),
          "error in /vcache/projX/src/a.cc:12",
          "LocalizeText respects path component boundaries");
  // The root itself, with no trailing separator, must still be rewritten.
  CheckEq(roots.CanonicalizeText("cd /home/u/proj; build"),
          "cd /vcache/proj; build",
          "CanonicalizeText still rewrites the bare root");

  // Siblings using a non-alphanumeric separator, or a non-ASCII name, must
  // not be rewritten either -- these bytes are legal in a filename, so an
  // allowlist of five character classes is not enough (the point that
  // exposed the original denylist-based fix as incomplete).
  CheckEq(roots.CanonicalizeText("error in /home/u/proj~old/a.cc"),
          "error in /home/u/proj~old/a.cc",
          "CanonicalizeText does not match a tilde-separated sibling");
  CheckEq(roots.CanonicalizeText("error in /home/u/proj@2/a.cc"),
          "error in /home/u/proj@2/a.cc",
          "CanonicalizeText does not match an '@'-separated sibling");
  CheckEq(roots.CanonicalizeText("error in /home/u/proj\xC3\xA9/a.cc"),
          "error in /home/u/proj\xC3\xA9/a.cc",
          "CanonicalizeText does not match a non-ASCII sibling");
  CheckEq(roots.CanonicalizeText("error in /home/u/proj%2/a.cc"),
          "error in /home/u/proj%2/a.cc",
          "CanonicalizeText does not match a '%'-separated sibling");
  CheckEq(roots.CanonicalizeText("error in /home/u/proj=x/a.cc"),
          "error in /home/u/proj=x/a.cc",
          "CanonicalizeText does not match an '='-separated sibling");

  // The under-rewrite direction: a bare root followed by punctuation must
  // still be rewritten, not left in the output verbatim, since the result is
  // written into diagnostics persisted into the shared cache. Getting this
  // wrong leaks a developer's local absolute path to every machine that
  // later hits the entry.
  CheckEq(roots.CanonicalizeText("warning generated in /home/u/proj."),
          "warning generated in /vcache/proj.",
          "CanonicalizeText rewrites the bare root before a sentence-final '.'");
  CheckEq(roots.CanonicalizeText("path is /home/u/proj:"),
          "path is /vcache/proj:",
          "CanonicalizeText rewrites the bare root before ':'");
  CheckEq(roots.CanonicalizeText("In file included from /home/u/proj, line 3"),
          "In file included from /vcache/proj, line 3",
          "CanonicalizeText rewrites the bare root before ','");
  CheckEq(roots.CanonicalizeText("(see /home/u/proj)"),
          "(see /vcache/proj)",
          "CanonicalizeText rewrites the bare root before ')'");
  CheckEq(roots.CanonicalizeText("the file /home/u/proj is stale"),
          "the file /vcache/proj is stale",
          "CanonicalizeText rewrites the bare root before a space");
  CheckEq(roots.CanonicalizeText("cannot open '/home/u/proj'"),
          "cannot open '/vcache/proj'",
          "CanonicalizeText rewrites the bare root before a quote");
  CheckEq(roots.CanonicalizeText("see [/home/u/proj]"),
          "see [/vcache/proj]",
          "CanonicalizeText rewrites the bare root before ']'");
  // But "proj.old" is a sibling, not the root followed by punctuation --
  // the '.' here continues the name because it is not followed by
  // whitespace or end of text.
  CheckEq(roots.CanonicalizeText("backup at /home/u/proj.old/a.cc"),
          "backup at /home/u/proj.old/a.cc",
          "CanonicalizeText treats '.' as part of the name, not a terminator, "
          "when it is not sentence-final");


  // The left edge of a match. find() has no notion of where a path begins, so
  // a root also matched in the middle of an unrelated absolute path:
  // "/opt/home/u/proj/x.c" became "/opt/vcache/proj/x.c". These pin the
  // rejection.
  CheckEq(roots.CanonicalizeText("/opt/home/u/proj/x.c"),
          "/opt/home/u/proj/x.c",
          "CanonicalizeText ignores a root matched inside a longer path");
  CheckEq(roots.CanonicalizeText("in /mnt/backup/home/u/proj/a.cc:3"),
          "in /mnt/backup/home/u/proj/a.cc:3",
          "CanonicalizeText ignores a mid-path match after whitespace");
  CheckEq(roots.LocalizeText("/opt/vcache/proj/x.c"), "/opt/vcache/proj/x.c",
          "LocalizeText ignores a canonical root matched inside a longer path");

  // The other direction matters more: rejecting the left edge must not start
  // declining legitimate references, because an unrewritten root is a local
  // absolute path persisted into the shared cache. A match preceded by a run
  // holding no '/' -- a compiler flag -- still begins a path.
  CheckEq(roots.CanonicalizeText("-I/home/u/proj/include"),
          "-I/vcache/proj/include",
          "CanonicalizeText still rewrites a root after -I");
  CheckEq(roots.CanonicalizeText("--sysroot=/home/u/proj"),
          "--sysroot=/vcache/proj",
          "CanonicalizeText still rewrites a root after a flag's '='");
  CheckEq(roots.CanonicalizeText("cannot open '/home/u/proj/a.h'"),
          "cannot open '/vcache/proj/a.h'",
          "CanonicalizeText still rewrites a quoted root");
  CheckEq(roots.CanonicalizeText("see [/home/u/proj]"),
          "see [/vcache/proj]",
          "CanonicalizeText still rewrites a bracketed root");
  CheckEq(roots.CanonicalizeText("/home/u/proj/a.cc:1: note"),
          "/vcache/proj/a.cc:1: note",
          "CanonicalizeText still rewrites a root at the start of the text");

  // The same walk makes the rule correct rather than merely convenient: with
  // root "/proj", the path "/home/u/proj/a.cc" is not under that root, and
  // Canonicalize() has always said so. Text mode now agrees.
  {
    core::RootMap short_root = MakeRoots({"/proj=proj"});
    CheckEq(short_root.Canonicalize("/home/u/proj/a.cc"), "/home/u/proj/a.cc",
            "Canonicalize: /home/u/proj is not under root /proj");
    CheckEq(short_root.CanonicalizeText("error in /home/u/proj/a.cc"),
            "error in /home/u/proj/a.cc",
            "CanonicalizeText agrees: /home/u/proj is not under root /proj");
    CheckEq(short_root.CanonicalizeText("error in /proj/a.cc"),
            "error in /vcache/proj/a.cc",
            "CanonicalizeText still rewrites a genuine match on root /proj");
  }

  // Policy parsing. The default is error, so a typo must not silently fall
  // back to a permissive setting.
  core::IncomingMapPolicy policy = core::IncomingMapPolicy::kKeep;
  Check(core::ParseIncomingMapPolicy("error", &policy) &&
            policy == core::IncomingMapPolicy::kError,
        "parses policy 'error'");
  Check(core::ParseIncomingMapPolicy("STRIP", &policy) &&
            policy == core::IncomingMapPolicy::kStrip,
        "policy parsing is case-insensitive");
  Check(core::ParseIncomingMapPolicy(" keep ", &policy) &&
            policy == core::IncomingMapPolicy::kKeep,
        "policy parsing trims whitespace");
  core::IncomingMapPolicy unchanged = core::IncomingMapPolicy::kError;
  Check(!core::ParseIncomingMapPolicy("nonsense", &unchanged) &&
            unchanged == core::IncomingMapPolicy::kError,
        "an unknown policy is rejected and leaves the value alone");
  CheckEq(core::IncomingMapPolicyName(core::IncomingMapPolicy::kError), "error",
          "policy name round-trips");
  Check(core::Config{}.incoming_map_policy == core::IncomingMapPolicy::kError,
        "the default policy is error");

  Check(core::IsPrefixMapFlag("-ffile-prefix-map=/a=/b"), "detects -ffile-prefix-map");
  Check(core::IsPrefixMapFlag("-fdebug-prefix-map=/a=/b"), "detects -fdebug-prefix-map");
  Check(core::IsPrefixMapFlag("--remap-path-prefix"), "detects bare --remap-path-prefix");
  Check(!core::IsPrefixMapFlag("-fPIC"), "does not misdetect ordinary flags");
}

void TestDepFile() {
  Section("core::depfile");

  const std::string gcc_style =
      "obj/a.o: src/a.cc \\\n /usr/include/stdio.h \\\n inc/a.h\n";
  auto dep = core::ParseDepFile(gcc_style);
  Check(dep.has_value(), "parses a gcc dependency file");
  if (dep) {
    Check(dep->rules.size() == 1, "one rule");
    CheckEq(dep->rules[0].targets[0], "obj/a.o", "target");
    Check(dep->rules[0].prerequisites.size() == 3, "three prerequisites");
  }

  // -MP appends phony targets with no prerequisites.
  auto mp = core::ParseDepFile("a.o: a.c b.h\n\nb.h:\n");
  Check(mp && mp->rules.size() == 2, "parses -MP phony rules");
  Check(mp && mp->rules[1].prerequisites.empty(), "phony rule has no prerequisites");

  // Escaped spaces must survive a full round trip.
  auto esc = core::ParseDepFile("out.o: /path/with\\ space/f.c\n");
  Check(esc && esc->rules[0].prerequisites[0] == "/path/with space/f.c",
        "unescapes backslash-escaped spaces");
  if (esc) {
    auto again = core::ParseDepFile(core::RenderDepFile(*esc));
    Check(again && again->rules[0].prerequisites[0] == "/path/with space/f.c",
          "re-escapes spaces on render");
  }

  // Rendering must not wrap. cargo reads dep-info a line at a time and treats a
  // token-final backslash as an escaped space in a filename, so a continuation
  // aborts the build with "malformed dep-info format, trailing \".
  if (dep) {
    const std::string rendered = core::RenderDepFile(*dep);
    Check(rendered.find("\\\n") == std::string::npos,
          "rendered rules carry no line continuations");
    CheckEq(rendered, "obj/a.o: src/a.cc /usr/include/stdio.h inc/a.h\n",
            "a wrapped gcc depfile renders as one line");
  }

  // A literal dollar is written as "$$".
  auto dollar = core::ParseDepFile("out.o: /p/a$$b.c\n");
  Check(dollar && dollar->rules[0].prerequisites[0] == "/p/a$b.c",
        "decodes $$ as a literal dollar");

  Check(!core::ParseDepFile("this is not a makefile rule").has_value(),
        "rejects input with no rule");

  // Canonicalise then localise must be the identity.
  core::RootMap roots = MakeRoots({"/home/u/proj=proj"});
  auto rt = core::ParseDepFile("/home/u/proj/a.o: /home/u/proj/src/a.cc /usr/include/x.h\n");
  Check(rt.has_value(), "parses absolute-path rule");
  if (rt) {
    core::DepFile original = *rt;
    core::RemapDepFile(&*rt, roots, core::MapDirection::kCanonicalize);
    Check(core::RenderDepFile(*rt).find("/vcache/proj/src/a.cc") != std::string::npos,
          "canonicalises prerequisites");
    Check(core::RenderDepFile(*rt).find("/usr/include/x.h") != std::string::npos,
          "leaves system headers untouched");
    core::RemapDepFile(&*rt, roots, core::MapDirection::kLocalize);
    CheckEq(core::RenderDepFile(*rt), core::RenderDepFile(original),
            "canonicalise/localise round-trips exactly");
  }

  // rustc ends its dep-info with the environment variables the crate read.
  const std::string rustc_style =
      "out/demo.d: src/lib.rs src/helper.rs\n"
      "\n"
      "src/lib.rs:\n"
      "\n"
      "# env-dep:DEMO_UNSET\n"
      "# env-dep:CARGO_PKG_VERSION=0.4.2\n"
      "# env-dep:DEMO_ARGS=a=b c\n";
  auto rs = core::ParseDepFile(rustc_style);
  Check(rs.has_value(), "parses rustc dep-info with env-dep lines");
  if (rs) {
    std::vector<std::string> prereqs;
    for (const core::DepRule& rule : rs->rules) {
      prereqs.insert(prereqs.end(), rule.prerequisites.begin(), rule.prerequisites.end());
    }
    CheckEq(util::Join(prereqs, " "), "src/lib.rs src/helper.rs",
            "env-dep lines contribute no prerequisites");
    Check(rs->rules.size() == 2, "env-dep lines are not rules");
  }
}

void TestDepFileEnvDeps() {
  Section("core::depfile env-dep");

  const std::string text =
      "out/demo.d: src/lib.rs\n"
      "\n"
      "# env-dep:DEMO_UNSET\n"
      "# env-dep:CARGO_PKG_VERSION=0.4.2\n"
      "# env-dep:DEMO_ARGS=a=b c:d\n"
      "# env-dep:DEMO_EMPTY=\n";
  auto dep = core::ParseDepFile(text);
  Check(dep.has_value(), "parses");
  if (!dep) return;
  Check(dep->env_deps.size() == 4, "four env deps, in file order");
  if (dep->env_deps.size() != 4) return;
  CheckEq(dep->env_deps[0].name, "DEMO_UNSET", "unset: name");
  Check(!dep->env_deps[0].value.has_value(), "unset: no value");
  CheckEq(dep->env_deps[1].name, "CARGO_PKG_VERSION", "set: name");
  CheckEq(dep->env_deps[1].value.value_or("<none>"), "0.4.2", "set: value");
  CheckEq(dep->env_deps[2].name, "DEMO_ARGS", "value splits at the first '='");
  CheckEq(dep->env_deps[2].value.value_or("<none>"), "a=b c:d",
          "value keeps later '=', spaces and ':' verbatim");
  Check(dep->env_deps[3].value.has_value() && dep->env_deps[3].value->empty(),
        "empty value is set, not unset");
  Check(dep->rules.size() == 1 && dep->rules[0].prerequisites.size() == 1,
        "sources exclude env deps");

  // cargo reads the env-dep lines back from the restored file to decide when
  // the crate is stale, so they must survive a round trip byte for byte.
  CheckEq(core::RenderDepFile(*dep), text, "env-dep lines render verbatim");
  core::RootMap roots = MakeRoots({"/home/u/proj=proj"});
  auto pathy = core::ParseDepFile("a.d: /home/u/proj/a.rs\n\n# env-dep:OUT_DIR=/home/u/proj/out\n");
  Check(pathy.has_value(), "parses a path-valued env dep");
  if (pathy) {
    core::RemapDepFile(&*pathy, roots, core::MapDirection::kCanonicalize);
    CheckEq(pathy->env_deps.empty() ? "" : pathy->env_deps[0].value.value_or(""),
            "/home/u/proj/out", "remapping leaves env values raw");
    core::RemapDepFile(&*pathy, roots, core::MapDirection::kCanonicalize, {"OUT_DIR"});
    CheckEq(pathy->env_deps.empty() ? "" : pathy->env_deps[0].value.value_or(""),
            "/vcache/proj/out", "a listed path env value is canonicalised");
    core::RemapDepFile(&*pathy, MakeRoots({"/work/b=proj"}), core::MapDirection::kLocalize,
                       {"OUT_DIR"});
    CheckEq(pathy->env_deps.empty() ? "" : pathy->env_deps[0].value.value_or(""),
            "/work/b/out", "a listed path env value is localised on restore");
  }

  // cargo compares the restored env-dep value with its own OUT_DIR as a string,
  // so a root reached through two spellings must restore the one cargo uses.
  auto scratch = util::MakeTempDir("vcache-depfile-");
  Check(scratch.has_value(), "depfile scratch dir");
  if (!scratch) return;
  struct ScratchGuard {
    std::string path;
    ~ScratchGuard() { util::RemoveRecursive(path); }
  } guard{*scratch};
  const std::string base = util::RealPath(*scratch).value_or(*scratch);
  const std::string resolved_tree = base + "/resolved-tree";
  const std::string alias_tree = base + "/l";
  util::MakeDirs(resolved_tree + "/out");
  std::error_code link_ec;
  fs::create_directory_symlink(resolved_tree, alias_tree, link_ec);
  Check(!link_ec, "alias symlink created");
  const core::RootMap aliased = MakeRoots({alias_tree + "=proj"});
  auto localized_out_dir = [&](const char* current) {
    if (current == nullptr) {
      ::unsetenv("OUT_DIR");
    } else {
      ::setenv("OUT_DIR", current, 1);
    }
    auto dep = core::ParseDepFile("a.d: /vcache/proj/a.rs\n\n# env-dep:OUT_DIR=/vcache/proj/out\n");
    if (!dep || dep->env_deps.empty()) return std::string("unparsed");
    core::RemapDepFile(&*dep, aliased, core::MapDirection::kLocalize, {"OUT_DIR"});
    ::unsetenv("OUT_DIR");
    return dep->env_deps[0].value.value_or("unset");
  };
  CheckEq(localized_out_dir((resolved_tree + "/out").c_str()), resolved_tree + "/out",
          "the resolved spelling of OUT_DIR is restored as cargo set it");
  CheckEq(localized_out_dir((alias_tree + "/out").c_str()), alias_tree + "/out",
          "the alias spelling of OUT_DIR is restored as cargo set it");
  CheckEq(localized_out_dir("/elsewhere/out"), aliased.Localize("/vcache/proj/out"),
          "an OUT_DIR of another tree does not replace the stored value");
  CheckEq(localized_out_dir(nullptr), aliased.Localize("/vcache/proj/out"),
          "an unset OUT_DIR restores through the roots");
}

void TestPreprocessedNormalization() {
  Section("core::preprocessed");

  core::RootMap roots = MakeRoots({"/home/u/proj=proj"});

  CheckEq(core::NormalizeLinemarker("# 1 \"/home/u/proj/src/a.cc\"", roots),
          "# 1 \"/vcache/proj/src/a.cc\"", "rewrites a linemarker path");
  CheckEq(core::NormalizeLinemarker("# 12 \"/home/u/proj/inc/a.h\" 1 3 4", roots),
          "# 12 \"/vcache/proj/inc/a.h\" 1 3 4", "preserves linemarker flags");
  CheckEq(core::NormalizeLinemarker("# 0 \"<built-in>\"", roots),
          "# 0 \"<built-in>\"", "leaves pseudo-files alone");
  CheckEq(core::NormalizeLinemarker("#pragma once", roots), "#pragma once",
          "leaves directives that are not linemarkers alone");
  CheckEq(core::NormalizeLinemarker("int x = 1;", roots), "int x = 1;",
          "leaves ordinary code alone");
  // A '#' line whose path lies outside every root must be untouched.
  CheckEq(core::NormalizeLinemarker("# 3 \"/usr/include/stdio.h\" 2", roots),
          "# 3 \"/usr/include/stdio.h\" 2", "leaves system headers alone");
  // Malformed input must not corrupt the stream.
  CheckEq(core::NormalizeLinemarker("# 5 \"unterminated", roots),
          "# 5 \"unterminated", "leaves an unterminated path alone");

  // .incbin makes the assembler read a file the preprocessed text never
  // mentions, so it has to be recognised wherever it is spelled.
  Check(core::ContainsIncbin("\t.incbin \"kernel/kheaders_data.tar.xz\"\n"),
        "finds a bare .incbin in preprocessed assembler");
  Check(core::ContainsIncbin(
            "\"\\t.incbin \\\"kernel/config_data.gz\\\"\\t\\n\""),
        "finds .incbin inside an asm() string literal");
  Check(!core::ContainsIncbin("int incbin = 1;"),
        "does not fire without the leading dot");
  Check(!core::ContainsIncbin("static void inc(void);"),
        "does not fire on ordinary code");
}

void TestManifestMerge() {
  Section("core::manifest merge");

  struct State {
    std::string key;
  };
  auto show = [](const std::vector<State>& states) {
    std::string out;
    for (const State& state : states) {
      if (!out.empty()) out.push_back(' ');
      out += state.key;
    }
    return out;
  };

  const std::vector<State> already{State{"F"}, State{"A"}};
  const std::vector<State> extra{State{"B"}};
  CheckEq(show(core::MergeManifestStates(State{"F"}, already, extra, &State::key)), "F A B",
          "a state already re-read stays one copy at the front");

  const std::vector<State> loaded{State{"A"}, State{"B"}};
  CheckEq(show(core::MergeManifestStates(State{"F"}, {}, loaded, &State::key)), "F A B",
          "an empty re-read still keeps the loaded states");

  const std::vector<State> reread{State{"A"}};
  CheckEq(show(core::MergeManifestStates(State{"F"}, reread, {}, &State::key)), "F A",
          "an empty loaded list keeps the re-read behind fresh");

  const std::vector<State> both_reread{State{"A"}, State{"C"}};
  const std::vector<State> both_loaded{State{"A"}, State{"B"}};
  CheckEq(show(core::MergeManifestStates(State{"F"}, both_reread, both_loaded, &State::key)),
          "F A C B", "a state in both lists is kept once, from the re-read");

  std::vector<State> full;
  for (int i = 1; i <= 8; ++i) full.push_back(State{"R" + std::to_string(i)});
  const std::vector<State> tail{State{"L1"}};
  CheckEq(show(core::MergeManifestStates(State{"F"}, full, tail, &State::key)),
          "F R1 R2 R3 R4 R5 R6 R7",
          "a full re-read stays ahead of loaded-only states");

  struct DepState {
    std::string result_key;
  };
  const std::vector<DepState> dep_loaded{DepState{"L"}};
  const std::vector<DepState> dep_merged =
      core::MergeManifestStates(DepState{"F"}, {}, dep_loaded, &DepState::result_key);
  Check(dep_merged.size() == 2 && dep_merged[0].result_key == "F" &&
            dep_merged[1].result_key == "L",
        "a dep-scan merge keeps a loaded-only state");
}

void TestManifestMediaCount() {
  Section("core::manifest media");
  auto scratch = util::MakeTempDir("vcache-manifest-media-");
  Check(scratch.has_value(), "manifest media scratch exists");
  if (!scratch) return;
  struct ScratchGuard {
    std::string path;
    ~ScratchGuard() { util::RemoveRecursive(path); }
  } guard{*scratch};

  const bool reread_failed =
      core::ReportCacheMediaErrors({"disk: read failed"}, *scratch, true);
  core::ReportCacheMediaErrors({"disk: write failed"}, *scratch, !reread_failed);
  const core::Stats stats = core::ReadStats(*scratch);
  Check(reread_failed, "the re-read failure is reported");
  CheckEq(std::to_string(stats.Get(core::Counter::kCacheMediaError)), "1",
          "a put that fails after a failed re-read is not counted again");
}

void TestManifestPause() {
  Section("core::manifest pause");
  auto scratch = util::MakeTempDir("vcache-manifest-pause-");
  Check(scratch.has_value(), "manifest pause scratch exists");
  if (!scratch) return;
  struct ScratchGuard {
    std::string path;
    ~ScratchGuard() { util::RemoveRecursive(path); }
  } guard{*scratch};

  const std::string log_path = *scratch + "/pause.log";
  ::setenv("VCACHE_LOG", log_path.c_str(), 1);
  util::InitLogging();
  auto log_text = [&]() {
    const auto text = util::ReadFile(log_path);
    return text ? *text : std::string();
  };
  auto clear_log = [&]() {
    FILE* f = ::fopen(log_path.c_str(), "w");
    if (f != nullptr) ::fclose(f);
  };

  clear_log();
  ::setenv("VCACHE_TEST_PAUSE_BEFORE_MANIFEST_PUT", (*scratch + "/missing").c_str(), 1);
  core::PauseBeforeManifestPut();
  Check(log_text().find("waiting before put") == std::string::npos,
        "a missing pause path does not wait");

  const std::string regular = *scratch + "/regular";
  util::WriteFileAtomic(regular, "x");
  clear_log();
  ::setenv("VCACHE_TEST_PAUSE_BEFORE_MANIFEST_PUT", regular.c_str(), 1);
  core::PauseBeforeManifestPut();
  Check(log_text().find("waiting before put") == std::string::npos,
        "a regular pause path does not wait");

  clear_log();
  ::setenv("VCACHE_TEST_PAUSE_BEFORE_MANIFEST_PUT", scratch->c_str(), 1);
  core::PauseBeforeManifestPut();
  Check(log_text().find("waiting before put") == std::string::npos,
        "a directory pause path does not wait");

  const std::string fifo = *scratch + "/pause.fifo";
  Check(::mkfifo(fifo.c_str(), 0600) == 0, "the pause fifo exists");
  clear_log();
  ::setenv("VCACHE_TEST_PAUSE_BOUND_MS", "500", 1);
  ::setenv("VCACHE_TEST_PAUSE_BEFORE_MANIFEST_PUT", fifo.c_str(), 1);
  const auto started = std::chrono::steady_clock::now();
  core::PauseBeforeManifestPut();
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();
  ::unsetenv("VCACHE_TEST_PAUSE_BEFORE_MANIFEST_PUT");
  ::unsetenv("VCACHE_TEST_PAUSE_BOUND_MS");
  Check(log_text().find("manifest: test pause timed out") != std::string::npos,
        "a fifo with no writer times out");
  Check(elapsed_ms >= 400 && elapsed_ms < 2000,
        "a fifo with no writer returns at the test bound (took " + std::to_string(elapsed_ms) +
            "ms)");
}

void TestScratchDirs() {
  Section("util::MakeScratchDir");

  auto cache = util::MakeTempDir("vcache-scratch-test-");
  Check(cache.has_value(), "scratch test cache exists");
  if (!cache) return;
  struct ScratchGuard {
    std::string path;
    ~ScratchGuard() { util::RemoveRecursive(path); }
  } guard{*cache};

  const auto scratch = util::MakeScratchDir(*cache, "vcache-");
  Check(scratch.has_value() && util::StartsWith(*scratch, *cache + "/tmp/vcache-") &&
            util::IsDirectory(*scratch),
        "a compile's scratch directory is created under <cache>/tmp");

  const std::string not_a_dir = *cache + "/plain-file";
  util::WriteFileAtomic(not_a_dir, "x");
  const auto fallback = util::MakeScratchDir(not_a_dir, "vcache-");
  Check(fallback.has_value() && !util::StartsWith(*fallback, not_a_dir) && util::IsDirectory(*fallback),
        "a cache dir that cannot hold tmp/ falls back to the system temp dir");
  if (fallback) util::RemoveRecursive(*fallback);

  const auto stale = util::MakeScratchDir(*cache, "vcache-");
  const auto fresh = util::MakeScratchDir(*cache, "vcache-");
  Check(stale.has_value() && fresh.has_value(), "two scratch directories created");
  if (!stale || !fresh) return;
  util::WriteFileAtomic(*stale + "/pp.i", "left by a killed compile");
  const timeval seven_hours_ago[2] = {{::time(nullptr) - 7 * 3600, 0}, {::time(nullptr) - 7 * 3600, 0}};
  ::utimes(stale->c_str(), seven_hours_ago);
  const std::string stale_file = *cache + "/tmp/stray-file";
  util::WriteFileAtomic(stale_file, "x");
  ::utimes(stale_file.c_str(), seven_hours_ago);

  Check(util::RemoveStaleScratchDirs(*cache, 6 * 3600) == 1,
        "the sweep removes exactly the one old directory");
  Check(!util::IsDirectory(*stale), "a scratch directory older than the limit is removed");
  Check(util::IsDirectory(*fresh), "a scratch directory in use is kept");
  Check(util::FileExists(stale_file), "a plain file under tmp/ is left alone");
  Check(util::RemoveStaleScratchDirs(*cache + "/missing", 0) == 0,
        "a cache without tmp/ sweeps nothing");
}

void TestReadFile() {
  Section("util::ReadFile");

  auto scratch = util::MakeTempDir("vcache-readfile-");
  Check(scratch.has_value(), "readfile scratch exists");
  if (!scratch) return;
  struct ScratchGuard {
    std::string path;
    ~ScratchGuard() { util::RemoveRecursive(path); }
  } guard{*scratch};

  const std::string empty_path = *scratch + "/empty";
  util::WriteFileAtomic(empty_path, "");
  const auto empty = util::ReadFile(empty_path);
  Check(empty.has_value() && empty->empty(), "an empty file reads as an empty string");

  const std::string three_path = *scratch + "/three";
  util::WriteFileAtomic(three_path, "abc");
  const auto three = util::ReadFile(three_path);
  Check(three.has_value() && *three == "abc", "a 3-byte file reads back those bytes");

  const std::string big_path = *scratch + "/big";
  std::string pattern(64ull << 20, '\0');
  for (size_t i = 0; i < pattern.size(); ++i) {
    pattern[i] = static_cast<char>(i * 131u);
  }
  Check(util::WriteFileAtomic(big_path, pattern), "the 64 MiB fixture was written");
  const auto big = util::ReadFile(big_path);
  Check(big.has_value() && big->size() == pattern.size(), "a 64 MiB file reads back its size");
  const auto file_hash = hash::HashFile(big_path);
  Check(big.has_value() && file_hash.has_value() && hash::HashString(*big) == *file_hash,
        "a 64 MiB file matches hash::HashFile");

  Check(!util::ReadFile(*scratch + "/missing").has_value(), "a missing path reads as nullopt");

  const auto status = util::ReadFile("/proc/self/status");
  Check(status.has_value() && !status->empty() && status->rfind("Name:", 0) == 0,
        "/proc/self/status is non-empty and starts with Name:");
}

void TestBlob() {
  Section("storage::Blob");

  storage::Blob blob;
  blob.object = std::string("\x7f", 1) + "ELF binary bytes\x00 with NULs";
  blob.depfile = "a.o: a.c\n";
  blob.has_depfile = true;
  blob.stderr_text = "warning: unused variable\n";
  blob.meta = "compiler: g++\n";
  blob.files.push_back(storage::BlobFile{"libdemo.rlib", "rlib bytes"});
  blob.files.push_back(storage::BlobFile{"demo.d", "demo.d: lib.rs\n"});
  // cargo's build scripts come back out of the cache and are then executed.
  blob.files.push_back(
      storage::BlobFile{"build-script-build", "\x7f" "ELF", /*executable=*/true});

  const std::string encoded = storage::SerializeBlob(blob);
  storage::Blob decoded;
  Check(storage::DeserializeBlob(encoded, &decoded), "round-trips");
  CheckEq(decoded.object, blob.object, "object survives NUL bytes");
  CheckEq(decoded.depfile, blob.depfile, "depfile survives");
  CheckEq(decoded.stderr_text, blob.stderr_text, "stderr survives");
  Check(decoded.files.size() == 3, "every file survives");
  if (decoded.files.size() == 3) {
    CheckEq(decoded.files[0].name, "libdemo.rlib", "file name survives");
    CheckEq(decoded.files[1].contents, "demo.d: lib.rs\n", "file contents survive");
    Check(!decoded.files[0].executable, "a plain file stays non-executable");
    Check(decoded.files[2].executable, "the execute bit survives");
    CheckEq(decoded.files[2].contents, "\x7f" "ELF", "an executable's contents survive");
  }

  // Corruption must read as a miss rather than yielding a bad object.
  std::string corrupt = encoded;
  corrupt[corrupt.size() / 2] ^= 0xff;
  storage::Blob ignored;
  Check(!storage::DeserializeBlob(corrupt, &ignored), "detects a flipped bit");
  Check(!storage::DeserializeBlob(encoded.substr(0, encoded.size() - 5), &ignored),
        "detects truncation");
  Check(!storage::DeserializeBlob("garbage", &ignored), "rejects a bad magic");
  Check(!storage::DeserializeBlob("", &ignored), "rejects empty input");

  storage::Blob two;
  two.files.push_back(storage::BlobFile{"a.rlib", "aaa"});
  two.files.push_back(storage::BlobFile{"b.rmeta", "bbb"});
  const std::string two_enc = storage::SerializeBlob(two);
  storage::Blob two_back;
  Check(storage::DeserializeBlob(two_enc, &two_back) && two_back.files.size() == 2 &&
            two_back.files[0].name == "a.rlib" && two_back.files[0].contents == "aaa" &&
            two_back.files[1].name == "b.rmeta" && two_back.files[1].contents == "bbb",
        "a two-file blob round-trips byte-exact");
  const auto body_byte = two_enc.find("aaa");
  Check(body_byte != std::string::npos, "the two-file body contains its payload");
  if (body_byte != std::string::npos) {
    std::string flipped = two_enc;
    flipped[body_byte] = static_cast<char>(flipped[body_byte] ^ 0x01);
    Check(!storage::DeserializeBlob(flipped, &ignored),
          "flipping one body byte is rejected");
  }
}

// An in-memory layer, so the chain's fan-out and backfill rules can be checked
// without a disk or a network. `fail` models a layer that is configured and
// reachable but refuses the write -- an S3 PUT that times out, say.
class FakeStorage : public storage::Storage {
 public:
  FakeStorage(std::string name, bool writable = true, bool fail = false)
      : name_(std::move(name)), writable_(writable), fail_(fail) {}

  std::string Name() const override { return name_; }

  bool Get(const std::string& key, std::string* value) override {
    ++gets;
    ClearError();
    if (fail_) {
      SetError("simulated read failure");
      return false;
    }
    auto it = entries_.find(key);
    if (it == entries_.end()) return false;  // a plain miss: not an error
    *value = it->second;
    return true;
  }

  bool Put(const std::string& key, const std::string& value) override {
    ++puts;
    ClearError();
    if (fail_) {
      SetError("simulated write failure");
      return false;
    }
    entries_[key] = value;
    return true;
  }

  bool writable() const override { return writable_; }

  void Seed(const std::string& key, const std::string& value) {
    entries_[key] = value;
  }
  bool Has(const std::string& key) const { return entries_.count(key) != 0; }

  int gets = 0;
  int puts = 0;

 private:
  std::string name_;
  bool writable_;
  bool fail_;
  std::map<std::string, std::string> entries_;
};

void TestCacheChain() {
  Section("storage::CacheChain");

  // A store must reach every writable layer. This is what makes a compile on
  // one machine visible to every other one: there is no later promotion step
  // that would move a local-only entry up to the shared layer.
  {
    auto disk = std::make_unique<FakeStorage>("disk");
    auto s3 = std::make_unique<FakeStorage>("s3");
    FakeStorage* disk_ptr = disk.get();
    FakeStorage* s3_ptr = s3.get();

    storage::CacheChain chain;
    chain.AddLayer(std::move(disk));
    chain.AddLayer(std::move(s3));

    Check(chain.Put("abcdef", "blob").stored, "a store that reaches a layer succeeds");
    Check(disk_ptr->Has("abcdef"), "the store reaches the local layer");
    Check(s3_ptr->Has("abcdef"), "the same store reaches the remote layer");
  }

  // One layer failing must not cost the other: a build with an unreachable
  // shared cache still fills its local one.
  {
    auto disk = std::make_unique<FakeStorage>("disk");
    auto s3 = std::make_unique<FakeStorage>("s3", /*writable=*/true, /*fail=*/true);
    FakeStorage* disk_ptr = disk.get();
    FakeStorage* s3_ptr = s3.get();

    storage::CacheChain chain;
    chain.AddLayer(std::move(disk));
    chain.AddLayer(std::move(s3));

    const storage::PutResult partial = chain.Put("abcdef", "blob");
    Check(partial.stored, "a partial store still reports success");
    Check(partial.errors.size() == 1, "and still reports the broken layer");
    Check(!partial.errors.empty() && partial.errors[0].find("s3:") == 0,
          "the error names the layer that failed");
    Check(disk_ptr->Has("abcdef"), "the working layer is written anyway");
    Check(s3_ptr->puts == 1, "the failing layer was still attempted");
  }

  // Every layer failing is a genuine store failure.
  {
    auto disk = std::make_unique<FakeStorage>("disk", /*writable=*/true, /*fail=*/true);
    storage::CacheChain chain;
    chain.AddLayer(std::move(disk));
    Check(!chain.Put("abcdef", "blob").stored, "a store that reaches no layer fails");
  }

  // A read-only layer is skipped before Put is even called -- the shape both
  // `no_credentials` and global read-only mode produce.
  {
    auto disk = std::make_unique<FakeStorage>("disk");
    auto s3 = std::make_unique<FakeStorage>("s3", /*writable=*/false);
    FakeStorage* disk_ptr = disk.get();
    FakeStorage* s3_ptr = s3.get();

    storage::CacheChain chain;
    chain.AddLayer(std::move(disk));
    chain.AddLayer(std::move(s3));

    const storage::PutResult ro = chain.Put("abcdef", "blob");
    Check(ro.stored, "a store with one read-only layer succeeds");
    Check(ro.errors.empty(), "a skipped read-only layer is not an error");
    Check(disk_ptr->Has("abcdef"), "the writable layer is still written");
    Check(s3_ptr->puts == 0, "a read-only layer is never asked to store");
  }

  // A hit in a slower layer is written back into every faster one it passed.
  {
    auto disk = std::make_unique<FakeStorage>("disk");
    auto s3 = std::make_unique<FakeStorage>("s3");
    FakeStorage* disk_ptr = disk.get();
    s3->Seed("abcdef", "blob");

    storage::CacheChain chain;
    chain.AddLayer(std::move(disk));
    chain.AddLayer(std::move(s3));

    storage::GetResult got = chain.Get("abcdef");
    Check(got.hit, "a remote-only entry is found");
    CheckEq(got.value, "blob", "the remote value is returned");
    CheckEq(got.layer, "s3", "the serving layer is reported");
    Check(disk_ptr->Has("abcdef"), "a remote hit backfills the local layer");
  }

  // The inverse, and the reason S3 is only ever populated by a machine that
  // actually compiled: backfill runs downward only. A local hit must not push
  // the entry up to the shared layer, and must not even look there.
  {
    auto disk = std::make_unique<FakeStorage>("disk");
    auto s3 = std::make_unique<FakeStorage>("s3");
    disk->Seed("abcdef", "blob");
    FakeStorage* s3_ptr = s3.get();

    storage::CacheChain chain;
    chain.AddLayer(std::move(disk));
    chain.AddLayer(std::move(s3));

    storage::GetResult got = chain.Get("abcdef");
    CheckEq(got.layer, "disk", "the local layer serves the hit");
    Check(s3_ptr->gets == 0, "a local hit stops the walk before the remote layer");
    Check(s3_ptr->puts == 0, "a local hit is never written up to the remote layer");
  }

  // Backfill must respect a faster layer that cannot be written.
  {
    auto disk = std::make_unique<FakeStorage>("disk", /*writable=*/false);
    auto s3 = std::make_unique<FakeStorage>("s3");
    FakeStorage* disk_ptr = disk.get();
    s3->Seed("abcdef", "blob");

    storage::CacheChain chain;
    chain.AddLayer(std::move(disk));
    chain.AddLayer(std::move(s3));

    Check(chain.Get("abcdef").hit, "a remote hit still serves with a read-only local layer");
    Check(disk_ptr->puts == 0, "backfill skips a read-only faster layer");
  }

  // The distinction the whole feature rests on: a cold layer reports nothing,
  // a broken one reports an error, and both still fall through to the next.
  {
    auto disk = std::make_unique<FakeStorage>("disk", /*writable=*/true, /*fail=*/true);
    auto s3 = std::make_unique<FakeStorage>("s3");
    s3->Seed("abcdef", "blob");

    storage::CacheChain chain;
    chain.AddLayer(std::move(disk));
    chain.AddLayer(std::move(s3));

    storage::GetResult got = chain.Get("abcdef");
    Check(got.hit, "a broken faster layer does not stop the lookup");
    CheckEq(got.layer, "s3", "the working layer still serves");
    Check(got.errors.size() >= 1, "the broken layer is reported");
    Check(!got.errors.empty() && got.errors[0].find("disk:") == 0,
          "the read error names the layer");
  }

  // A miss everywhere reports no layer.
  {
    storage::CacheChain chain;
    chain.AddLayer(std::make_unique<FakeStorage>("disk"));
    chain.AddLayer(std::make_unique<FakeStorage>("s3"));
    storage::GetResult got = chain.Get("abcdef");
    Check(!got.hit, "an entry in no layer is a miss");
    Check(got.layer.empty(), "a miss names no serving layer");
    Check(got.errors.empty(), "a miss is not a media error");
  }
}

// A remote that answers until told to break, as a daemon that dies mid-build
// would. Counts calls so a test can see the chain stop asking it.
class FakeRemote : public storage::RemoteCache {
 public:
  explicit FakeRemote(bool* broken) : broken_(broken) {}
  std::string Name() const override { return "daemon"; }
  bool Get(const std::string& key, storage::GetResult* result) override {
    if (*broken_) return false;
    auto it = entries_.find(key);
    if (it == entries_.end()) return true;
    result->hit = true;
    result->value = it->second;
    result->layer = "s3";
    return true;
  }
  bool Put(const std::string& key, const std::string& value,
           storage::PutResult* result) override {
    if (*broken_) return false;
    entries_[key] = value;
    result->stored = true;
    return true;
  }

 private:
  bool* broken_;
  std::map<std::string, std::string> entries_;
};

void TestCacheChainRemote() {
  Section("storage::CacheChain with a remote (daemon)");

  bool broken = false;
  auto remote = std::make_unique<FakeRemote>(&broken);
  FakeStorage* local_ptr = nullptr;
  int builds = 0;

  storage::CacheChain chain;
  chain.SetRemote(std::move(remote), [&](storage::CacheChain* c) {
    ++builds;
    auto local = std::make_unique<FakeStorage>("disk");
    local_ptr = local.get();
    c->AddLayer(std::move(local));
  });
  Check(!chain.empty(), "a chain with only a remote is not empty");
  Check(builds == 0, "local layers are not built while the remote answers");

  Check(chain.Put("abcdef", "blob").stored, "a store goes to the remote");
  const storage::GetResult hit = chain.Get("abcdef");
  Check(hit.hit && hit.value == "blob", "a lookup is answered by the remote");
  CheckEq(hit.layer, "s3", "and keeps the layer the remote reported, for stats");

  // The daemon dies. The compile must carry on with its own layers rather than
  // fail, and stop asking the dead remote.
  broken = true;
  const storage::GetResult miss = chain.Get("abcdef");
  Check(!miss.hit, "a lookup after the remote breaks is a miss, not an error");
  Check(miss.errors.empty(), "and reports no media error for the remote");
  Check(builds == 1 && local_ptr != nullptr, "the local layers are built once");
  Check(chain.Put("abcdef", "blob2").stored, "a store after that reaches the local layer");
  Check(local_ptr->Has("abcdef"), "which now holds the entry");
  Check(!chain.remote(), "the broken remote is dropped");
  chain.Get("abcdef");
  Check(builds == 1, "and the local layers are not rebuilt");
}

void TestDaemonProtocol() {
  Section("daemon::protocol");

  {
    daemon::Writer w;
    w.U8(7);
    w.U64(0x0102030405060708ull);
    w.Str(std::string("a\0b", 3));
    w.StrList({"one", "", "three"});
    daemon::Reader r(w.data());
    uint8_t u8 = 0;
    uint64_t u64 = 0;
    std::string str;
    std::vector<std::string> list;
    Check(r.U8(&u8) && u8 == 7, "u8 round-trips");
    Check(r.U64(&u64) && u64 == 0x0102030405060708ull, "u64 round-trips");
    Check(r.Str(&str) && str == std::string("a\0b", 3),
          "a string with an embedded NUL round-trips");
    Check(r.StrList(&list) && list.size() == 3 && list[1].empty() && list[2] == "three",
          "a string list round-trips, empty elements included");
    Check(r.done(), "and the reader ends exactly at the end");
  }

  // Every truncation of a valid frame must be rejected, never read past.
  {
    daemon::Writer w;
    w.U8(2);
    w.Str("abcdef0123");
    w.StrList({"x", "yz"});
    const std::string full = w.data();
    bool all_rejected = true;
    for (size_t n = 0; n < full.size(); ++n) {
      const std::string cut = full.substr(0, n);
      daemon::Reader r(cut);
      uint8_t op = 0;
      std::string key;
      std::vector<std::string> list;
      const bool ok = r.U8(&op) && r.Str(&key) && r.StrList(&list) && r.done();
      if (ok) all_rejected = false;
    }
    Check(all_rejected, "every truncated frame is rejected");
  }

  // A length that claims more than the frame holds must fail without trying
  // to allocate it.
  {
    daemon::Writer w;
    w.U64(~0ull);
    daemon::Reader r(w.data());
    std::string s;
    Check(!r.Str(&s), "a string length past the end is rejected");
    daemon::Reader r2(w.data());
    std::vector<std::string> list;
    Check(!r2.StrList(&list), "a list count past the end is rejected");
  }

  // The fingerprint is what stops a daemon serving a client from the wrong
  // cache. Same configuration agrees; each setting that moves entries does not.
  {
    core::Config a;
    a.disk.dir = "/tmp/vcache-fp";
    core::Config b = a;
    CheckEq(daemon::FingerprintMismatch(daemon::ConfigFingerprint(a),
                                        daemon::ConfigFingerprint(b)),
            "", "identical configurations agree");

    b.disk.dir = "/tmp/vcache-other";
    const std::string why = daemon::FingerprintMismatch(daemon::ConfigFingerprint(a),
                                                        daemon::ConfigFingerprint(b));
    Check(util::StartsWith(why, "disk.dir:"), "a different cache directory is named: " + why);

    core::Config c = a;
    c.s3.enabled = true;
    c.s3.bucket = "bucket";
    core::Config d = c;
    d.s3.prefix = "other/";
    Check(util::StartsWith(daemon::FingerprintMismatch(daemon::ConfigFingerprint(c),
                                                       daemon::ConfigFingerprint(d)),
                           "s3.prefix:"),
          "a different s3 prefix is named");
    Check(!daemon::FingerprintMismatch(daemon::ConfigFingerprint(a),
                                       daemon::ConfigFingerprint(c)).empty(),
          "s3 on in one and off in the other disagrees");

    core::Config e = c;
    e.s3.access_key = "AKIDONE";
    core::Config f = c;
    f.s3.access_key = "AKIDTWO";
    Check(util::StartsWith(daemon::FingerprintMismatch(daemon::ConfigFingerprint(e),
                                                       daemon::ConfigFingerprint(f)),
                           "s3.identity:"),
          "different credentials disagree");
    Check(daemon::ConfigFingerprint(e).find("AKIDONE") == std::string::npos,
          "and the access key itself is not in the fingerprint");

    core::Config g = a;
    g.read_only = true;
    Check(!daemon::FingerprintMismatch(daemon::ConfigFingerprint(a),
                                       daemon::ConfigFingerprint(g)).empty(),
          "read-only disagrees, since the daemon backfills on the client's behalf");
  }

  // sockaddr_un holds about a hundred bytes; a deep cache directory falls back
  // to a short path rather than failing to bind.
  {
    core::Config shallow;
    shallow.disk.dir = "/tmp/c";
    CheckEq(daemon::SocketPath(shallow), "/tmp/c/daemon/sock",
            "a short cache directory keeps its socket inside it");

    core::Config deep;
    deep.disk.dir = "/tmp/" + std::string(150, 'd');
    const std::string path = daemon::SocketPath(deep);
    Check(path.size() < 100 && util::StartsWith(path, "/tmp/vcache-"),
          "a deep one falls back to a short path: " + path);
    core::Config deep2 = deep;
    deep2.disk.dir += "2";
    Check(daemon::SocketPath(deep2) != path, "and two deep caches still get two sockets");

    core::Config explicit_socket = deep;
    explicit_socket.daemon.socket = "/tmp/explicit.sock";
    CheckEq(daemon::SocketPath(explicit_socket), "/tmp/explicit.sock",
            "an explicit socket wins");
  }

  {
    core::DaemonMode mode = core::DaemonMode::kOff;
    Check(core::ParseDaemonMode("auto", &mode) && mode == core::DaemonMode::kAuto,
          "daemon mode 'auto' parses");
    Check(core::ParseDaemonMode("1", &mode) && mode == core::DaemonMode::kOn,
          "daemon mode '1' means on");
    Check(!core::ParseDaemonMode("sometimes", &mode), "an unknown mode is rejected");
  }
}

bool PollUntil(const std::function<bool()>& condition, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  do {
    if (condition()) return true;
    ::poll(nullptr, 0, 10);
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

struct SessionDaemon {
  core::Config config;
  std::string directory;
  pid_t pid = -1;

  explicit SessionDaemon(int idle_seconds = 0, bool single_flight = false,
                         bool block_put = false, uint64_t mem_available_kb = 0,
                         const std::map<std::string, std::string>& test_settings = {},
                         bool jobserver = false) {
    directory = util::MakeTempDir("vcache-session-test-").value_or("");
    if (directory.empty()) return;
    config.disk.dir = directory + "/cache";
    config.daemon.idle_timeout_seconds = idle_seconds;
    config.daemon.single_flight = single_flight;
    config.daemon.jobserver = jobserver;
    config.daemon.jobserver_jobs = 4;
    if (mem_available_kb > 0) {
      const std::string meminfo = "MemAvailable: " + std::to_string(mem_available_kb) + " kB\n";
      const int mem_fd = ::open((directory + "/meminfo").c_str(), O_WRONLY | O_CREAT, 0600);
      if (mem_fd < 0) return;
      ::write(mem_fd, meminfo.data(), meminfo.size());
      ::close(mem_fd);
    }
    if (block_put && ::mkfifo((directory + "/put-gate").c_str(), 0600) != 0) return;
    for (const auto& [variable, value] : test_settings) {
      if (variable.ends_with("_GATE") && ::mkfifo((directory + "/" + value).c_str(), 0600) != 0) {
        return;
      }
    }
    int ready_pipe[2];
    if (::pipe(ready_pipe) != 0) return;
    pid = ::fork();
    if (pid == 0) {
      ::close(ready_pipe[0]);
      for (const auto& [variable, value] : test_settings) {
        const std::string setting = value == "1" ? value : directory + "/" + value;
        ::setenv(variable.c_str(), setting.c_str(), 1);
      }
      if (block_put) {
        ::setenv("VCACHE_DAEMON_TEST_BLOCK_PUT", (directory + "/put-gate").c_str(), 1);
        ::setenv("VCACHE_LOG", (directory + "/put-log").c_str(), 1);
        util::InitLogging();
      }
      if (mem_available_kb > 0) {
        ::setenv("VCACHE_DAEMON_MEMINFO", (directory + "/meminfo").c_str(), 1);
        ::setenv("VCACHE_LOG", (directory + "/memory-log").c_str(), 1);
        util::InitLogging();
      }
      ::_exit(daemon::RunServer(config, ready_pipe[1]));
    }
    ::close(ready_pipe[1]);
    pollfd ready{ready_pipe[0], POLLIN, 0};
    char code = 0;
    Check(::poll(&ready, 1, 2000) > 0 && ::read(ready_pipe[0], &code, 1) == 1 &&
              code == 'R', "session daemon becomes ready within two seconds");
    ::close(ready_pipe[0]);
  }

  ~SessionDaemon() {
    if (pid > 0) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, nullptr, 0);
    }
    if (!directory.empty()) util::RemoveRecursive(directory);
  }

  std::string Status() const {
    std::string reply;
    const int fd = Hello(daemon::kProtocolVersion, &reply);
    if (fd < 0) return "";
    daemon::Writer request;
    request.U8(static_cast<uint8_t>(daemon::Op::kStatus));
    std::string text;
    if (daemon::SendFrame(fd, request.data()) && daemon::RecvFrame(fd, &reply)) {
      daemon::Reader in(reply);
      uint8_t status = 0;
      if (!in.U8(&status) || status != static_cast<uint8_t>(daemon::Status::kOk) ||
          !in.Str(&text) || !in.done()) text.clear();
    }
    ::close(fd);
    return text;
  }

  bool Sessions(size_t expected) const {
    const std::string status = Status();
    const size_t row = status.find("compile sessions");
    if (row == std::string::npos) return false;
    return std::strtoull(status.c_str() + row + 16, nullptr, 10) == expected;
  }

  int Hello(uint64_t version, std::string* reply) const {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    timeval timeout{0, 500000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string path = daemon::SocketPath(config);
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    daemon::Writer hello;
    hello.U8(static_cast<uint8_t>(daemon::Op::kHello));
    hello.U64(version);
    hello.Str(daemon::ConfigFingerprint(config));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        !daemon::SendFrame(fd, hello.data()) || !daemon::RecvFrame(fd, reply)) {
      ::close(fd);
      return -1;
    }
    return fd;
  }

  int Open(std::string* reply) const {
    const int fd = Hello(daemon::kProtocolVersion, reply);
    if (fd < 0) return -1;
    daemon::Writer request;
    request.U8(6);
    if (!daemon::SendFrame(fd, request.data()) || !daemon::RecvFrame(fd, reply)) {
      ::close(fd);
      return -1;
    }
    return fd;
  }
};

void TestCompileSessions() {
  Section("daemon::session");
  SessionDaemon server(1);
  std::string reply;
  int fd = server.Hello(1, &reply);
  daemon::Reader refused(reply);
  uint8_t status = 0;
  std::string reason;
  Check(fd >= 0 && refused.U8(&status) &&
            status == static_cast<uint8_t>(daemon::Status::kRefused) &&
            refused.Str(&reason) && reason.find("version 1") != std::string::npos &&
            reason.find("speaks 2") != std::string::npos,
        "v1 hello is refused and names client version 1 and daemon version 2");
  if (fd >= 0) ::close(fd);

  fd = server.Open(&reply);
  Check(fd >= 0 && reply == std::string(1, '\0'), "session open replies ok");
  Check(server.Sessions(1), "compile sessions 1 survives a separate status request");
  const auto idle_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1600);
  bool stayed_alive = true;
  PollUntil([&] {
    stayed_alive &= ::kill(server.pid, 0) == 0 && util::FileExists(daemon::SocketPath(server.config));
    return !stayed_alive || std::chrono::steady_clock::now() >= idle_deadline;
  }, 2000);
  Check(stayed_alive && server.Sessions(1), "an open session prevents the one-second idle exit");

#if defined(__linux__)
  std::string duplicate_reply;
  const int duplicate_fd = server.Open(&duplicate_reply);
  Check(duplicate_fd >= 0 && !duplicate_reply.empty() &&
            duplicate_reply[0] == static_cast<char>(daemon::Status::kError) &&
            server.Sessions(1), "a pid cannot hold two compile sessions");
  if (duplicate_fd >= 0) ::close(duplicate_fd);
#endif
  if (fd >= 0) ::close(fd);
  Check(PollUntil([&] { return server.Sessions(0); }, 1000),
        "closing the session fd reports compile sessions 0 within one second");

  const pid_t client_pid = ::fork();
  if (client_pid == 0) {
    std::string child_reply;
    const int child_fd = server.Open(&child_reply);
    if (child_fd < 0 || child_reply != std::string(1, '\0')) ::_exit(1);
    ::poll(nullptr, 0, 5000);
    ::_exit(2);
  }
  Check(client_pid > 0 && PollUntil([&] { return server.Sessions(1); }, 1000),
        "a separate client process opens a session");
  if (client_pid > 0) {
    ::kill(client_pid, SIGKILL);
    ::waitpid(client_pid, nullptr, 0);
  }
  Check(PollUntil([&] { return server.Sessions(0); }, 1000),
        "killing the client releases its session within one second");

  fd = server.Open(&reply);
  daemon::Writer shutdown;
  shutdown.U8(static_cast<uint8_t>(daemon::Op::kShutdown));
  int shutdown_fd = server.Hello(daemon::kProtocolVersion, &reply);
  const auto started = std::chrono::steady_clock::now();
  bool stopped = false;
  if (shutdown_fd >= 0 && daemon::SendFrame(shutdown_fd, shutdown.data())) {
    pollfd response{shutdown_fd, POLLIN, 0};
    stopped = ::poll(&response, 1, 2000) > 0 && daemon::RecvFrame(shutdown_fd, &reply) &&
              !reply.empty() && reply[0] == static_cast<char>(daemon::Status::kOk);
  }
  Check(stopped && std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
        "Shutdown returns promptly with an open session (under two seconds)");
  if (shutdown_fd >= 0) ::close(shutdown_fd);
  if (fd >= 0) {
    pollfd closed{fd, POLLIN, 0};
    Check(::poll(&closed, 1, 1000) > 0, "Shutdown immediately closes the held session");
    ::close(fd);
  }
}

void TestCompileSessionShutdownRace() {
  Section("daemon::session shutdown interleaving");
  SessionDaemon server;
  std::string reply;
  const int held_fd = server.Open(&reply);
  Check(held_fd >= 0 && reply == std::string(1, '\0'),
        "shutdown-race holder opens a session");
  int ready_pipe[2];
  int open_pipe[2];
  int result_pipe[2];
  if (::pipe(ready_pipe) != 0 || ::pipe(open_pipe) != 0 || ::pipe(result_pipe) != 0) {
    Check(false, "shutdown-race barriers can be created");
    if (held_fd >= 0) ::close(held_fd);
    return;
  }
  const pid_t opener_pid = ::fork();
  if (opener_pid == 0) {
    ::close(ready_pipe[0]);
    ::close(open_pipe[1]);
    ::close(result_pipe[0]);
    if (held_fd >= 0) ::close(held_fd);
    std::string child_reply;
    const int child_fd = server.Hello(daemon::kProtocolVersion, &child_reply);
    const char ready = child_fd >= 0 ? 'R' : 'E';
    ::write(ready_pipe[1], &ready, 1);
    pollfd open_gate{open_pipe[0], POLLIN, 0};
    char open = 0;
    bool refused = false;
    if (child_fd >= 0 && ::poll(&open_gate, 1, 2000) > 0 &&
        ::read(open_pipe[0], &open, 1) == 1) {
      daemon::Writer request;
      request.U8(static_cast<uint8_t>(daemon::Op::kSessionOpen));
      if (daemon::SendFrame(child_fd, request.data()) &&
          daemon::RecvFrame(child_fd, &child_reply)) {
        daemon::Reader in(child_reply);
        uint8_t status = 0;
        std::string reason;
        refused = in.U8(&status) && status == static_cast<uint8_t>(daemon::Status::kError) &&
                  in.Str(&reason) && reason == "daemon shutting down" && in.done();
      }
      ::close(child_fd);
    }
    const char outcome = refused ? 'R' : 'E';
    ::write(result_pipe[1], &outcome, 1);
    ::_exit(refused ? 0 : 1);
  }
  ::close(ready_pipe[1]);
  ::close(open_pipe[0]);
  ::close(result_pipe[1]);
  pollfd ready{ready_pipe[0], POLLIN, 0};
  char code = 0;
  const bool opener_ready = opener_pid > 0 && ::poll(&ready, 1, 2000) > 0 &&
                            ::read(ready_pipe[0], &code, 1) == 1 && code == 'R';
  Check(opener_ready, "shutdown-race opener completes hello before SIGTERM");
  if (server.pid > 0) ::kill(server.pid, SIGTERM);
  bool terminal_received = false;
  if (held_fd >= 0 && daemon::RecvFrame(held_fd, &reply)) {
    daemon::Reader in(reply);
    uint8_t status = 0;
    std::string reason;
    terminal_received = in.U8(&status) &&
                        status == static_cast<uint8_t>(daemon::Status::kError) &&
                        in.Str(&reason) && reason == "daemon shutting down" && in.done();
  }
  Check(terminal_received, "SIGTERM broadcasts a terminal session frame before the late open");
  const char open = 'O';
  if (opener_ready) ::write(open_pipe[1], &open, 1);
  pollfd result{result_pipe[0], POLLIN, 0};
  const bool refused = ::poll(&result, 1, 1000) > 0 &&
                       ::read(result_pipe[0], &code, 1) == 1 && code == 'R';
  Check(refused, "session open after SIGTERM broadcast replies daemon shutting down within 1 s");
  if (held_fd >= 0) ::close(held_fd);
  for (int fd : {ready_pipe[0], open_pipe[1], result_pipe[0]}) ::close(fd);
  if (opener_pid > 0) {
    const bool exited = PollUntil(
        [&] { return ::waitpid(opener_pid, nullptr, WNOHANG) == opener_pid; }, 1000);
    if (!exited) {
      ::kill(opener_pid, SIGKILL);
      ::waitpid(opener_pid, nullptr, 0);
    }
  }
}

void TestCompileSessionStalledSend() {
  Section("daemon::session send isolation");
  SessionDaemon server;
  std::string reply;
  const int held_fd = server.Open(&reply);
  if (held_fd < 0 || reply != std::string(1, '\0')) {
    Check(false, "send-isolation holder opens a session");
    if (held_fd >= 0) ::close(held_fd);
    return;
  }
  std::string flood;
  daemon::Writer request;
  request.U8(static_cast<uint8_t>(daemon::Op::kSessionOpen));
  daemon::Writer header;
  header.U64(request.data().size());
  for (int i = 0; i < 4096; ++i) flood.append(header.data() + request.data());
  size_t flood_offset = 0;
  bool queued_requests = false;
  const bool stalled = PollUntil([&] {
    int send_flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
    send_flags |= MSG_NOSIGNAL;
#endif
    const ssize_t sent = ::send(held_fd, flood.data() + flood_offset,
                                flood.size() - flood_offset, send_flags);
    if (sent > 0) flood_offset = (flood_offset + static_cast<size_t>(sent)) % flood.size();
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) queued_requests = true;
    int unread_bytes = 0;
    ::ioctl(held_fd, FIONREAD, &unread_bytes);
    return queued_requests && unread_bytes >= 2048;
  }, 500);
  Check(stalled, "session peer stops reading until its replies fill the socket");
  const auto started = std::chrono::steady_clock::now();
  const bool responsive = server.Sessions(1);
  Check(responsive && std::chrono::steady_clock::now() - started <
                          std::chrono::milliseconds(250),
        "a stalled session send leaves separate status requests responsive within 250 ms");
  ::close(held_fd);
  Check(PollUntil([&] { return server.Sessions(0); }, 1000),
        "stalled-send session closes with compile sessions 0");
}

struct LeasePeer {
  int control_fd = -1;
  pid_t pid = -1;
  uint8_t last_reason = 255;
  uint8_t last_outcome = 255;
  uint64_t last_holder_pid = 0;
  uint64_t last_waited_ms = 0;
  bool request_pending = false;
  pid_t compiler_pid = -1;
  uint64_t reservation_sequence = 0;

  explicit LeasePeer(const SessionDaemon& server) {
    int control[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, control) != 0) return;
    pid = ::fork();
    if (pid == 0) {
      ::close(control[0]);
      std::string reply;
      const int session_fd = server.Open(&reply);
      if (!daemon::SendFrame(control[1], reply) || session_fd < 0) ::_exit(1);
      timeval session_timeout{5, 0};
      ::setsockopt(session_fd, SOL_SOCKET, SO_RCVTIMEO, &session_timeout,
                   sizeof(session_timeout));
      pid_t worker_pid = -1;
      int worker_stop_fd = -1;
      while (daemon::RecvFrame(control[1], &reply)) {
        if (!reply.empty() && static_cast<uint8_t>(reply[0]) == 251) {
          std::string hello_reply;
          const int put_fd = server.Hello(daemon::kProtocolVersion, &hello_reply);
          char code = put_fd >= 0 ? 'R' : 'E';
          iovec payload{&code, 1};
          alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int))]{};
          msghdr message{};
          message.msg_iov = &payload;
          message.msg_iovlen = 1;
          if (put_fd >= 0) {
            message.msg_control = ancillary;
            message.msg_controllen = sizeof(ancillary);
            cmsghdr* descriptor = CMSG_FIRSTHDR(&message);
            descriptor->cmsg_level = SOL_SOCKET;
            descriptor->cmsg_type = SCM_RIGHTS;
            descriptor->cmsg_len = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(descriptor), &put_fd, sizeof(put_fd));
          }
          const bool sent = ::sendmsg(control[1], &message, 0) == 1;
          if (put_fd >= 0) ::close(put_fd);
          if (!sent) break;
          continue;
        }
        if (!reply.empty() && static_cast<uint8_t>(reply[0]) == 253) {
          daemon::Reader worker_request(reply);
          uint8_t code = 0;
          uint64_t rss_kb = 0, depth = 0;
          std::string mode;
          if (!worker_request.U8(&code) || !worker_request.U64(&rss_kb) ||
              !worker_request.U64(&depth) || !worker_request.Str(&mode) ||
              rss_kb > 65536 || depth > 2) ::_exit(2);
          int worker_ready[2], worker_stop[2];
          if (::pipe(worker_ready) != 0 || ::pipe(worker_stop) != 0) ::_exit(2);
          worker_pid = ::fork();
          if (worker_pid == 0) {
            ::setpgid(0, 0);
            ::close(control[1]);
            ::close(session_fd);
            ::close(worker_ready[0]);
            ::close(worker_stop[1]);
            if (depth > 0 || !mode.empty()) {
              const std::string helper = util::DirName(util::SelfPath().value_or("")) +
                                         "/vcache_test_alloc";
              const std::string mib = std::to_string(rss_kb / 1024);
              const std::string levels = std::to_string(depth);
              const std::string ready_fd = std::to_string(worker_ready[1]);
              const std::string stop_fd = std::to_string(worker_stop[0]);
              ::execl(helper.c_str(), helper.c_str(), mib.c_str(), levels.c_str(),
                      ready_fd.c_str(), stop_fd.c_str(),
                      mode.empty() ? nullptr : mode.c_str(), nullptr);
              ::_exit(2);
            }
            std::vector<char> memory(rss_kb * 1024, 'm');
            for (size_t page = 0; page < memory.size(); page += 4096) memory[page] = 'r';
            ::write(worker_ready[1], "R", 1);
            pollfd stop{worker_stop[0], POLLIN, 0};
            ::poll(&stop, 1, 10000);
            ::_exit(0);
          }
          ::close(worker_ready[1]);
          ::close(worker_stop[0]);
          worker_stop_fd = worker_stop[1];
          pollfd ready{worker_ready[0], POLLIN, 0};
          char code_ready = 0;
          const bool started = ::poll(&ready, 1, 1000) > 0 &&
                               ::read(worker_ready[0], &code_ready, 1) == 1;
          ::close(worker_ready[0]);
          daemon::Writer worker_reply;
          worker_reply.U64(started ? worker_pid : 0);
          if (!daemon::SendFrame(control[1], worker_reply.data())) break;
          continue;
        }
        if (!reply.empty() && static_cast<uint8_t>(reply[0]) == 250) {
          daemon::Reader signal(reply);
          uint8_t code = 0, byte = 0;
          const bool sent = signal.U8(&code) && signal.U8(&byte) && signal.done() &&
                            worker_stop_fd >= 0 && ::write(worker_stop_fd, &byte, 1) == 1;
          if (!daemon::SendFrame(control[1], std::string(1, sent ? '\0' : '\1'))) break;
          continue;
        }
        if (!reply.empty() && static_cast<uint8_t>(reply[0]) == 252) {
          if (worker_pid > 0) {
            ::kill(-worker_pid, SIGKILL);
            ::kill(worker_pid, SIGKILL);
            ::waitpid(worker_pid, nullptr, 0);
            worker_pid = -1;
          }
          if (worker_stop_fd >= 0) ::close(worker_stop_fd);
          worker_stop_fd = -1;
          if (!daemon::SendFrame(control[1], std::string(1, '\0'))) break;
          continue;
        }
        if (!reply.empty() && reply[0] == '\0') {
          daemon::Reader probe(reply);
          uint8_t code = 0;
          uint64_t timeout_ms = 0;
          if (!probe.U8(&code) || !probe.U64(&timeout_ms) || !probe.done()) ::_exit(2);
          pollfd readable{session_fd, POLLIN, 0};
          const bool quiet = ::poll(&readable, 1, static_cast<int>(timeout_ms)) == 0;
          if (!daemon::SendFrame(control[1], std::string(1, quiet ? '\0' : '\1'))) break;
          continue;
        }
        if (!daemon::SendFrame(session_fd, reply) ||
            !daemon::RecvFrame(session_fd, &reply) ||
            !daemon::SendFrame(control[1], reply)) break;
      }
      ::close(session_fd);
      ::_exit(0);
    }
    ::close(control[1]);
    control_fd = control[0];
    timeval timeout{0, 500000};
    ::setsockopt(control_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(control_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    std::string reply;
    Check(pid > 0 && daemon::RecvFrame(control_fd, &reply) && reply == std::string(1, '\0'),
          "lease peer opens its held session");
  }

  ~LeasePeer() { Close(); }

  void Close() {
    if (compiler_pid > 0) StopCompiler();
    if (control_fd >= 0) ::close(control_fd);
    control_fd = -1;
    if (pid > 0) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, nullptr, 0);
    }
    pid = -1;
  }

  bool Acquire(const std::string& key, uint64_t bound_ms) {
    daemon::Writer request;
    request.U8(7);
    request.Str(key);
    request.U64(bound_ms);
    request_pending = daemon::SendFrame(control_fd, request.data());
    return request_pending;
  }

  bool Reserve(uint64_t estimate_kb, uint64_t bound_ms = 3000,
               const std::string& cost_key = "") {
    daemon::Writer request;
    request.U8(9);
    request.Str(cost_key.empty() ? std::string(32, 'a') + std::to_string(pid) + "b" +
                                  std::to_string(++reservation_sequence) : cost_key);
    request.U64(estimate_kb);
    request.U64(bound_ms);
    request_pending = daemon::SendFrame(control_fd, request.data());
    return request_pending;
  }

  int PutSocket() const {
    daemon::Writer request;
    request.U8(251);
    if (!daemon::SendFrame(control_fd, request.data())) return -1;
    char code = 0;
    iovec payload{&code, 1};
    alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int))]{};
    msghdr message{};
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    message.msg_control = ancillary;
    message.msg_controllen = sizeof(ancillary);
    if (::recvmsg(control_fd, &message, 0) != 1 || code != 'R') return -1;
    const cmsghdr* descriptor = CMSG_FIRSTHDR(&message);
    if (!descriptor || descriptor->cmsg_level != SOL_SOCKET ||
        descriptor->cmsg_type != SCM_RIGHTS) return -1;
    int put_fd = -1;
    std::memcpy(&put_fd, CMSG_DATA(descriptor), sizeof(put_fd));
    return put_fd;
  }

  bool MemoryOutcome(uint8_t expected, int timeout_ms = 1000) {
    pollfd ready{control_fd, POLLIN, 0};
    std::string reply;
    if (::poll(&ready, 1, timeout_ms) <= 0 || !daemon::RecvFrame(control_fd, &reply)) return false;
    request_pending = false;
    daemon::Reader in(reply);
    uint8_t status = 0;
    return in.U8(&status) && status == static_cast<uint8_t>(daemon::Status::kOk) &&
           in.U8(&last_outcome) && last_outcome == expected &&
           in.U64(&last_waited_ms) && in.done();
  }

  bool Spawned(pid_t reported_pid) {
    daemon::Writer request;
    request.U8(10);
    request.U64(reported_pid);
    request_pending = daemon::SendFrame(control_fd, request.data());
    return request_pending && Released();
  }

  pid_t StartCompiler(uint64_t rss_kb, uint64_t depth = 0, const std::string& mode = "") {
    daemon::Writer request;
    request.U8(253);
    request.U64(rss_kb);
    request.U64(depth);
    request.Str(mode);
    std::string reply;
    uint64_t worker_pid = 0;
    if (!daemon::SendFrame(control_fd, request.data()) ||
        !daemon::RecvFrame(control_fd, &reply)) return -1;
    daemon::Reader in(reply);
    if (!in.U64(&worker_pid) || !in.done()) return -1;
    compiler_pid = static_cast<pid_t>(worker_pid);
    return compiler_pid;
  }

  bool SignalCompiler(char byte) {
    daemon::Writer request;
    request.U8(250);
    request.U8(byte);
    std::string reply;
    return daemon::SendFrame(control_fd, request.data()) &&
           daemon::RecvFrame(control_fd, &reply) && reply == std::string(1, '\0');
  }

  void StopCompiler() {
    if (compiler_pid <= 0) return;
    daemon::Writer request;
    request.U8(252);
    std::string reply;
    if (!daemon::SendFrame(control_fd, request.data()) || !daemon::RecvFrame(control_fd, &reply)) {
      ::kill(-compiler_pid, SIGKILL);
      ::kill(compiler_pid, SIGKILL);
    }
    compiler_pid = -1;
  }

  bool Release(const std::string& key, bool stored) {
    daemon::Writer request;
    request.U8(8);
    request.Str(key);
    request.U8(stored ? 0 : 1);
    request_pending = daemon::SendFrame(control_fd, request.data());
    return request_pending;
  }

  bool Outcome(uint8_t expected, int timeout_ms = 1000) {
    pollfd ready{control_fd, POLLIN, 0};
    std::string reply;
    if (::poll(&ready, 1, timeout_ms) <= 0 || !daemon::RecvFrame(control_fd, &reply)) return false;
    request_pending = false;
    daemon::Reader in(reply);
    uint8_t status = 0;
    return in.U8(&status) && status == static_cast<uint8_t>(daemon::Status::kOk) &&
           in.U8(&last_outcome) && last_outcome <= 2 &&
           (expected == 255 || last_outcome == expected) &&
           in.U8(&last_reason) && in.U64(&last_holder_pid) && in.U64(&last_waited_ms) && in.done();
  }

  bool Released() {
    std::string reply;
    if (!daemon::RecvFrame(control_fd, &reply)) return false;
    request_pending = false;
    return reply == std::string(1, '\0');
  }

  bool NoReply(int timeout_ms = 50) const {
    if (!request_pending) {
      daemon::Writer probe;
      probe.U8(0);
      probe.U64(timeout_ms);
      std::string reply;
      return daemon::SendFrame(control_fd, probe.data()) &&
             daemon::RecvFrame(control_fd, &reply) && reply == std::string(1, '\0');
    }
    pollfd ready{control_fd, POLLIN, 0};
    return ::poll(&ready, 1, timeout_ms) == 0;
  }

  bool Error() {
    std::string reply, reason;
    uint8_t status = 0;
    if (!daemon::RecvFrame(control_fd, &reply)) return false;
    request_pending = false;
    daemon::Reader in(reply);
    return in.U8(&status) && status == static_cast<uint8_t>(daemon::Status::kError) &&
           in.Str(&reason) && !reason.empty() && in.done();
  }
};

bool LeaseStat(const SessionDaemon& server, const std::string& name, uint64_t expected) {
  const std::string status = server.Status();
  const size_t row = status.find(name);
  return row != std::string::npos &&
         std::strtoull(status.c_str() + row + name.size(), nullptr, 10) == expected;
}

bool PutLeaseBlob(const SessionDaemon& server, const std::string& key, int timeout_ms = 500,
                  const LeasePeer* owner = nullptr) {
  storage::Blob blob;
  blob.object = "lease test object";
  std::string reply;
  const int fd = owner ? owner->PutSocket() : server.Hello(daemon::kProtocolVersion, &reply);
  if (fd < 0) return false;
  timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  daemon::Writer request;
  request.U8(static_cast<uint8_t>(daemon::Op::kPut));
  request.Str(key);
  request.Str(storage::SerializeBlob(blob));
  const bool answered = daemon::SendFrame(fd, request.data()) && daemon::RecvFrame(fd, &reply);
  ::close(fd);
  daemon::Reader in(reply);
  uint8_t status = 0;
  uint8_t stored = 0;
  return answered && in.U8(&status) && status == static_cast<uint8_t>(daemon::Status::kOk) &&
         in.U8(&stored) && stored == 1;
}

void TestKeyLeases() {
  Section("daemon::lease");
  Check(daemon::LeaseWaitBoundMs(std::nullopt) == daemon::kReplyTimeoutSeconds * 1000,
        "no recorded duration waits up to the reply timeout");
  Check(daemon::LeaseWaitBoundMs(5000) == 30000,
        "a 5000 ms record keeps the 30000 ms minimum");
  Check(daemon::LeaseWaitBoundMs(20000) == 40000,
        "a 20000 ms record doubles to 40000 ms");
  Check(daemon::LeaseWaitBoundMs(UINT64_MAX) == daemon::kReplyTimeoutSeconds * 1000,
        "a huge recorded duration saturates at the reply timeout without overflow");
  SessionDaemon server(0, true);
  const std::string key(64, 'a');
  LeasePeer holder(server);
  LeasePeer waiter(server);
  Check(holder.Acquire(key, 3000) && holder.Outcome(0), "a free key grants one lease");
  Check(holder.Acquire(key, 3000) && holder.Outcome(0), "holder acquire is idempotent");
  Check(LeaseStat(server, "leases held", 1), "idempotent acquire leaves leases held 1");
  Check(waiter.Acquire(key, 3000) &&
            PollUntil([&] { return LeaseStat(server, "leases waiting", 1); }, 1000) &&
            waiter.NoReply(), "a second session waits instead of becoming a holder");
  Check(PutLeaseBlob(server, key, 500, &holder), "holder key Put stores a production blob");
  Check(waiter.Outcome(1), "holder Put wakes the waiter with stored");
  Check(waiter.last_reason == 0 && waiter.last_holder_pid == static_cast<uint64_t>(holder.pid),
        "stored reply carries the holder pid and no compile reason");
  Check(waiter.NoReply(), "holder Put sends exactly one waiter reply");
  Check(LeaseStat(server, "leases held", 0) && LeaseStat(server, "leases waiting", 0),
        "Put clears the held and waiting lease status rows");
  Check(LeaseStat(server, "compiles deduplicated", 1), "stored wake counts one deduplicated compile");

  const std::string failed_key(64, 'b');
  Check(holder.Acquire(failed_key, 3000) && holder.Outcome(0), "failed-outcome holder is granted");
  waiter.Acquire(failed_key, 3000);
  Check(PollUntil([&] { return LeaseStat(server, "leases waiting", 1); }, 1000),
        "failed-outcome waiter is queued");
  Check(holder.Release(failed_key, false) && holder.Released(), "holder releases failed");
  Check(waiter.Outcome(2), "failed release wakes the waiter with compile");
  Check(waiter.last_reason == 1, "failed release reply distinguishes holder failure");

  const std::string gone_key(64, 'c');
  Check(holder.Acquire(gone_key, 3000) && holder.Outcome(0), "holder-gone lease is granted");
  waiter.Acquire(gone_key, 3000);
  Check(PollUntil([&] { return LeaseStat(server, "leases waiting", 1); }, 1000),
        "holder-gone waiter is queued");
  const auto gone_started = std::chrono::steady_clock::now();
  holder.Close();
  Check(waiter.Outcome(2) && std::chrono::steady_clock::now() - gone_started <
                               std::chrono::milliseconds(1500),
        "holder close wakes compile within 1.5 s instead of the 3 s bound");
  Check(LeaseStat(server, "leases expired", 1), "holder close reports leases expired 1");
  Check(waiter.last_reason == 2, "session close reply distinguishes holder loss");

  LeasePeer next_holder(server);
  const std::string bound_key(64, 'd');
  Check(next_holder.Acquire(bound_key, 3000) && next_holder.Outcome(0),
        "wait-bound holder is granted");
  const auto wait_started = std::chrono::steady_clock::now();
  waiter.Acquire(bound_key, 75);
  Check(waiter.Outcome(2, 750) && std::chrono::steady_clock::now() - wait_started <
                                   std::chrono::milliseconds(750),
        "wait bound returns compile within 750 ms");
  Check(LeaseStat(server, "leases held", 1), "wait bound keeps the original holder");
  Check(waiter.last_reason == 3 && waiter.last_waited_ms >= 75,
        "wait-bound reply records the bound reason and elapsed milliseconds");
  std::mutex put_mutex;
  std::condition_variable put_ready;
  bool put_allowed = false;
  bool put_stored = false;
  const auto bound_started = std::chrono::steady_clock::now();
  waiter.Acquire(bound_key, 75);
  Check(PollUntil([&] { return LeaseStat(server, "leases waiting", 1); }, 1000),
        "bound-versus-Put waiter is queued before the race");
  std::thread concurrent_put([&] {
    std::unique_lock<std::mutex> lock(put_mutex);
    if (!put_ready.wait_for(lock, std::chrono::seconds(1), [&] { return put_allowed; })) return;
    lock.unlock();
    put_stored = PutLeaseBlob(server, bound_key, 500, &next_holder);
  });
  PollUntil([&] { return std::chrono::steady_clock::now() - bound_started >=
                        std::chrono::milliseconds(75); }, 200);
  {
    std::lock_guard<std::mutex> lock(put_mutex);
    put_allowed = true;
  }
  put_ready.notify_one();
  const bool one_outcome = waiter.Outcome(255);
  concurrent_put.join();
  Check(one_outcome && (waiter.last_outcome == 1 || waiter.last_outcome == 2) &&
            (waiter.last_reason == 0 || waiter.last_reason == 3) && put_stored &&
            waiter.NoReply(), "bound-versus-Put sends exactly one stored or bound reply");
  next_holder.Acquire(bound_key, 3000);
  next_holder.Outcome(0);
  waiter.Acquire(bound_key, 3000);
  Check(PollUntil([&] { return LeaseStat(server, "leases waiting", 1); }, 1000),
        "disconnecting waiter is queued");
  waiter.Close();
  Check(PollUntil([&] { return LeaseStat(server, "leases waiting", 0); }, 1000),
        "a disconnected waiter is removed within one second");
  Check(next_holder.Release(bound_key, false) && next_holder.Released(),
        "holder remains usable after waiter disconnect");

  LeasePeer stored_waiter(server);
  const std::string released_key(64, 'e');
  next_holder.Acquire(released_key, 3000);
  Check(next_holder.Outcome(0), "stored-release holder is granted");
  stored_waiter.Acquire(released_key, 3000);
  Check(PollUntil([&] { return LeaseStat(server, "leases waiting", 1); }, 1000),
        "stored-release waiter is queued");
  Check(next_holder.Release(released_key, true) && next_holder.Released() &&
            stored_waiter.Outcome(1), "release stored wakes stored before the subsequent Put");
  std::string before_put_why;
  auto before_put_client = daemon::DaemonClient::Connect(server.config, &before_put_why);
  storage::GetResult before_put;
  Check(before_put_client && before_put_client->Get(released_key, &before_put) && !before_put.hit,
        "Get before the release-stored Put can safely miss");
  Check(PutLeaseBlob(server, released_key), "Put following release stored lands the entry");
  std::string why;
  auto client = daemon::DaemonClient::Connect(server.config, &why);
  storage::GetResult stored;
  Check(client && client->Get(released_key, &stored) && stored.hit,
        "ordinary Get after release stored then Put hits the stored entry");
  Check(stored_waiter.NoReply(), "release stored followed by Put sends no second reply");

  const std::string independent_key(64, 'f');
  const std::string other_key(64, '1');
  Check(next_holder.Acquire(independent_key, 3000) && next_holder.Outcome(0) &&
            stored_waiter.Acquire(other_key, 3000) && stored_waiter.Outcome(0) &&
            LeaseStat(server, "leases held", 2), "different keys grant independent holders");
  Check(stored_waiter.Release(independent_key, false) && stored_waiter.Error() &&
            LeaseStat(server, "leases held", 2), "another session cannot release a holder's key");
  next_holder.Release(independent_key, false);
  next_holder.Released();
  stored_waiter.Release(other_key, false);
  stored_waiter.Released();

  const std::string store_failed_key(64, '2');
  const int blocker_fd = ::open((server.config.disk.dir + "/22").c_str(),
                                O_WRONLY | O_CREAT | O_EXCL, 0600);
  const bool blocked = blocker_fd >= 0 && ::write(blocker_fd, "blocked shard", 13) == 13;
  if (blocker_fd >= 0) ::close(blocker_fd);
  Check(blocked,
        "store-failure fixture blocks a production disk shard");
  next_holder.Acquire(store_failed_key, 3000);
  next_holder.Outcome(0);
  stored_waiter.Acquire(store_failed_key, 3000);
  Check(PollUntil([&] { return LeaseStat(server, "leases waiting", 1); }, 1000),
        "store-failure waiter is queued");
  Check(!PutLeaseBlob(server, store_failed_key, 500, &next_holder) && stored_waiter.Outcome(2) &&
            stored_waiter.last_reason == 1, "a failed production store wakes compile with failure");

  const std::string shutdown_key(64, '3');
  next_holder.Acquire(shutdown_key, 3000);
  next_holder.Outcome(0);
  stored_waiter.Acquire(shutdown_key, 3000);
  Check(PollUntil([&] { return LeaseStat(server, "leases waiting", 1); }, 1000),
        "shutdown waiter is queued");
  const auto shutdown_started = std::chrono::steady_clock::now();
  ::kill(server.pid, SIGTERM);
  Check(stored_waiter.Error() && std::chrono::steady_clock::now() - shutdown_started <
                                   std::chrono::milliseconds(1000),
        "shutdown replaces a pending lease reply with a terminal error within one second");
}

void TestKeyLeaseValidation() {
  Section("daemon::lease validation");
  SessionDaemon server(0, true);
  std::string reply;
  const int fd = server.Hello(daemon::kProtocolVersion, &reply);
  daemon::Writer acquire;
  acquire.U8(7);
  acquire.Str(std::string(64, 'a'));
  acquire.U64(100);
  Check(fd >= 0 && daemon::SendFrame(fd, acquire.data()) && daemon::RecvFrame(fd, &reply) &&
            reply[0] == static_cast<char>(daemon::Status::kError),
        "lease acquire requires a held compile session");
  if (fd >= 0) ::close(fd);
  LeasePeer peer(server);
  Check(peer.Acquire("../outside", 100) && peer.Error() && LeaseStat(server, "leases held", 0),
        "invalid lease keys are refused without holding a lease");
  SessionDaemon disabled;
  LeasePeer disabled_peer(disabled);
  Check(disabled_peer.Acquire(std::string(64, 'a'), 100) && disabled_peer.Outcome(0) &&
            LeaseStat(disabled, "leases held", 1),
        "a daemon started without switches grants an enabled client's lease");
}

void TestKeyLeasePutClose() {
  Section("daemon::lease Put and holder close");
  for (bool store_succeeds : {true, false}) {
    SessionDaemon server(0, true, true);
    LeasePeer holder(server);
    LeasePeer waiter(server);
    const std::string key(64, store_succeeds ? '4' : '5');
    const int gate_fd = ::open((server.directory + "/put-gate").c_str(), O_RDWR | O_NONBLOCK);
    Check(gate_fd >= 0 && holder.Acquire(key, 3000) && holder.Outcome(0),
          "Put-close holder acquires before the store barrier");
    waiter.Acquire(key, 3000);
    Check(PollUntil([&] { return LeaseStat(server, "leases waiting", 1); }, 1000),
          "Put-close waiter queues before the store barrier");
    bool stored = false;
    std::thread put([&] { stored = PutLeaseBlob(server, key, 3000, &holder); });
    const bool blocked = PollUntil([&] {
      return util::ReadFile(server.directory + "/put-log").value_or("").find(
                 "lease: test Put blocked " + key.substr(0, 16)) != std::string::npos;
    }, 1000);
    Check(blocked, "Put is marked in flight before the close barrier");
    holder.Close();
    Check(PollUntil([&] { return server.Sessions(1); }, 1000),
          "holder close removes its compile session while Put remains in flight");
    const auto closed_at = std::chrono::steady_clock::now();
    PollUntil([&] { return std::chrono::steady_clock::now() - closed_at >=
                          std::chrono::milliseconds(600); }, 800);
    Check(LeaseStat(server, "leases held", 1) && waiter.NoReply(),
          "a Put in flight keeps the lease held after close without a timer grace");
    const char outcome = store_succeeds ? 's' : 'f';
    if (gate_fd >= 0) ::write(gate_fd, &outcome, 1);
    put.join();
    Check(stored == store_succeeds, "blocked Put records its actual store result");
    const bool replied = waiter.Outcome(store_succeeds ? 1 : 2);
    Check(replied && waiter.last_reason == (store_succeeds ? 0 : 1),
          store_succeeds ? "Put completing after holder close wakes stored" :
                           "failed Put completing after holder close wakes compile");
    Check(waiter.NoReply(), "Put-close sends exactly one reply on the real session socket");
    Check(LeaseStat(server, "leases held", 0) && LeaseStat(server, "leases waiting", 0) &&
              LeaseStat(server, "compiles deduplicated", store_succeeds ? 1 : 0),
          "Put-close clears held and waiting rows and counts only a stored wake");
    Check(util::ReadFile(server.directory + "/put-log").value_or("").find(
              "lease: test Put waited " + key.substr(0, 16) + " for ") != std::string::npos,
          "Put barrier logs its bounded wait length");
    if (gate_fd >= 0) ::close(gate_fd);
  }
}

void TestMemoryAdmission() {
  Section("daemon::admission");
  {
    SessionDaemon server(0, false, false, 8192);
    LeasePeer holder(server), head(server), small(server);
    Check(holder.Reserve(6144) && holder.MemoryOutcome(0) &&
              LeaseStat(server, "memory reserved", 6144), "a fitting estimate is reserved");
    head.Reserve(4096);
    Check(PollUntil([&] { return LeaseStat(server, "memory waiting", 1); }, 1000) &&
              head.NoReply(), "an estimate exceeding remaining memory queues");
    small.Reserve(1024);
    Check(PollUntil([&] { return LeaseStat(server, "memory waiting", 2); }, 1000) &&
              small.NoReply(), "a smaller reservation never jumps the FIFO head");
    holder.Close();
    Check(head.MemoryOutcome(0) && small.MemoryOutcome(0) &&
              LeaseStat(server, "memory reserved", 5120),
          "session close releases memory and grants queued requests in order");
    const std::string log = util::ReadFile(server.directory + "/memory-log").value_or("");
    Check(log.find("reserve: granted 4096 kB") < log.find("reserve: granted 1024 kB") &&
              log.find("reserve: waiting (") != std::string::npos,
          "FIFO grant order and wait arithmetic appear in VCACHE_LOG");
    Check(LeaseStat(server, "reserve waits", 2), "each queued request counts one reserve wait");
    LeasePeer bounded(server);
    bounded.Reserve(6144, 75);
    Check(bounded.MemoryOutcome(1, 750) && bounded.last_waited_ms >= 75 &&
              LeaseStat(server, "memory reserved", 5120) &&
              LeaseStat(server, "memory waiting", 0),
          "a memory bound runs unreserved without changing held reservations");
    head.Close();
    small.Close();
    LeasePeer next(server);
    Check(next.Reserve(8192) && next.MemoryOutcome(0),
          "release is applied before a new reserve in the same turn");
  }
  {
    SessionDaemon server(0, false, false, 5 * 1024 * 1024);
    LeasePeer first(server), second(server), third(server);
    Check(first.Reserve(2 * 1024 * 1024) && first.MemoryOutcome(0) &&
              second.Reserve(2 * 1024 * 1024) && second.MemoryOutcome(0),
          "stale 5 GB meminfo grants the first two 2 GB estimates");
    third.Reserve(2 * 1024 * 1024);
    Check(PollUntil([&] { return LeaseStat(server, "memory waiting", 1); }, 1000) &&
              third.NoReply() && LeaseStat(server, "memory reserved", 4 * 1024 * 1024),
          "unrealised reservations queue the third grant despite stale meminfo");
    Check(first.Spawned(2000000000), "an invisible compiler pid selects conservative fallback");
    Check(util::ReadFile(server.directory + "/memory-log").value_or("").find(
              "whole estimate unrealised for 10000 ms") != std::string::npos,
          "an invisible compiler logs its ten-second conservative fallback");
    Check(PollUntil([&] { return server.Sessions(3); }, 1000) &&
              LeaseStat(server, "memory reserved", 4 * 1024 * 1024) && third.NoReply(650),
          "a failed proc read never releases a live session's reservation");
    first.Close();
    Check(third.MemoryOutcome(0), "closing the fallback session releases its reservation");
  }
  {
    SessionDaemon server(0, false, false, 1024 * 1024);
    LeasePeer alone(server);
    Check(alone.Reserve(2 * 1024 * 1024) && alone.MemoryOutcome(0),
          "always-one grants a 2 GB estimate on a 1 GB machine when alone");
    alone.Close();
    LeasePeer defaults(server);
    Check(defaults.Reserve(0) && defaults.MemoryOutcome(0) &&
              LeaseStat(server, "memory reserved", 2097152),
          "a zero estimate uses the default compile cost");
  }
  {
    SessionDaemon server(0, false, false, 50000);
    LeasePeer first(server), second(server);
    first.Reserve(32768);
    Check(first.MemoryOutcome(0), "RSS fixture obtains the first reservation");
    const pid_t compiler_pid = first.StartCompiler(16384);
    Check(compiler_pid > 0 && first.Spawned(compiler_pid),
          "CompilerSpawned reports a visible child of the session peer");
    auto pid_descriptors = [&] {
      size_t count = 0;
      std::error_code error;
      for (const auto& entry : std::filesystem::directory_iterator(
               "/proc/" + std::to_string(server.pid) + "/fd", error)) {
        if (std::filesystem::read_symlink(entry.path(), error) == "anon_inode:[pidfd]") ++count;
      }
      return count;
    };
    const size_t original_pid_descriptors = pid_descriptors();
    Check(first.Spawned(compiler_pid) && pid_descriptors() == original_pid_descriptors,
          "repeated compiler notifications retain exactly one pid descriptor");
    Check(first.Spawned(2000000000) && LeaseStat(server, "memory realised", 0),
          "a replacement invisible pid starts with the whole estimate unrealised");
    Check(first.Spawned(compiler_pid), "a visible compiler can replace the fallback pid");
    second.Reserve(32768);
    Check(second.MemoryOutcome(0, 1500),
          "realised RSS reduces unrealised memory without double counting MemAvailable");
    Check(PollUntil([&] {
      const std::string status = server.Status();
      const size_t row = status.find("memory realised");
      return row != std::string::npos &&
             std::strtoull(status.c_str() + row + 15, nullptr, 10) >= 16384;
    }, 1000), "memory realised status reports sampled RSS");
    first.StopCompiler();
    Check(PollUntil([&] { return LeaseStat(server, "memory reserved", 32768); }, 1500) &&
              server.Sessions(2), "confirmed compiler exit releases memory before session close");
    second.Close();
    Check(first.Reserve(1024) && first.MemoryOutcome(0), "an overshoot fixture reserves 1024 kB");
    const pid_t overshoot_pid = first.StartCompiler(4096);
    first.Spawned(overshoot_pid);
    Check(PollUntil([&] {
      return util::ReadFile(server.directory + "/memory-log").value_or("").find(
                 "rss overshoot") != std::string::npos;
    }, 1500), "RSS above the estimate is clamped and logged as an overshoot");
    LeasePeer after_overshoot(server);
    Check(after_overshoot.Reserve(50000) && after_overshoot.MemoryOutcome(0, 1000),
          "RSS above the estimate contributes zero unrealised memory to the next grant");
    first.StopCompiler();
  }
  {
    SessionDaemon server(0, false, false, 1024);
    LeasePeer holder(server), queued(server);
    holder.Reserve(1024);
    holder.MemoryOutcome(0);
    queued.Reserve(1024);
    Check(PollUntil([&] { return LeaseStat(server, "memory waiting", 1); }, 1000),
          "memory-rise fixture queues a reservation");
    const int mem_fd = ::open((server.directory + "/meminfo").c_str(), O_WRONLY | O_TRUNC);
    if (mem_fd >= 0) {
      const std::string meminfo = "MemAvailable: 2048 kB\n";
      ::write(mem_fd, meminfo.data(), meminfo.size());
      ::close(mem_fd);
    }
    Check(queued.MemoryOutcome(0, 1500) && LeaseStat(server, "memory reserved", 2048),
          "rising MemAvailable grants the FIFO head within one sample interval");
  }
  {
    SessionDaemon server(0, false, false, 8192);
    LeasePeer holder(server);
    holder.Reserve(8192);
    holder.MemoryOutcome(0);
    core::Config enabled = server.config;
    enabled.daemon.mode = core::DaemonMode::kOn;
    enabled.daemon.admission = true;
    auto session = daemon::DaemonClient::OpenCompileSession(enabled);
    bool reserved = false;
    std::atomic<bool> finished{false};
    std::thread reserve([&] {
      if (session) reserved = session->ReserveMemory(std::string(64, 'a'), 1024);
      finished = true;
    });
    Check(PollUntil([&] { return LeaseStat(server, "memory waiting", 1); }, 1000),
          "an enabled client reserves through a daemon started with admission off");
    bool timeout_has_margin = false;
    for (int fd = 0; fd < 256; ++fd) {
      timeval timeout{};
      socklen_t size = sizeof(timeout);
      if (::getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, &size) == 0 &&
          timeout.tv_sec == 615) timeout_has_margin = true;
    }
    Check(timeout_has_margin, "reserve receive timeout has the required 15 second margin");
    holder.Close();
    const bool received = PollUntil([&] { return finished.load(); }, 1000);
    if (!received && server.pid > 0) ::kill(server.pid, SIGTERM);
    reserve.join();
    Check(received && reserved, "the client receives granted after the earlier reservation ends");
  }
  {
    SessionDaemon server(0, false, false, 8192);
    LeasePeer holder(server), head(server), later(server);
    holder.Reserve(6144);
    holder.MemoryOutcome(0);
    head.Reserve(4096, 100);
    PollUntil([&] { return LeaseStat(server, "memory waiting", 1); }, 1000);
    later.Reserve(1024);
    Check(head.MemoryOutcome(1, 1000) && later.MemoryOutcome(0, 1000) &&
              LeaseStat(server, "memory reserved", 7168),
          "an expired FIFO head is removed before the next fitting request is granted");
  }
  {
    SessionDaemon server(0, false, false, 2048);
    LeasePeer holder(server);
    holder.Reserve(2048);
    holder.MemoryOutcome(0);
    holder.Spawned(2000000000);
    core::Config enabled = server.config;
    enabled.daemon.mode = core::DaemonMode::kOn;
    enabled.daemon.admission = true;
    auto session = daemon::DaemonClient::OpenCompileSession(enabled);
    bool reserved = false;
    std::atomic<bool> answered{false};
    const auto started = std::chrono::steady_clock::now();
    std::thread reserve([&] {
      if (session) reserved = session->ReserveMemory(std::string(64, 'a'), 2048, 12000);
      answered = true;
    });
    Check(PollUntil([&] { return LeaseStat(server, "memory waiting", 1); }, 1000) &&
              LeaseStat(server, "memory realised", 0),
          "an invisible pid keeps the whole estimate unrealised initially");
    Check(PollUntil([&] { return answered.load(); }, 11000) && reserved &&
              std::chrono::steady_clock::now() - started >= std::chrono::seconds(9) &&
              LeaseStat(server, "memory realised", 2048) &&
              LeaseStat(server, "memory reserved", 4096),
          "the ten-second pid fallback realises its estimate and grants the queued head");
    reserve.join();
  }
  {
    SessionDaemon disabled;
    LeasePeer peer(disabled);
    Check(peer.Reserve(1024) && peer.MemoryOutcome(0),
          "a switch-free daemon always serves an enabled client's reserve");
  }
}

void TestAdmissionProcessTree() {
  Section("daemon::admission process tree");
  SessionDaemon server(0, false, false, 90000, {}, true);
  LeasePeer holder(server), queued(server);
  Check(holder.Reserve(65536) && holder.MemoryOutcome(0),
        "process-tree fixture reserves its driver estimate");
  const pid_t compiler = holder.StartCompiler(49152, 2);
  Check(compiler > 0 && holder.Spawned(compiler),
        "a visible compiler driver reports its grandchild allocator");
  queued.Reserve(65536);
  Check(queued.MemoryOutcome(0, 1500),
        "a grandchild allocator's RSS realises the driver reservation and grants the next job");
  Check(PollUntil([&] {
    const std::string status = server.Status();
    const size_t row = status.find("memory realised");
    return row != std::string::npos &&
           std::strtoull(status.c_str() + row + 15, nullptr, 10) >= 49152;
  }, 1000), "process-tree realised memory includes the live grandchild");
  const auto granted = std::chrono::steady_clock::now();
  PollUntil([&] { return std::chrono::steady_clock::now() - granted >=
                        std::chrono::milliseconds(1100); }, 1500);
  Check(LeaseStat(server, "memory waiting", 0) &&
            LeaseStat(server, "jobserver tokens withdrawn", 0) &&
            LeaseStat(server, "jobserver tokens free", 4),
        "realised grandchild memory keeps the jobserver fully available across two ticks");
}

void TestAdmissionReview() {
  Section("daemon::admission synchronized samples");
  {
    SessionDaemon server(0, true, false, 8192);
    LeasePeer blocker(server), holder(server), waiter(server);
    blocker.Reserve(8192);
    blocker.MemoryOutcome(0);
    const std::string key(64, 'c');
    holder.Acquire(key, 3000);
    holder.Outcome(0);
    holder.Reserve(8192);
    Check(PollUntil([&] { return LeaseStat(server, "memory waiting", 1); }, 1000),
          "a lease holder queues behind the full memory reservation");
    waiter.Acquire(key, 100);
    Check(PollUntil([&] { return LeaseStat(server, "leases waiting", 1); }, 1000) &&
              waiter.NoReply(250),
          "a 100 ms lease bound stays paused while its holder waits for memory");
    blocker.Close();
    Check(holder.MemoryOutcome(0, 1000) && PutLeaseBlob(server, key, 500, &holder) &&
              waiter.Outcome(1, 1000) && waiter.NoReply(),
          "a memory-queued holder's Put wakes exactly one stored lease reply");
  }
  {
    SessionDaemon server(0, false, false, 50000);
    LeasePeer holder(server), queued(server);
    holder.Reserve(32768);
    Check(holder.MemoryOutcome(0), "meminfo-drop fixture reserves its first compiler");
    const pid_t compiler = holder.StartCompiler(16384, 1);
    util::WriteFileAtomic(server.directory + "/meminfo", "MemAvailable: 20000 kB\n");
    holder.Spawned(compiler);
    queued.Reserve(30000);
    Check(PollUntil([&] { return LeaseStat(server, "memory waiting", 1); }, 1000) &&
              queued.NoReply(100),
          "RSS growth refreshes the dropped MemAvailable before granting another reservation");
  }
  {
    SessionDaemon server(0, false, false, 1);
    util::WriteFileAtomic(server.directory + "/meminfo", "unreadable MemAvailable\n");
    LeasePeer first(server), second(server);
    first.Reserve(1024);
    second.Reserve(1024);
    Check(first.MemoryOutcome(0) && second.MemoryOutcome(0, 1000) &&
              LeaseStat(server, "memory reserved", 2048),
          "unavailable meminfo grants concurrent reserves when no good sample exists");
    const std::string log = util::ReadFile(server.directory + "/memory-log").value_or("");
    Check(log.find("MemAvailable unreadable, admission unavailable") != std::string::npos,
          "unavailable admission is logged instead of silently serializing compiles");
    const auto warned = std::chrono::steady_clock::now();
    PollUntil([&] { return std::chrono::steady_clock::now() - warned >=
                          std::chrono::milliseconds(750); }, 1000);
    const std::string repeated_log = util::ReadFile(server.directory + "/memory-log").value_or("");
    const size_t warning = repeated_log.find("MemAvailable unreadable");
    Check(warning != std::string::npos &&
              repeated_log.find("MemAvailable unreadable", warning + 1) == std::string::npos,
          "unreadable meminfo logs one warning across repeated sampling ticks");
  }
  {
    SessionDaemon server(0, false, false, 8192);
    LeasePeer first(server), second(server);
    first.Reserve(4096);
    first.MemoryOutcome(0);
    util::WriteFileAtomic(server.directory + "/meminfo", "MemAvailable missing\n");
    first.Spawned(2000000000);
    second.Reserve(2048);
    Check(second.MemoryOutcome(0, 1000) && LeaseStat(server, "memory reserved", 6144),
          "an unreadable meminfo refresh retains its last good value");
  }
  {
    SessionDaemon server(0, false, false, 8192,
                         {{"VCACHE_DAEMON_TEST_NO_PIDFD", "1"},
                          {"VCACHE_DAEMON_TEST_FAIL_RSS", "failed-rss"}});
    LeasePeer holder(server), queued(server);
    holder.Reserve(65536);
    holder.MemoryOutcome(0);
    const pid_t compiler = holder.StartCompiler(1024, 1);
    Check(compiler > 0 && holder.Spawned(compiler),
          "failed-read fixture has a visible live compiler without a pidfd");
    util::WriteFileAtomic(server.directory + "/failed-rss", "fail next proc read\n");
    holder.Spawned(compiler);
    queued.Reserve(8192);
    Check(PollUntil([&] { return LeaseStat(server, "memory waiting", 1); }, 1000) &&
              LeaseStat(server, "memory reserved", 65536) && queued.NoReply(100) &&
              ::kill(compiler, 0) == 0,
          "a failed visible proc read without pidfd never releases a live reservation");
  }
  {
    SessionDaemon server(0, false, false, 90000,
                         {{"VCACHE_DAEMON_TEST_RSS_GATE", "rss-gate"}});
    LeasePeer holder(server);
    holder.Reserve(65536);
    holder.MemoryOutcome(0);
    const pid_t compiler = holder.StartCompiler(2048, 1);
    const int gate = ::open((server.directory + "/rss-gate").c_str(), O_RDWR | O_NONBLOCK);
    bool spawned = false;
    std::thread spawn([&] { spawned = holder.Spawned(compiler); });
    Check(PollUntil([&] {
      return util::ReadFile(server.directory + "/memory-log").value_or("").find(
                 "reserve: test RSS sample blocked") != std::string::npos;
    }, 1000), "RSS fixture reaches its outside-lock sample barrier");
    const auto started = std::chrono::steady_clock::now();
    Check(server.Sessions(1) && std::chrono::steady_clock::now() - started <
                                   std::chrono::milliseconds(250),
          "a blocked RSS sample leaves status responsive within 250 ms");
    if (gate >= 0) ::write(gate, "R", 1);
    spawn.join();
    Check(spawned, "RSS sampling resumes after its bounded barrier");
    const auto sampled = std::chrono::steady_clock::now();
    PollUntil([&] { return std::chrono::steady_clock::now() - sampled >=
                          std::chrono::milliseconds(750); }, 1000);
    const std::string prefix = "reserve: " + std::to_string(compiler) + " rss ";
    Check(util::ReadFile(server.directory + "/memory-log").value_or("").find(prefix) ==
              std::string::npos, "steady RSS sampling does not write per-tick log lines");
    holder.Close();
    Check(PollUntil([&] {
      return util::ReadFile(server.directory + "/memory-log").value_or("").find(prefix) !=
             std::string::npos;
    }, 1000), "reservation release logs the process-tree RSS peak");
    if (gate >= 0) ::close(gate);
  }
  {
    SessionDaemon server(0, false, false, 90000,
                         {{"VCACHE_DAEMON_TEST_TRACE_RSS", "1"}});
    LeasePeer first(server), second(server);
    first.Reserve(32768);
    first.MemoryOutcome(0);
    const pid_t first_pid = first.StartCompiler(1024, 1);
    first.Spawned(first_pid);
    second.Reserve(32768);
    second.MemoryOutcome(0);
    const pid_t second_pid = second.StartCompiler(1024, 1);
    second.Spawned(second_pid);
    const std::string log = util::ReadFile(server.directory + "/memory-log").value_or("");
    const std::string marker = "reserve: test spawn sample " + std::to_string(first_pid);
    const size_t first_sample = log.find(marker);
    Check(first_sample != std::string::npos &&
              log.find(marker, first_sample + marker.size()) == std::string::npos &&
              log.find("reserve: test spawn sample " + std::to_string(second_pid)) !=
                  std::string::npos,
          "a new compiler notification samples only its own pid tree");
  }
  {
    SessionDaemon server(0, false, false, 8192,
                         {{"VCACHE_DAEMON_TEST_RESERVE_GATE", "reserve-gate"}});
    LeasePeer peer(server);
    const int gate = ::open((server.directory + "/reserve-gate").c_str(), O_RDWR | O_NONBLOCK);
    peer.Reserve(1024);
    Check(PollUntil([&] {
      return util::ReadFile(server.directory + "/memory-log").value_or("").find(
                 "reserve: test reserve blocked") != std::string::npos;
    }, 1000), "late-reserve fixture pauses a received frame before admission");
    std::string shutdown_reply;
    const int shutdown_fd = server.Hello(daemon::kProtocolVersion, &shutdown_reply);
    daemon::Writer shutdown_request;
    shutdown_request.U8(static_cast<uint8_t>(daemon::Op::kShutdown));
    Check(shutdown_fd >= 0 && daemon::SendFrame(shutdown_fd, shutdown_request.data()),
          "the late-reserve fixture begins a real daemon shutdown request");
    Check(PollUntil([&] {
      return util::ReadFile(server.directory + "/memory-log").value_or("").find(
                 "reserve: test sampler joined during shutdown") != std::string::npos;
    }, 1000), "shutdown moves and joins the old sampler before the late reserve resumes");
    if (gate >= 0) ::write(gate, "R", 1);
    peer.Error();
    peer.Close();
    Check(shutdown_fd >= 0 && daemon::RecvFrame(shutdown_fd, &shutdown_reply) &&
              !shutdown_reply.empty() && shutdown_reply[0] == '\0',
          "the shutdown client receives its drain result after the late reserve");
    if (shutdown_fd >= 0) ::close(shutdown_fd);
    int status = 0;
    const bool exited = PollUntil([&] { return ::waitpid(server.pid, &status, WNOHANG) ==
                                              server.pid; }, 2000);
    Check(exited && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "a reserve received before shutdown cannot restart a worker or abort the daemon");
    if (exited) server.pid = -1;
    if (gate >= 0) ::close(gate);
  }
  {
    SessionDaemon server(0, false, false, 8192);
    LeasePeer holder(server);
    holder.Reserve(8192);
    holder.MemoryOutcome(0);
    const std::string client_log = server.directory + "/waiting-client-log";
    ::setenv("VCACHE_LOG", client_log.c_str(), 1);
    util::InitLogging();
    core::Config enabled = server.config;
    enabled.daemon.mode = core::DaemonMode::kOn;
    enabled.daemon.admission = true;
    auto session = daemon::DaemonClient::OpenCompileSession(enabled);
    std::atomic<bool> answered{false};
    bool reserved = false;
    std::thread reserve([&] {
      if (session) reserved = session->ReserveMemory(std::string(64, 'a'), 1024);
      answered = true;
    });
    const bool queued = PollUntil([&] { return LeaseStat(server, "memory waiting", 1); }, 1000);
    holder.Close();
    const bool received = PollUntil([&] { return answered.load(); }, 1000);
    if (!received) ::kill(server.pid, SIGTERM);
    reserve.join();
    Check(queued && received && reserved && util::ReadFile(client_log).value_or("").find(
              "reserve: granted 1024 kB after ") != std::string::npos,
          "a granted memory wait is logged in the waiting client's own decision log");
    session.reset();
    ::unsetenv("VCACHE_LOG");
    util::InitLogging();
  }
}

void TestAdmissionBoundsAndTreePeaks() {
  Section("daemon::admission bounds and tree peaks");
  const auto memory_reserved = [](const SessionDaemon& server) {
    const auto status = server.Status();
    const auto row = status.find("memory reserved");
    return row == std::string::npos ? uint64_t{0} :
        std::strtoull(status.c_str() + row + 15, nullptr, 10);
  };
  const auto started = std::chrono::steady_clock::now();
  for (const uint64_t bound : {uint64_t{0}, uint64_t{50}, daemon::kMemoryWaitBoundMs,
                               daemon::kMemoryWaitBoundMs + 1, UINT64_MAX}) {
    const auto deadline = daemon::MemoryReserveDeadline(started, bound);
    Check(deadline - started == std::chrono::milliseconds(
              std::min(bound, daemon::kMemoryWaitBoundMs)),
          "server reserve deadline caps requested milliseconds: " + std::to_string(bound));
  }
  struct DeadlineCase {
    uint64_t bound_ms;
    int64_t queue_ms, expected_ms;
    const char* name;
  };
  for (const auto& row : std::vector<DeadlineCase>{
      {100, -50, 100, "a negative queue duration cannot shorten a lease deadline"},
      {100, 50, 150, "a lease deadline adds only overlapping holder queue time"},
      {300000, 600000, 900000, "lease deadline reaches exactly the 600 second pause cap"},
      {300000, 600001, 900000, "lease deadline rejects queue time above the 600 second cap"},
      {UINT64_MAX, 86400000, 900000,
       "huge bounds and queue time still cap lease wait at 900 seconds"}}) {
    Check(daemon::LeaseWaitDeadline(started, row.bound_ms,
                                   std::chrono::milliseconds(row.queue_ms)) - started ==
              std::chrono::milliseconds(row.expected_ms), row.name);
  }
  {
    SessionDaemon server(0, false, false, 8192);
    LeasePeer holder(server), queued(server);
    holder.Reserve(8192);
    holder.MemoryOutcome(0);
    queued.Reserve(8192, UINT64_MAX);
    Check(PollUntil([&] {
      return LeaseStat(server, "memory waiting", 1) &&
          util::ReadFile(server.directory + "/memory-log").value_or("").find(
              "reserve: wait bound 600000 ms") != std::string::npos;
    }, 1000), "a real reserve request above the server cap logs a 600000 ms bound");
    holder.Close();
    Check(queued.MemoryOutcome(0, 1000), "a capped reserve still wakes as soon as memory is free");
  }
  {
    SessionDaemon server(0, false, false, 262144);
    core::RootMap roots;
    roots.AddIfUncovered(server.directory, "tree-cost");
    const std::string source = server.directory + "/parallel-link";
    util::ProcResult observation;
    observation.max_rss_kb = 32768;
    core::RecordCompileCost(server.config.disk.dir, "link", source, "", {"-flto"}, roots,
                            observation);
    const std::string key = core::ComputeCostKey("link", source, "", {"-flto"}, roots);
    const auto client_estimate = daemon::MemoryEstimateKb(server.config, key, true);
    LeasePeer first(server);
    first.Reserve(client_estimate, 3000, key);
    first.MemoryOutcome(0);
    const pid_t compiler = first.StartCompiler(32768, 1, "parallel");
    Check(compiler > 0 && first.Spawned(compiler) && PollUntil([&] {
      const auto status = server.Status();
      const auto row = status.find("memory realised");
      return row != std::string::npos &&
             std::strtoull(status.c_str() + row + 15, nullptr, 10) >= 65536;
    }, 1000), "parallel compiler children contribute their summed RSS to the sampled peak");
    LeasePeer blocker(server), queued(server);
    blocker.Reserve(262144);
    blocker.MemoryOutcome(0);
    queued.Reserve(client_estimate, 3000, key);
    Check(PollUntil([&] { return LeaseStat(server, "memory waiting", 1); }, 1000),
          "a matching cost request can queue before the preceding tree peak is released");
    first.StopCompiler();
    Check(PollUntil([&] { return LeaseStat(server, "memory reserved", 262144); }, 1500),
          "parallel compiler exit releases its reservation before another estimate");
    blocker.Close();
    Check(queued.MemoryOutcome(0) && memory_reserved(server) >= 65536,
          "a queued matching cost key refreshes its tree-peak estimate before grant");
    queued.Close();
    PollUntil([&] { return LeaseStat(server, "memory reserved", 0); }, 1000);
    LeasePeer next(server), unrelated(server);
    next.Reserve(client_estimate, 3000, key);
    Check(next.MemoryOutcome(0) && memory_reserved(server) >= 65536,
          "the next matching cost key reserves its observed tree-sum peak above wait4 RSS");
    next.Close();
    PollUntil([&] { return LeaseStat(server, "memory reserved", 0); }, 1000);
    const std::string client_log = server.directory + "/granted-estimate-client-log";
    ::setenv("VCACHE_LOG", client_log.c_str(), 1);
    util::InitLogging();
    core::Config enabled = server.config;
    enabled.daemon.mode = core::DaemonMode::kOn;
    enabled.daemon.admission = true;
    auto session = daemon::DaemonClient::OpenCompileSession(enabled);
    const bool reserved = session && session->ReserveMemory(key, client_estimate, 1000);
    const auto granted_kb = memory_reserved(server);
    Check(reserved && granted_kb >= 65536 &&
              util::ReadFile(client_log).value_or("").find(
                  "reserve: granted " + std::to_string(granted_kb) + " kB after ") !=
                  std::string::npos,
          "the client logs the server's learned granted estimate rather than requested wait4 RSS");
    Check(session && session->ReserveMemory(key, 131072, 1000) &&
              memory_reserved(server) == granted_kb &&
              util::ReadFile(client_log).value_or("").find("reserve: granted 131072 kB after ") ==
                  std::string::npos,
          "a duplicate reserve logs the held grant rather than its new requested estimate");
    session.reset();
    ::unsetenv("VCACHE_LOG");
    util::InitLogging();
    PollUntil([&] { return LeaseStat(server, "memory reserved", 0); }, 1000);
    unrelated.Reserve(1024);
    Check(unrelated.MemoryOutcome(0) &&
              util::ReadFile(server.directory + "/memory-log").value_or("").find(
                  "reserve: granted 1024 kB") != std::string::npos,
          "tree-sum learning does not inflate an unrelated cost key");
    next.Close();
    unrelated.Close();
    LeasePeer larger(server);
    larger.Reserve(131072, 3000, key);
    Check(larger.MemoryOutcome(0) && LeaseStat(server, "memory reserved", 131072),
          "an existing larger estimate stays above the learned tree-sum peak");
  }
  {
    SessionDaemon server(0, false, false, 90000);
    LeasePeer holder(server), queued(server);
    holder.Reserve(65536);
    holder.MemoryOutcome(0);
    const pid_t compiler = holder.StartCompiler(49152, 2, "zombie");
    holder.Spawned(compiler);
    queued.Reserve(65536);
    Check(compiler > 0 && queued.MemoryOutcome(0, 1500),
          "an unreaped zombie sibling does not void its live grandchild's RSS sample");
  }
  {
    SessionDaemon server(0, false, false, 262144);
    LeasePeer holder(server);
    const std::string key(64, 'd');
    holder.Reserve(1024, 3000, key);
    holder.MemoryOutcome(0);
    const pid_t compiler = holder.StartCompiler(8192, 1, "grow");
    holder.Spawned(compiler);
    const auto realised = [&] {
      const auto status = server.Status();
      const auto row = status.find("memory realised");
      return row == std::string::npos ? uint64_t{0} :
          std::strtoull(status.c_str() + row + 15, nullptr, 10);
    };
    holder.SignalCompiler('G');
    const bool grew = PollUntil([&] { return realised() >= 16384; }, 1500);
    holder.SignalCompiler('G');
    const bool grew_again = PollUntil([&] { return realised() >= 24576; }, 1500);
    const auto log = util::ReadFile(server.directory + "/memory-log").value_or("");
    const auto overshoot = log.find("reserve: rss overshoot");
    Check(grew && grew_again && overshoot != std::string::npos &&
              log.find("reserve: rss overshoot", overshoot + 1) == std::string::npos,
          "a growing overshoot logs once while retaining its largest sampled peak");
    holder.SignalCompiler('S');
    Check(PollUntil([&] { return realised() > 0 && realised() < 16384; }, 1500),
          "the live tree's RSS can shrink after its observed peak");
    holder.StopCompiler();
    PollUntil([&] { return LeaseStat(server, "memory reserved", 0); }, 1500);
    LeasePeer next(server);
    next.Reserve(1024, 3000, key);
    Check(next.MemoryOutcome(0) && memory_reserved(server) >= 24576,
          "tree-sum estimates retain the peak after the live sample shrinks");
  }
  {
    SessionDaemon server(0, false, false, 8192);
    LeasePeer holder(server);
    holder.Reserve(4096);
    holder.MemoryOutcome(0);
    holder.Spawned(2000000000);
    holder.Close();
    Check(PollUntil([&] {
      return util::ReadFile(server.directory + "/memory-log").value_or("").find(
          "reserve: 2000000000 unsampled of 4096 at release") != std::string::npos;
    }, 1000), "an invisible compiler release reports unsampled rather than a zero RSS observation");
  }
}

void TestRunSpawnCallback() {
  Section("util::Run spawn callback");
  int calls = 0;
  pid_t spawned_pid = 0;
  util::ProcOptions options{.capture_stdout = true, .capture_stderr = true};
  options.on_spawn = [&](int pid) {
    ++calls;
    spawned_pid = pid;
    Check(pid > 0 && ::kill(pid, 0) == 0,
          "spawn callback receives the live child pid in the parent");
  };
  const auto result = util::Run({"sh", "-c", "printf out; printf err >&2; exit 7"}, options);
  Check(calls == 1 && spawned_pid > 0 && result.exit_code == 7 &&
            result.stdout_data == "out" && result.stderr_data == "err",
        "spawn notification preserves exit status and concurrent pipe capture");
  calls = 0;
  const auto absent = util::Run({}, options);
  Check(calls == 0 && absent.exit_code == -1 && absent.wall_ms == 0 && absent.max_rss_kb == 0,
        "a process that was never spawned has no notification or cost");
}

void TestLeaseReviewFixes() {
  Section("daemon::lease review fixes");
  SessionDaemon server;
  LeasePeer holder(server), waiter(server);
  const std::string key(64, '6');
  holder.Acquire(key, 3000);
  Check(holder.Outcome(0), "a switch-free daemon grants the holder's lease");
  waiter.Acquire(key, 3000);
  Check(PollUntil([&] { return LeaseStat(server, "leases waiting", 1); }, 1000),
        "foreign-store fixture queues a waiter");
  const std::string blocker = server.config.disk.dir + "/66";
  const int blocker_fd = ::open(blocker.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (blocker_fd >= 0) ::close(blocker_fd);
  Check(blocker_fd >= 0 && !PutLeaseBlob(server, key) && waiter.NoReply() &&
            LeaseStat(server, "leases held", 1),
        "a failing foreign Put leaves the holder and waiter unchanged");
  Check(::rename(blocker.c_str(), (server.directory + "/blocked-shard").c_str()) == 0 &&
            PutLeaseBlob(server, key, 500, &holder) && waiter.Outcome(1) && waiter.NoReply(),
        "the holder's Put alone wakes the waiter with exactly one stored reply");

  core::Config enabled = server.config;
  enabled.daemon.mode = core::DaemonMode::kOn;
  enabled.daemon.single_flight = true;
  const std::string log_path = server.directory + "/client-log";
  ::setenv("VCACHE_LOG", log_path.c_str(), 1);
  util::InitLogging();
  auto session = daemon::DaemonClient::OpenCompileSession(enabled);
  Check(session && session->AcquireLease("invalid/key", 100) == daemon::LeaseOutcome::kCompile &&
            session->AcquireLease(std::string(64, '7'), 100) == daemon::LeaseOutcome::kGranted,
        "a refused lease keeps the session usable for the next scheduler operation");
  Check(util::ReadFile(log_path).value_or("").find(
            "lease: daemon refused (invalid lease acquire), compiling") != std::string::npos,
        "lease refusal is logged distinctly from holder loss");
  if (session) session->ReleaseLease(false);
  const std::string bounded_key(64, '8');
  holder.Acquire(bounded_key, 3000);
  holder.Outcome(0);
  daemon::LeaseOutcome outcome = daemon::LeaseOutcome::kGranted;
  std::thread acquire([&] {
    if (session) outcome = session->AcquireLease(bounded_key, 300000);
  });
  Check(PollUntil([&] { return LeaseStat(server, "leases waiting", 1); }, 1000),
        "capped lease request reaches its server wait");
  bool timeout_has_margin = false;
  for (int fd = 0; fd < 256; ++fd) {
    timeval timeout{};
    socklen_t size = sizeof(timeout);
    if (::getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, &size) == 0 &&
        timeout.tv_sec == 915) timeout_has_margin = true;
  }
  Check(timeout_has_margin,
        "capped lease receive timeout includes memory-queue headroom and the 15 second margin");
  holder.Release(bounded_key, false);
  holder.Released();
  acquire.join();
  Check(outcome == daemon::LeaseOutcome::kCompile,
        "a capped wait still receives its holder failure reply normally");
  session.reset();
  ::unsetenv("VCACHE_LOG");
  util::InitLogging();
}

void TestLeaseOrphanPut() {
  Section("daemon::lease holder identity after close");
  SessionDaemon server(0, false, true);
  LeasePeer holder(server);
  const std::string key(64, '9');
  holder.Acquire(key, 3000);
  Check(holder.Outcome(0), "orphan-Put holder acquires without waiters");
  const int gate_fd = ::open((server.directory + "/put-gate").c_str(), O_RDWR | O_NONBLOCK);
  bool stored = false;
  std::thread put([&] { stored = PutLeaseBlob(server, key, 3000, &holder); });
  Check(PollUntil([&] {
    return util::ReadFile(server.directory + "/put-log").value_or("").find(
               "lease: test Put blocked ") != std::string::npos;
  }, 1000), "orphan-Put store reaches the accepted-store barrier");
  const uint64_t holder_pid = holder.pid;
  holder.Close();
  Check(PollUntil([&] { return server.Sessions(0); }, 1000),
        "holder session ends before a later waiter exists");
  LeasePeer waiter(server);
  waiter.Acquire(key, 3000);
  Check(PollUntil([&] { return LeaseStat(server, "leases waiting", 1); }, 1000),
        "a later waiter joins the orphaned in-flight Put");
  const std::string blocker = server.config.disk.dir + "/99";
  const int blocker_fd = ::open(blocker.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (blocker_fd >= 0) ::close(blocker_fd);
  Check(blocker_fd >= 0 && !PutLeaseBlob(server, key) && waiter.NoReply(),
        "cached holder identity rejects a foreign failed Put after session destruction");
  if (blocker_fd >= 0) ::rename(blocker.c_str(), (server.directory + "/blocked-shard").c_str());
  if (gate_fd >= 0) ::write(gate_fd, "s", 1);
  put.join();
  Check(stored && waiter.Outcome(1) && waiter.last_holder_pid == holder_pid && waiter.NoReply(),
        "the accepted holder Put wakes once with its retained holder pid");
  if (gate_fd >= 0) ::close(gate_fd);
}

void TestHasher() {
  Section("hash::Hasher");

  Check(hash::HashString("a") != hash::HashString("b"), "distinct inputs differ");
  CheckEq(hash::HashString("x"), hash::HashString("x"), "hashing is deterministic");
  Check(hash::HashString("").size() == hash::kDigestHexLen, "digest is 64 hex chars");

  // Length delimiting must stop ("ab","c") colliding with ("a","bc").
  hash::Hasher h1;
  h1.UpdateDelimited("ab");
  h1.UpdateDelimited("c");
  hash::Hasher h2;
  h2.UpdateDelimited("a");
  h2.UpdateDelimited("bc");
  Check(h1.Hex() != h2.Hex(), "delimited updates are unambiguous");

  // Hex() must not consume the state.
  hash::Hasher h3;
  h3.Update("hello");
  const std::string first = h3.Hex();
  CheckEq(h3.Hex(), first, "Hex() is repeatable");
}

void TestSha256() {
  Section("hash::Sha256 / HmacSha256");

  // NIST FIPS 180-4 examples.
  CheckEq(hash::Sha256Hex("abc"),
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256(\"abc\")");
  CheckEq(hash::Sha256Hex(
              "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "SHA-256(two-block NIST example)");

  // Lengths clustered around the padding boundaries, which is where a
  // hand-written Final() goes wrong: 55/56 straddle the point where the length
  // field no longer fits, and 63/64/65 straddle the block size.
  struct Case {
    size_t length;
    const char* digest;
  };
  static const Case kCases[] = {
      {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {1, "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb"},
      {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
      {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
      {57, "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6"},
      {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
      {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
      {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
      {119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
      {120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"},
      {127, "c57e9278af78fa3cab38667bef4ce29d783787a2f731d4e12200270f0c32320a"},
      {128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e"},
      {1000, "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"},
  };
  bool all_lengths_ok = true;
  for (const Case& c : kCases) {
    const std::string message(c.length, 'a');
    if (hash::Sha256Hex(message) != c.digest) {
      all_lengths_ok = false;
      std::printf("         length %zu mismatch\n", c.length);
    }
  }
  Check(all_lengths_ok, "SHA-256 across padding and block boundaries (13 lengths)");

  // Streaming in awkward chunk sizes must equal the one-shot digest, since
  // vcache feeds SigV4 payloads of arbitrary size.
  {
    std::string message;
    for (int i = 0; i < 500; ++i) message.push_back(static_cast<char>(i % 251));
    const std::string expected = hash::Sha256Hex(message);
    bool streaming_ok = true;
    for (size_t chunk : {size_t{1}, size_t{7}, size_t{63}, size_t{64}, size_t{65},
                         size_t{128}}) {
      hash::Sha256 ctx;
      for (size_t off = 0; off < message.size(); off += chunk) {
        ctx.Update(message.data() + off, std::min(chunk, message.size() - off));
      }
      unsigned char digest[hash::kSha256DigestLen];
      ctx.Final(digest);
      if (util::HexEncode(digest, sizeof(digest)) != expected) streaming_ok = false;
    }
    Check(streaming_ok, "streaming updates match the one-shot digest");
  }

  // Reset() must return the object to its initial state.
  {
    hash::Sha256 ctx;
    ctx.Update("garbage that should be discarded");
    ctx.Reset();
    ctx.Update("abc");
    unsigned char digest[hash::kSha256DigestLen];
    ctx.Final(digest);
    CheckEq(util::HexEncode(digest, sizeof(digest)),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "Reset() restores the initial state");
  }

  // NUL bytes must not truncate anything.
  Check(hash::Sha256Hex(std::string("a\0b", 3)) != hash::Sha256Hex("a"),
        "embedded NUL bytes are hashed");

  // RFC 4231 HMAC-SHA256 test vectors.
  CheckEq(hash::HmacSha256Hex(std::string(20, '\x0b'), "Hi There"),
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
          "RFC 4231 case 1");
  CheckEq(hash::HmacSha256Hex("Jefe", "what do ya want for nothing?"),
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
          "RFC 4231 case 2");
  CheckEq(hash::HmacSha256Hex(std::string(20, '\xaa'), std::string(50, '\xdd')),
          "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe",
          "RFC 4231 case 3");
  // Case 6 exercises the key-longer-than-block path, where RFC 2104 says the
  // key is replaced by its own hash.
  CheckEq(hash::HmacSha256Hex(
              std::string(131, '\xaa'),
              "Test Using Larger Than Block-Size Key - Hash Key First"),
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
          "RFC 4231 case 6 (key longer than block)");
  CheckEq(hash::HmacSha256Hex(
              std::string(131, '\xaa'),
              "This is a test using a larger than block-size key and a larger "
              "than block-size data. The key needs to be hashed before being "
              "used by the HMAC algorithm."),
          "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2",
          "RFC 4231 case 7 (long key and data)");

  // A key of exactly the block size must not be hashed first.
  Check(hash::HmacSha256Hex(std::string(64, 'k'), "x") !=
            hash::HmacSha256Hex(hash::Sha256Hex(std::string(64, 'k')), "x"),
        "a block-sized key is used directly, not hashed");
}

void TestCompilerArgs() {
  Section("args::CompilerArgs (gcc/clang)");

  auto simple = args::Parse({"g++", "-c", "-O2", "foo.cc", "-o", "foo.o"});
  Check(simple.cacheable(), "a plain -c compile is cacheable");
  CheckEq(simple.source, "foo.cc", "source located");
  CheckEq(simple.output, "foo.o", "output located");
  Check(simple.language == args::Language::kCxx, "language inferred from .cc");

  // -I/-D must stay out of the key: their effect is already in the
  // preprocessed text, and hashing them would block cross-directory hits.
  auto with_includes =
      args::Parse({"gcc", "-c", "-I", "/a/inc", "-DFOO=1", "-Ijoined", "x.c", "-o", "x.o"});
  bool leaked = false;
  for (const std::string& arg : with_includes.key_args) {
    if (arg.find("/a/inc") != std::string::npos || arg == "-DFOO=1" ||
        arg == "-Ijoined") {
      leaked = true;
    }
  }
  Check(!leaked, "preprocessor flags are excluded from the cache key");
  bool kept = false;
  for (const std::string& arg : with_includes.base_args) {
    if (arg == "/a/inc") kept = true;
  }
  Check(kept, "preprocessor flags are still passed to the compiler");

  // Codegen flags must be in the key.
  bool has_o2 = false;
  for (const std::string& arg : simple.key_args) {
    if (arg == "-O2") has_o2 = true;
  }
  Check(has_o2, "-O2 is part of the cache key");

  // -o must not be, or the same object cached under two names would miss.
  auto out_a = args::Parse({"g++", "-c", "-O2", "f.cc", "-o", "a.o"});
  auto out_b = args::Parse({"g++", "-c", "-O2", "f.cc", "-o", "b.o"});
  CheckEq(util::Join(out_a.key_args, " "), util::Join(out_b.key_args, " "),
          "the output path is not part of the cache key");

  // Joined -o form.
  auto joined_o = args::Parse({"g++", "-c", "f.cc", "-ofoo.o"});
  CheckEq(joined_o.output, "foo.o", "parses -ofoo.o");

  // Implicit output name.
  auto implicit = args::Parse({"g++", "-c", "dir/f.cc"});
  CheckEq(implicit.output, "f.o", "derives the default object name");

  // Dependency flags.
  auto deps = args::Parse({"g++", "-c", "-MD", "-MF", "out/f.d", "f.cc", "-o", "out/f.o"});
  Check(deps.generates_deps, "detects -MD");
  CheckEq(deps.depfile, "out/f.d", "detects -MF");
  Check(!deps.dep_target_explicit, "no -MT means vcache supplies the target");
  auto implicit_dep = args::Parse({"g++", "-c", "-MD", "f.cc", "-o", "out/f.o"});
  CheckEq(implicit_dep.depfile, "out/f.d", "derives the implicit .d name");
  auto mt = args::Parse({"g++", "-c", "-MD", "-MT", "custom", "f.cc", "-o", "f.o"});
  Check(mt.dep_target_explicit, "detects an explicit -MT");

  // Dependency flags must not reach the preprocessing command: gcc rejects -MP
  // or -MF unless -M/-MM is also present, which would fail every -E run.
  auto full_deps =
      args::Parse({"g++", "-c", "-MMD", "-MP", "-MF", "f.d", "f.cc", "-o", "f.o"});
  bool dep_flag_in_base = false;
  for (const std::string& arg : full_deps.base_args) {
    if (arg == "-MMD" || arg == "-MP" || arg == "-MF" || arg == "f.d") {
      dep_flag_in_base = true;
    }
  }
  Check(!dep_flag_in_base, "dependency flags are kept out of base_args");

  // kbuild never spells the dependency flags the way the driver does: it hands
  // them to the preprocessor with -Wp,. Left opaque, the depfile path -- which
  // is absolute for everything the kernel builds under tools/ -- lands in the
  // cache key, and no two checkouts ever agree on it.
  {
    auto wp = args::Parse({"gcc", "-Wp,-MMD,dir/.f.o.d", "-c", "f.c", "-o",
                           "dir/f.o"});
    Check(wp.generates_deps, "-Wp,-MMD names a dependency file");
    CheckEq(wp.depfile, "dir/.f.o.d", "and vcache knows where it goes");
    for (const std::string& arg : wp.base_args) {
      Check(arg.find("-Wp,") == std::string::npos,
            "the -Wp, form is not passed through to the preprocessor");
    }
    for (const std::string& arg : wp.key_args) {
      Check(arg.find("dir/.f.o.d") == std::string::npos,
            "and the depfile path stays out of the key");
    }

    // The host tools under tools/ use the two-flag form with absolute paths.
    auto wp_mt = args::Parse({"gcc", "-Wp,-MD,/abs/b/.f.o.d", "-Wp,-MT,/abs/b/f.o",
                              "-c", "f.c", "-o", "/abs/b/f.o"});
    CheckEq(wp_mt.depfile, "/abs/b/.f.o.d", "-Wp,-MD names the file");
    Check(wp_mt.dep_target_explicit, "-Wp,-MT is an explicit target");
    bool keyed_target = false;
    for (const std::string& arg : wp_mt.key_args) {
      if (arg == "/abs/b/f.o") keyed_target = true;
    }
    Check(keyed_target,
          "the -Wp, target is keyed, since it is written into the depfile");

    // Anything inside a -Wp, that is not a dependency option is left exactly
    // as it was, because it may well affect the generated code.
    auto wp_other = args::Parse({"gcc", "-Wp,-D,FOO", "-c", "f.c", "-o", "f.o"});
    bool kept = false;
    for (const std::string& arg : wp_other.base_args) {
      if (arg == "-Wp,-D,FOO") kept = true;
    }
    Check(kept, "a non-dependency -Wp, is passed through untouched");

    auto wp_mixed =
        args::Parse({"gcc", "-Wp,-MMD,f.d,-D,FOO", "-c", "f.c", "-o", "f.o"});
    CheckEq(wp_mixed.depfile, "f.d", "a mixed -Wp, still yields the depfile");
    bool residual = false;
    for (const std::string& arg : wp_mixed.base_args) {
      if (arg == "-Wp,-D,FOO") residual = true;
    }
    Check(residual, "and the rest of it survives as its own -Wp,");
  }

  // A file gcc reads after preprocessing has to be hashed, or its contents are
  // invisible to the key. The kernel generates its randstruct seed per tree.
  {
    auto seeded = args::Parse({"gcc", "-frandomize-layout-seed-file=scripts/seed",
                               "-c", "f.c", "-o", "f.o"});
    bool keyed = false;
    for (const std::string& path : seeded.key_files) {
      if (path == "scripts/seed") keyed = true;
    }
    Check(keyed, "the randstruct seed file is hashed into the key");
  }
  bool has_mp = false, has_mmd = false;
  for (const std::string& arg : full_deps.dep_args) {
    if (arg == "-MP") has_mp = true;
    if (arg == "-MMD") has_mmd = true;
  }
  Check(has_mp && has_mmd, "dependency flags are recorded in dep_args");
  // -MP changes what the .d file contains, so it must affect the key.
  bool mp_in_key = false;
  for (const std::string& arg : full_deps.key_args) {
    if (arg == "-MP") mp_in_key = true;
  }
  Check(mp_in_key, "-MP is part of the cache key");

  // -march=native and friends stay cacheable: the parse records them so the
  // compile path can resolve them to a concrete target for the key.
  {
    const auto native =
        args::Parse({"g++", "-c", "a.cc", "-march=native", "-mtune=native", "-o", "a.o"});
    Check(native.cacheable(), "-march=native is cacheable");
    Check(native.native_flags.size() == 2, "both native flags are recorded");
    Check(Contains(native.native_flags, "-march=native"), "-march=native recorded");
    Check(Contains(native.native_flags, "-mtune=native"), "-mtune=native recorded");
    Check(Contains(native.key_args, "-march=native"), "-march=native is in the key");
    Check(Contains(native.base_args, "-march=native"),
          "-march=native still reaches the compiler");

    const auto explicit_arch = args::Parse({"g++", "-c", "a.cc", "-march=znver4", "-o", "a.o"});
    Check(explicit_arch.native_flags.empty(),
          "an explicit -march= needs no target probe");
    Check(Contains(explicit_arch.key_args, "-march=znver4"),
          "an explicit -march= is still in the key");
  }

  // -M/-MM without -c is a dependency scan: cacheable, but by a different
  // route, so the parse flags it rather than rejecting it.
  {
    const auto scan = args::Parse({"gcc", "-M", "-MP", "-I", "inc", "a.c", "-o", "a.d"});
    Check(scan.cacheable(), "-M without -c is cacheable");
    Check(scan.dep_only, "-M without -c is a dependency scan");
    CheckEq(scan.depfile, "a.d", "-o names the dependency output");
    Check(scan.output.empty(), "a dependency scan produces no object");

    const auto mf = args::Parse({"gcc", "-MM", "a.c", "-MF", "dep.d"});
    Check(mf.dep_only, "-MM without -c is a dependency scan too");
    CheckEq(mf.depfile, "dep.d", "-MF names the dependency output");

    const auto to_stdout = args::Parse({"gcc", "-M", "a.c"});
    Check(to_stdout.dep_only && to_stdout.cacheable(), "-M to stdout is cacheable");
    Check(to_stdout.depfile.empty(), "no -o or -MF means stdout");

    // With -c the depfile is a side output of a real compile, which is the
    // ordinary cached path, not a scan.
    const auto compiling = args::Parse({"gcc", "-c", "-MD", "a.c", "-o", "a.o"});
    Check(!compiling.dep_only, "-MD alongside -c is not a dependency scan");
    CheckEq(compiling.output, "a.o", "and it still produces an object");
  }

  // Uncacheable shapes.
  Check(!args::Parse({"g++", "a.cc", "-o", "prog"}).cacheable(), "linking is uncacheable");
  Check(!args::Parse({"g++", "-c", "a.cc", "b.cc"}).cacheable(),
        "multiple inputs are uncacheable");
  Check(!args::Parse({"g++", "-E", "a.cc"}).cacheable(), "-E is uncacheable");
  Check(!args::Parse({"g++", "-c", "-M", "-MG", "a.cc"}).cacheable(),
        "-MG is uncacheable: a manifest cannot verify a file that is not there");
  Check(!args::Parse({"g++", "-c"}).cacheable(), "no input is uncacheable");
  Check(!args::Parse({"g++", "-c", "-save-temps", "a.cc", "-o", "a.o"}).cacheable(),
        "-save-temps is uncacheable");
  Check(!args::Parse({"g++", "-c", "a.s", "-o", "a.o"}).cacheable(),
        "plain assembly is uncacheable");

  // Precompiled headers. Under -fpch-preprocess the preprocessed text names the
  // .gch instead of expanding the header, so it no longer stands in for it.
  {
    const auto pch_pp = args::Parse(
        {"g++", "-c", "-fpch-preprocess", "-include", "h.h", "a.cc", "-o", "a.o"});
    Check(!pch_pp.cacheable(), "-fpch-preprocess is uncacheable");
    CheckEq(pch_pp.uncacheable ? pch_pp.uncacheable->Describe() : "",
            "unsupported flag: -fpch-preprocess", "and the reason names the flag");

    // clang re-emits the PCH's header text under -E, so these stay cacheable.
    const auto driver_pch =
        args::Parse({"clang++", "-c", "-include-pch", "h.pch", "a.cc", "-o", "a.o"});
    Check(driver_pch.cacheable(), "-include-pch is cacheable");
    CheckEq(driver_pch.source, "a.cc", "-include-pch's value is not a source file");
    const auto cc1_pch = args::Parse({"clang++", "-c", "-Xclang", "-include-pch",
                                      "-Xclang", "h.pch", "a.cc", "-o", "a.o"});
    Check(cc1_pch.cacheable(), "-Xclang -include-pch -Xclang h.pch is cacheable");

    // An unvalidated PCH may predate an edit to the header the key now covers.
    const auto driver_unvalidated = args::Parse(
        {"clang++", "-c", "-fno-validate-pch", "-include-pch", "h.pch", "a.cc", "-o", "a.o"});
    CheckEq(driver_unvalidated.uncacheable ? driver_unvalidated.uncacheable->Describe() : "",
            "unsupported flag: -fno-validate-pch", "-fno-validate-pch is declined");
    const auto cc1_unvalidated =
        args::Parse({"clang++", "-c", "-Xclang", "-fno-validate-pch", "-include-pch", "h.pch",
                     "a.cc", "-o", "a.o"});
    CheckEq(cc1_unvalidated.uncacheable ? cc1_unvalidated.uncacheable->Describe() : "",
            "unsupported flag: -fno-validate-pch", "-Xclang -fno-validate-pch is declined");
    CheckEq(cc1_unvalidated.source, "a.cc", "-Xclang's value is not a source file");
  }
  Check(args::Parse({"gcc", "-c", "a.S", "-o", "a.o"}).cacheable(),
        "preprocessed assembly is cacheable");
  Check(!args::Parse({"g++", "-c", "a.cc", "-o", "/dev/null"}).cacheable(),
        "writing to /dev/null is uncacheable");

  // Incoming prefix maps are captured, not passed through.
  auto incoming = args::Parse({"g++", "-c", "-ffile-prefix-map=/a=/b", "f.cc", "-o", "f.o"});
  Check(incoming.incoming_prefix_maps.size() == 1, "captures an incoming prefix map");
  bool passed_through = false;
  for (const std::string& arg : incoming.base_args) {
    if (util::StartsWith(arg, "-ffile-prefix-map")) passed_through = true;
  }
  Check(!passed_through, "does not pass an incoming prefix map to the compiler");

  // -x overrides the extension.
  auto forced = args::Parse({"gcc", "-x", "c++", "-c", "f.c", "-o", "f.o"});
  Check(forced.language == args::Language::kCxx, "-x overrides the extension");

  Check(args::Parse({"g++", "-c", "-g", "f.cc", "-o", "f.o"}).generates_debug_info,
        "detects -g");
}

void TestClangArgs() {
  Section("args::CompilerArgs (clang specifics)");

  // clang passes options through to -cc1 in -Xclang pairs. Both halves must be
  // consumed as a unit, or the value looks like a source file. This exact
  // sequence comes from Firedancer, and sccache declines to cache it.
  auto fsqrt = args::Parse({"clang", "-c", "-O2", "a.c", "-o", "a.o", "-Xclang",
                            "-target-feature", "-Xclang", "+fast-vector-fsqrt"});
  Check(fsqrt.cacheable(), "-Xclang pairs stay cacheable");
  CheckEq(fsqrt.source, "a.c", "the -Xclang value is not mistaken for a source");
  CheckEq(fsqrt.output, "a.o", "the output is still found after -Xclang pairs");

  // Two invocations differing only inside -Xclang must not collide: the value
  // reaches code generation, so it has to reach the key.
  auto other = args::Parse({"clang", "-c", "-O2", "a.c", "-o", "a.o", "-Xclang",
                            "-target-feature", "-Xclang", "+avx2"});
  Check(fsqrt.key_args != other.key_args,
        "the -Xclang value is part of the key");

  // --target changes code generation and must be keyed; it is also the flag a
  // cross build differs by, so a collision here would be silent and severe.
  auto host = args::Parse({"clang", "-c", "a.c", "-o", "a.o"});
  auto cross = args::Parse({"clang", "-c", "a.c", "-o", "a.o", "-target",
                            "aarch64-linux-gnu"});
  Check(host.key_args != cross.key_args, "-target is part of the key");

  // Flags that write a second file alongside the object. Caching these would
  // return the object and silently omit the companion, so each must decline.
  struct Case {
    std::vector<std::string> extra;
    const char* what;
  };
  const std::vector<Case> side_outputs = {
      {{"--coverage"}, "--coverage (.gcno)"},
      {{"-ftest-coverage"}, "-ftest-coverage (.gcno)"},
      {{"-fprofile-arcs"}, "-fprofile-arcs (.gcno)"},
      {{"-fstack-usage"}, "-fstack-usage (.su)"},
      {{"-ftime-trace"}, "-ftime-trace (.json)"},
      {{"-fsave-optimization-record"}, "-fsave-optimization-record (.opt.yaml)"},
      {{"-fdump-tree-all"}, "-fdump-tree-all"},
      {{"-MJ", "frag.json"}, "-MJ (compilation database fragment)"},
      {{"-serialize-diagnostics", "a.dia"}, "-serialize-diagnostics (.dia)"},
      {{"-gen-cdb-fragment-path", "/tmp/cdb"}, "-gen-cdb-fragment-path"},
      {{"-gsplit-dwarf"}, "-gsplit-dwarf (.dwo)"},
      {{"-gsplit-dwarf=split"}, "-gsplit-dwarf=split (.dwo)"},
  };
  for (const Case& c : side_outputs) {
    std::vector<std::string> argv = {"clang", "-c", "a.c", "-o", "a.o"};
    argv.insert(argv.end(), c.extra.begin(), c.extra.end());
    auto parsed = args::Parse(argv);
    Check(!parsed.cacheable(), std::string("declines ") + c.what);
  }

  // The value-taking forms must have their value consumed, or it is read as a
  // second input and the parse describes the wrong command.
  auto mj = args::Parse({"clang", "-c", "a.c", "-o", "a.o", "-MJ", "frag.json"});
  CheckEq(mj.source, "a.c", "-MJ's value is not treated as a source file");

  // -gsplit-dwarf=single keeps the debug sections inside the object, so there
  // is no companion file and no reason to decline it. Verified against clang
  // rather than assumed from the flag's name.
  auto single = args::Parse(
      {"clang", "-g", "-gsplit-dwarf=single", "-c", "a.c", "-o", "a.o"});
  Check(single.cacheable(), "-gsplit-dwarf=single stays cacheable");

  // A plain clang compile with none of the above is still perfectly cacheable;
  // the checks above must not have made clang uncacheable in general.
  auto plain = args::Parse({"clang", "-c", "-O2", "-Wall", "a.c", "-o", "a.o"});
  Check(plain.cacheable(), "an ordinary clang compile is still cacheable");
}

void TestRustcArgs() {
  Section("args::RustcArgs");

  auto rs = args::ParseRustc({"rustc", "--crate-name", "demo", "--crate-type", "lib",
                              "--emit=dep-info,link", "--out-dir", "target/deps",
                              "-C", "opt-level=3", "--extern", "bar=/abs/libbar.rlib",
                              "-L", "dependency=/abs/deps", "src/lib.rs"});
  Check(rs.cacheable(), "a normal cargo-style invocation is cacheable");
  CheckEq(rs.source, "src/lib.rs", "crate root located");
  CheckEq(rs.out_dir, "target/deps", "out-dir located");
  CheckEq(rs.crate_name, "demo", "crate name located");
  Check(rs.emit_kinds.size() == 2, "parses --emit list");
  Check(rs.externs.size() == 1 && rs.externs[0].name == "bar" &&
            rs.externs[0].path == "/abs/libbar.rlib",
        "parses --extern");

  // Search paths and the extern's location vary between checkouts; the crate
  // contents are hashed separately, so neither belongs in the key.
  bool leaked = false;
  for (const std::string& arg : rs.key_args) {
    if (arg.find("/abs/deps") != std::string::npos ||
        arg.find("/abs/libbar.rlib") != std::string::npos ||
        arg.find("target/deps") != std::string::npos) {
      leaked = true;
    }
  }
  Check(!leaked, "search paths and extern paths stay out of the key");
  bool has_opt = false;
  for (size_t i = 0; i + 1 < rs.key_args.size(); ++i) {
    if (rs.key_args[i] == "-C" && rs.key_args[i + 1] == "opt-level=3") has_opt = true;
  }
  Check(has_opt, "-C opt-level is part of the key");

  auto joined = args::ParseRustc({"rustc", "--emit=link", "--out-dir", "o",
                                  "-Cdebuginfo=2", "src/main.rs"});
  Check(joined.cacheable(), "parses joined -C form");

  auto has_pair = [](const std::vector<std::string>& args, const std::string& opt,
                     const std::string& value) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
      if (args[i] == opt && args[i + 1] == value) return true;
    }
    return false;
  };
  auto mentions = [](const std::vector<std::string>& args, const std::string& text) {
    for (const std::string& arg : args) {
      if (arg.find(text) != std::string::npos) return true;
    }
    return false;
  };
  // cargo points -C incremental into the target directory, which differs
  // between checkouts.
  const std::vector<std::pair<std::string, std::vector<std::string>>> incremental_spellings = {
      {"separate", {"-C", "incremental=/a/b"}},
      {"joined", {"-Cincremental=/a/b"}},
      {"long separate", {"--codegen", "incremental=/a/b"}},
      {"long joined", {"--codegen=incremental=/a/b"}}};
  for (const auto& [form, spelling] : incremental_spellings) {
    std::vector<std::string> argv = {"rustc", "--emit=link", "--out-dir", "o",
                                     "-C", "opt-level=3"};
    argv.insert(argv.end(), spelling.begin(), spelling.end());
    argv.push_back("src/lib.rs");
    auto inc = args::ParseRustc(argv);
    Check(inc.cacheable(), form + " -C incremental is cacheable");
    Check(has_pair(inc.base_args, "-C", "incremental=/a/b"),
          form + " -C incremental reaches rustc");
    Check(!mentions(inc.key_args, "/a/b"),
          form + " -C incremental directory stays out of the key");
    Check(has_pair(inc.key_args, "-C", "incremental"),
          form + " -C incremental still marks the key as incremental");
    Check(has_pair(inc.key_args, "-C", "opt-level=3"),
          form + " -C incremental leaves other -C options in the key");
    Check(!mentions(inc.dep_info_args, "incremental"),
          form + " -C incremental is left off the dep-info run");
    Check(has_pair(inc.dep_info_args, "-C", "opt-level=3"),
          form + " -C incremental leaves other -C options on the dep-info run");
  }

  Check(!args::ParseRustc({"rustc", "--emit=link", "src/lib.rs"}).cacheable(),
        "no --out-dir is uncacheable");
  Check(!args::ParseRustc({"rustc", "--out-dir", "o", "src/lib.rs"}).cacheable(),
        "no --emit is uncacheable");
  Check(!args::ParseRustc({"rustc", "--emit=link", "--out-dir", "o", "a.rs", "b.rs"})
             .cacheable(),
        "multiple inputs are uncacheable");
  Check(!args::ParseRustc({"rustc", "--emit=asm=/tmp/x.s", "--out-dir", "o", "a.rs"})
             .cacheable(),
        "--emit with an explicit path is uncacheable");

  Check(args::LooksLikeRustc("/home/u/.cargo/bin/rustc"), "recognises rustc by path");
  Check(!args::LooksLikeRustc("/usr/bin/g++"), "does not mistake g++ for rustc");
}

// A 64-character key whose last digit is `i`, for ordering assertions.
std::string StateKey(int i) {
  return std::string(hash::kDigestHexLen - 1, '0') + "0123456789abcdef"[i];
}

void TestDepScanKeyArgs() {
  Section("core::DepScanKeyArgs");

  const auto key_args = [](const std::vector<std::string>& argv,
                           const core::RootMap& roots) {
    return util::Join(core::DepScanKeyArgs(args::Parse(argv), roots,
                                           /*keep_link_args=*/false),
                      "\n");
  };

  const core::RootMap no_roots = MakeRoots({});
  CheckEq(key_args({"gcc", "-MM", "a/main.c"}, no_roots),
          key_args({"gcc", "-MM", "a/main.c"}, no_roots),
          "the same source path gives the same key material");
  Check(key_args({"gcc", "-MM", "a/main.c"}, no_roots) !=
            key_args({"gcc", "-MM", "b/main.c"}, no_roots),
        "different relative source paths give different key material");
  Check(key_args({"gcc", "-M", "/home/u/a/main.c"}, no_roots) !=
            key_args({"gcc", "-M", "/home/u/b/main.c"}, no_roots),
        "different absolute source paths give different key material");

  const core::RootMap one_root = MakeRoots({"/home/u=proj"});
  Check(key_args({"gcc", "-M", "/home/u/a/main.c"}, one_root) !=
            key_args({"gcc", "-M", "/home/u/b/main.c"}, one_root),
        "two sources under one root stay apart");

  // Per-checkout roots name both copies by one canonical path, so they share.
  CheckEq(key_args({"gcc", "-M", "/home/u/a/main.c"}, MakeRoots({"/home/u/a=proj"})),
          key_args({"gcc", "-M", "/home/u/b/main.c"}, MakeRoots({"/home/u/b=proj"})),
          "per-checkout roots give one canonical source path");

  CheckEq(key_args({"gcc", "-MM", "main.c", "-MF", "a.d"}, no_roots),
          key_args({"gcc", "-MM", "main.c", "-MF", "b.d"}, no_roots),
          "where the answer goes is not key material");
}

void TestRustManifest() {
  Section("rust::manifest");

  auto scratch = util::MakeTempDir("vcache-rust-manifest-");
  Check(scratch.has_value(), "scratch dir");
  if (!scratch) return;
  struct ScratchGuard {
    std::string path;
    ~ScratchGuard() { util::RemoveRecursive(path); }
  } guard{*scratch};

  // Root specs are symlink-resolved and Canonicalize matches the spelling as
  // given, so every test path is built from the resolved scratch directory.
  const std::string base = util::RealPath(*scratch).value_or(*scratch);
  const std::string root = base + "/a";
  const std::string other_root = base + "/b";
  for (const std::string& tree : {root, other_root}) {
    util::MakeDirs(tree + "/src");
    util::WriteFileAtomic(tree + "/src/lib.rs", "mod helper;\n");
    util::WriteFileAtomic(tree + "/src/helper.rs", "pub fn v() -> u32 { 1 }\n");
  }
  const core::RootMap roots = MakeRoots({root + "=crate"});

  rust::RustManifestState state;
  state.key = StateKey(1);
  for (const char* rel : {"/src/lib.rs", "/src/helper.rs"}) {
    state.files.emplace_back(roots.Canonicalize(root + rel),
                             hash::HashFile(root + rel).value_or(""));
  }
  state.env_deps = {{"VCACHE_UT_SET", "a\\\\b"},
                    {"VCACHE_UT_UNSET", std::nullopt},
                    {"VCACHE_UT_EMPTY", ""}};
  state.externs = {{"proc_macro", ""}, {"syn", hash::HashString("libsyn")}};
  CheckEq(state.files[0].first, "/vcache/crate/src/lib.rs", "files are recorded canonically");

  rust::RustManifestState spaced;
  spaced.key = StateKey(2);
  spaced.files.emplace_back("/vcache/crate/src/a b.rs", hash::HashString("x"));

  // ---- serialise / parse ----
  const std::string text = rust::RenderRustManifest({state, spaced});
  std::vector<rust::RustManifestState> parsed;
  Check(rust::ParseRustManifest(text, &parsed), "manifest parses back");
  CheckEq(rust::RenderRustManifest(parsed), text, "manifest round-trips exactly");
  Check(parsed.size() == 2 && parsed[0].key == state.key &&
            parsed[0].files == state.files && parsed[0].externs.size() == 2 &&
            parsed[0].externs[0].digest.empty() &&
            parsed[0].externs[1].digest == state.externs[1].digest,
        "files and extern digests survive the round trip");
  Check(parsed.size() == 2 && parsed[0].env_deps.size() == 3 &&
            parsed[0].env_deps[0].value == std::optional<std::string>("a\\\\b") &&
            !parsed[0].env_deps[1].value.has_value() &&
            parsed[0].env_deps[2].value == std::optional<std::string>(""),
        "set, unset and empty env values stay distinct");
  Check(parsed.size() == 2 && parsed[1].files.size() == 1 &&
            parsed[1].files[0].first == "/vcache/crate/src/a b.rs",
        "a path containing a space survives");

  const std::string header = "vcache-rustmanifest-1\n";
  const std::vector<std::pair<std::string, std::string>> malformed = {
      {"vcache-rustmanifest-0\n", "a different header"},
      {header + "F " + hash::HashString("x") + " /a.rs\n", "a file before any state"},
      {header + "state abc\n", "a short key"},
      {header + "state " + StateKey(1) + "\nF abc /a.rs\n", "a short file digest"},
      {header + "state " + StateKey(1) + "\nE =x\n", "an env line without a name"},
      {header + "state " + StateKey(1) + "\nQ x\n", "an unknown record"},
  };
  for (const auto& [bad_text, what] : malformed) {
    std::vector<rust::RustManifestState> ignored;
    Check(!rust::ParseRustManifest(bad_text, &ignored), "rejects " + what);
  }

  CheckEq(rust::EscapeEnvDepValue("a\\b\nc\rd=e"), "a\\\\b\\nc\\rd=e",
          "env values are escaped the way rustc writes them");

  // ---- matching ----
  ::setenv("VCACHE_UT_SET", "a\\b", 1);
  ::unsetenv("VCACHE_UT_UNSET");
  ::setenv("VCACHE_UT_EMPTY", "", 1);
  auto mismatch = [&](const std::vector<rust::RustExtern>& externs,
                      const core::RootMap& with_roots) {
    return rust::FindRustStateMismatch(state, externs, with_roots, {}).value_or("");
  };
  CheckEq(mismatch(state.externs, roots), "", "an unchanged state matches");
  CheckEq(mismatch(state.externs, MakeRoots({other_root + "=crate"})), "",
          "the same state matches from another checkout");

  util::WriteFileAtomic(root + "/src/helper.rs", "pub fn v() -> u32 { 2 }\n");
  CheckEq(mismatch(state.externs, roots), root + "/src/helper.rs changed",
          "a changed source rejects the state");
  util::WriteFileAtomic(root + "/src/helper.rs", "pub fn v() -> u32 { 1 }\n");
  CheckEq(mismatch(state.externs, roots), "", "reverting the source matches again");
  std::error_code ec;
  fs::rename(root + "/src/helper.rs", root + "/helper.rs.away", ec);
  CheckEq(mismatch(state.externs, roots), root + "/src/helper.rs is gone",
          "a deleted source rejects the state");
  fs::rename(root + "/helper.rs.away", root + "/src/helper.rs", ec);

  ::setenv("VCACHE_UT_SET", "a\\c", 1);
  CheckEq(mismatch(state.externs, roots), "env VCACHE_UT_SET is 'a\\\\c', was 'a\\\\b'",
          "a changed env value rejects the state");
  ::setenv("VCACHE_UT_SET", "a\\b", 1);
  ::setenv("VCACHE_UT_UNSET", "x", 1);
  CheckEq(mismatch(state.externs, roots), "env VCACHE_UT_UNSET is 'x', was unset",
          "setting an unset variable rejects the state");
  ::unsetenv("VCACHE_UT_UNSET");
  ::unsetenv("VCACHE_UT_EMPTY");
  CheckEq(mismatch(state.externs, roots), "env VCACHE_UT_EMPTY is unset, was ''",
          "unsetting an empty variable rejects the state");
  ::setenv("VCACHE_UT_EMPTY", "", 1);

  std::vector<rust::RustExtern> rebuilt = state.externs;
  rebuilt[1].digest = hash::HashString("libsyn rebuilt");
  CheckEq(mismatch(rebuilt, roots), "extern syn changed",
          "a changed extern digest rejects the state");
  std::vector<rust::RustExtern> extra = state.externs;
  extra.push_back({"quote", hash::HashString("libquote")});
  CheckEq(mismatch(extra, roots), "the --extern set differs",
          "a different extern set rejects the state");
  CheckEq(mismatch(state.externs, roots), "", "the restored inputs match again");

  // ---- path-valued env deps ----
  const std::vector<std::string> out_dir_only = {"OUT_DIR"};
  const core::RootMap other_roots = MakeRoots({other_root + "=crate"});
  CheckEq(rust::KeyedEnvDepValue("OUT_DIR", root + "/out", out_dir_only, roots)
              .value_or("unset"),
          "/vcache/crate/out", "a listed path env dep is keyed canonically");
  CheckEq(rust::KeyedEnvDepValue("OUT_DIR", root + "/out", {}, roots).value_or("unset"),
          root + "/out", "an unlisted env dep is keyed raw");
  CheckEq(rust::KeyedEnvDepValue("OUT_DIR", std::nullopt, out_dir_only, roots)
              .value_or("unset"),
          "unset", "an unset listed env dep stays unset");
  rust::RustManifestState with_out_dir = state;
  with_out_dir.env_deps.push_back(
      {"OUT_DIR", rust::KeyedEnvDepValue("OUT_DIR", root + "/out", out_dir_only, roots)});
  ::setenv("OUT_DIR", (other_root + "/out").c_str(), 1);
  CheckEq(rust::FindRustStateMismatch(with_out_dir, state.externs, other_roots, out_dir_only)
              .value_or(""),
          "", "a listed OUT_DIR matches from another checkout");
  CheckEq(rust::FindRustStateMismatch(with_out_dir, state.externs, other_roots, {})
              .value_or(""),
          "env OUT_DIR is '" + other_root + "/out', was '/vcache/crate/out'",
          "an unlisted OUT_DIR is compared raw");
  ::setenv("OUT_DIR", "/elsewhere/out", 1);
  CheckEq(rust::FindRustStateMismatch(with_out_dir, state.externs, other_roots, out_dir_only)
              .value_or(""),
          "env OUT_DIR is '/elsewhere/out', was '/vcache/crate/out'",
          "a listed OUT_DIR outside every root still rejects the state");
  ::unsetenv("OUT_DIR");

  // ---- the cap ----
  std::vector<rust::RustManifestState> states;
  for (int i = 0; i < 10; ++i) {
    rust::RustManifestState fresh;
    fresh.key = StateKey(i);
    states = core::PrependManifestState(std::move(fresh), std::move(states),
                                       &rust::RustManifestState::key);
  }
  auto order = [&]() {
    std::string out;
    for (const rust::RustManifestState& s : states) out += s.key.back();
    return out;
  };
  CheckEq(order(), "98765432", "eight states, newest first, oldest dropped");
  states = core::PrependManifestState(states[4], states, &rust::RustManifestState::key);
  CheckEq(order(), "59876432", "re-recording a state moves it to the front once");
  rust::RustManifestState newest;
  newest.key = StateKey(10);
  states = core::PrependManifestState(std::move(newest), std::move(states),
                                     &rust::RustManifestState::key);
  CheckEq(order(), "a5987643", "the least recently used state is the one evicted");
}

void TestSigV4() {
  Section("storage::sigv4");

  using namespace vcache::storage::sigv4;

  CheckEq(Sha256Hex(""),
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "SHA-256 of the empty string");
  CheckEq(Sha256Hex("hello"),
          "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
          "SHA-256 of \"hello\"");
  CheckEq(HmacSha256Hex("key", "The quick brown fox jumps over the lazy dog"),
          "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8",
          "HMAC-SHA256 reference vector");

  // AWS documents this signing key for secret/date/region/service below; it is
  // the standard check that the derivation chain is correct.
  const std::string secret = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
  std::string k = HmacSha256("AWS4" + secret, "20150830");
  k = HmacSha256(k, "us-east-1");
  k = HmacSha256(k, "iam");
  k = HmacSha256(k, "aws4_request");
  CheckEq(util::HexEncode(reinterpret_cast<const unsigned char*>(k.data()), k.size()),
          "c4afb1cc5771d871763a393e44b703571b55cc28424d1a5e86da6ed3c154a4b9",
          "AWS documented signing-key derivation");

  CheckEq(UriEncode("/vcache/ab/cd", /*keep_slash=*/true), "/vcache/ab/cd",
          "UriEncode keeps slashes when asked");
  CheckEq(UriEncode("/a b", /*keep_slash=*/true), "/a%20b", "UriEncode escapes spaces");
  CheckEq(UriEncode("a/b", /*keep_slash=*/false), "a%2Fb",
          "UriEncode escapes slashes when asked");
  CheckEq(UriEncode("-_.~", true), "-_.~", "UriEncode leaves unreserved characters");

  // Full Authorization header, cross-checked against an independent
  // implementation of SigV4.
  const std::string payload_hash = Sha256Hex("body");
  CheckEq(payload_hash,
          "230d8358dc8e8890b4c58deeb62912ee2f20357ae92a5cc861b98e68fe31acb5",
          "payload hash");
  const std::string auth = BuildAuthorization(
      "PUT", "/vcache/ab/cdef", /*query=*/"", "bkt.s3.us-east-1.amazonaws.com",
      "20150830T123600Z", "20150830", payload_hash, /*session_token=*/"",
      "us-east-1", "AKIDEXAMPLE", secret);
  Check(auth.find("Signature=6a89ab420d95da1c11102e64cd1c0c95942eff50b1b5efa666e12e5383701605") !=
            std::string::npos,
        "full request signature matches the reference implementation");
  Check(auth.find("SignedHeaders=host;x-amz-content-sha256;x-amz-date") !=
            std::string::npos,
        "signed header list is correct");
  Check(auth.find("Credential=AKIDEXAMPLE/20150830/us-east-1/s3/aws4_request") !=
            std::string::npos,
        "credential scope is correct");

  // A session token must join the signed header set.
  const std::string with_token = BuildAuthorization(
      "GET", "/k", /*query=*/"", "h", "20150830T123600Z", "20150830",
      Sha256Hex(""), "TOKEN", "us-east-1", "AKIDEXAMPLE", secret);
  Check(with_token.find("x-amz-security-token") != std::string::npos,
        "session token is included in SignedHeaders");

  // The canonical query string is part of the signed request, so a listing and
  // a bare GET of the same path must not produce the same signature.
  const std::string unqueried = BuildAuthorization(
      "GET", "/bkt", /*query=*/"", "h", "20150830T123600Z", "20150830",
      Sha256Hex(""), "", "us-east-1", "AKIDEXAMPLE", secret);
  const std::string queried = BuildAuthorization(
      "GET", "/bkt", "list-type=2", "h", "20150830T123600Z", "20150830",
      Sha256Hex(""), "", "us-east-1", "AKIDEXAMPLE", secret);
  Check(unqueried != queried, "the query string is covered by the signature");
}

void TestS3ResponseParsing() {
  Section("storage::S3 response parsing");

  std::time_t when = 0;
  Check(storage::ParseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT", &when),
        "parses an IMF-fixdate");
  CheckEq(std::to_string(when), "784111777", "IMF-fixdate is interpreted as UTC");

  when = 0;
  Check(storage::ParseIso8601("1994-11-06T08:49:37.000Z", &when),
        "parses an ISO-8601 instant");
  CheckEq(std::to_string(when), "784111777", "both formats agree on the instant");

  // A header vcache cannot read must not be mistaken for an expired entry.
  Check(!storage::ParseHttpDate("not a date", &when), "rejects a non-date");
  Check(!storage::ParseHttpDate("", &when), "rejects an empty date");
  Check(!storage::ParseHttpDate("Sun, 06 Xxx 1994 08:49:37 GMT", &when),
        "rejects an unknown month");

  const std::string listing =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
      "<IsTruncated>true</IsTruncated>"
      "<Contents><Key>vcache/ab/cdef</Key>"
      "<LastModified>2026-01-02T03:04:05.000Z</LastModified>"
      "<Size>1234</Size></Contents>"
      "<Contents><Key>vcache/12/3456</Key>"
      "<LastModified>2026-01-03T03:04:05.000Z</LastModified>"
      "<Size>9</Size></Contents>"
      "<NextContinuationToken>tok</NextContinuationToken>"
      "</ListBucketResult>";

  std::vector<storage::S3Object> objects;
  std::string token;
  Check(storage::ParseListObjectsV2(listing, &objects, &token), "parses a listing");
  Check(objects.size() == 2, "finds every Contents entry");
  if (objects.size() == 2) {
    CheckEq(objects[0].key, "vcache/ab/cdef", "reads the key");
    CheckEq(std::to_string(objects[0].size), "1234", "reads the size");
    CheckEq(objects[1].key, "vcache/12/3456", "reads the second key");
    Check(objects[0].last_modified < objects[1].last_modified,
          "reads timestamps in a comparable order");
  }
  CheckEq(token, "tok", "reports the continuation token");

  // A complete listing must not look truncated, or Trim would loop.
  const std::string last_page =
      "<ListBucketResult><IsTruncated>false</IsTruncated>"
      "<Contents><Key>k</Key><LastModified>2026-01-02T03:04:05.000Z</LastModified>"
      "<Size>1</Size></Contents></ListBucketResult>";
  objects.clear();
  Check(storage::ParseListObjectsV2(last_page, &objects, &token),
        "parses a final page");
  Check(token.empty(), "a complete listing yields no continuation token");

  // An error document returned with a 200 must not read as an empty bucket,
  // which Trim would otherwise treat as "nothing to delete" and report clean.
  objects.clear();
  Check(!storage::ParseListObjectsV2("<Error><Code>AccessDenied</Code></Error>",
                                     &objects, &token),
        "rejects a body that is not a listing");
}


// --------------------------------------------------------------------------
// Disk cache eviction.
//
// The disk layer shards on the first byte of the key, and eviction used to be
// per-shard: each of the 256 shards was trimmed against max_size/256. Entries
// are not uniformly sized, so a handful of large objects landing in one shard
// blew through that 1/256th share and evicted the shard's hot entries while
// the rest of the cache sat nearly empty -- 1.52 GiB held in a 4 GiB cache,
// and identical builds plateauing at 84-86% hits.
//
// There is no clock to inject: the layer uses each file's mtime as its LRU
// timestamp, deliberately, because relatime makes atime unreliable. So the
// mtime *is* the clock, and Age() below stamps it directly. Without that,
// entries written inside the same second sort arbitrarily and any ordering
// assertion is a coin flip that passes on a fast machine.
// --------------------------------------------------------------------------

// A cache directory that cleans itself up, so a failing case cannot leak state
// into the next one.
class TempCacheDir {
 public:
  TempCacheDir() {
    char tmpl[] = "/tmp/vcache-disk-test-XXXXXX";
    const char* made = ::mkdtemp(tmpl);
    path_ = made ? made : "";
  }
  ~TempCacheDir() {
    if (!path_.empty()) util::RemoveRecursive(path_);
  }
  TempCacheDir(const TempCacheDir&) = delete;
  TempCacheDir& operator=(const TempCacheDir&) = delete;

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

// Backdates an entry's mtime, which is what the trimmer sorts on. Larger
// `seconds` means older, so Age(k, 100) is evicted before Age(k, 1).
void Age(const std::string& file, int64_t seconds) {
  struct stat st {};
  if (::stat(file.c_str(), &st) != 0) return;
  // POSIX spells these st_atim/st_mtim; Apple spells them st_atimespec and
  // st_mtimespec, predating the standardised names.
#if defined(__APPLE__)
  const struct timespec atime = st.st_atimespec, mtime = st.st_mtimespec;
#else
  const struct timespec atime = st.st_atim, mtime = st.st_mtim;
#endif
  struct timespec times[2];
  times[0].tv_sec = atime.tv_sec - seconds;
  times[0].tv_nsec = 0;
  times[1].tv_sec = mtime.tv_sec - seconds;
  times[1].tv_nsec = 0;
  ::utimensat(0, file.c_str(), times, 0);
}

// Keys are hex digests in production and the shard is just the first two
// characters, so a test can place an entry in a chosen shard by construction.
std::string KeyIn(const std::string& shard, const std::string& suffix) {
  return shard + suffix;
}

std::string EntryPath(const std::string& dir, const std::string& key) {
  return dir + "/" + key.substr(0, 2) + "/" + key.substr(2);
}

size_t CountFiles(const std::string& dir) {
  return util::ListFilesRecursive(dir).size();
}

void TestDiskTrimPins() {
  Section("storage::disk trim");

  const std::string payload(4096, 'a');
  const std::string k1 = "aa1111";
  const std::string k2 = "bb2222";
  const std::string k3 = "cc3333";

  {
    TempCacheDir tmp;
    storage::DiskStorage disk(tmp.path(), 10 * 1024, false);
    Check(disk.Put(k1, payload) && disk.Put(k2, payload), "two entries fit under the high water");
    Age(EntryPath(tmp.path(), k1), 100);
    Age(EntryPath(tmp.path(), k2), 50);
    util::MakeDirs(tmp.path() + "/daemon/pending");
    util::WriteFileAtomic(tmp.path() + "/daemon/pending/" + k1, "");
    Check(disk.Put(k3, payload), "the store that crosses the budget succeeds");
    Check(util::FileExists(EntryPath(tmp.path(), k1)), "a pending upload is not evicted");
    Check(!util::FileExists(EntryPath(tmp.path(), k2)), "an unpinned older entry is evicted");
    Check(util::FileExists(EntryPath(tmp.path(), k3)), "the new entry stays");
  }
  {
    TempCacheDir tmp;
    storage::DiskStorage disk(tmp.path(), 10 * 1024, false);
    Check(disk.Put(k1, payload) && disk.Put(k2, payload), "the control stores two entries");
    Age(EntryPath(tmp.path(), k1), 100);
    Age(EntryPath(tmp.path(), k2), 50);
    util::MakeDirs(tmp.path() + "/daemon/pending");
    Check(disk.Put(k3, payload), "the control store succeeds");
    Check(!util::FileExists(EntryPath(tmp.path(), k1)),
          "without a pending marker the oldest entry is evicted");
  }
  {
    TempCacheDir tmp;
    for (const std::string& key : {k1, k2, k3}) {
      util::MakeDirs(tmp.path() + "/" + key.substr(0, 2));
      util::WriteFileAtomic(EntryPath(tmp.path(), key), payload);
      util::MakeDirs(tmp.path() + "/daemon/pending");
      util::WriteFileAtomic(tmp.path() + "/daemon/pending/" + key, "");
    }
    storage::DiskStorage disk(tmp.path(), 10 * 1024, false);
    const pid_t pid = ::fork();
    Check(pid >= 0, "the all-pinned trim can be watched");
    if (pid == 0) {
      ::alarm(2);
      disk.Trim();
      _exit(0);
    }
    if (pid > 0) {
      int status = 0;
      ::waitpid(pid, &status, 0);
      Check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "an all-pinned trim finishes");
    }
    Check(util::FileExists(EntryPath(tmp.path(), k1)) &&
              util::FileExists(EntryPath(tmp.path(), k2)) &&
              util::FileExists(EntryPath(tmp.path(), k3)),
          "an all-pinned trim removes nothing");
  }
}

void TestDiskStorageEviction() {
  Section("disk storage: global eviction");

  // ---- layout and round trip -------------------------------------------
  {
    TempCacheDir tmp;
    storage::DiskStorage disk(tmp.path(), 1 << 20, /*read_only=*/false);
    Check(disk.Put("abcdef", "payload"), "Put accepts a normal key");
    std::string got;
    Check(disk.Get("abcdef", &got) && got == "payload", "Get returns what Put stored");
    Check(util::ReadFile(tmp.path() + "/ab/cdef").has_value(),
          "the entry lands in the shard named by the first two key characters");
    Check(!disk.Put("ab", "x"), "a key too short to shard is refused");
    Check(!disk.Get("ab", &got), "a key too short to shard never hits");
  }

  // ---- the regression --------------------------------------------------
  // Four 4 KiB entries in one shard is 16 KiB, far past this cache's 1 KiB
  // per-shard share, while the whole cache holds well under its 256 KiB
  // budget. Nothing should be evicted. Under the old per-shard rule this is
  // exactly the case that threw away hot entries.
  {
    TempCacheDir tmp;
    const uint64_t max_size = 256 * 1024;  // shard share = 1 KiB
    storage::DiskStorage disk(tmp.path(), max_size, /*read_only=*/false);
    const std::string payload(4096, 'x');
    for (int i = 0; i < 4; ++i) {
      disk.Put(KeyIn("aa", "skew" + std::to_string(i)), payload);
    }
    Check(CountFiles(tmp.path()) == 4,
          "a shard far over its 1/256th share is left alone below the global budget");
    Check(disk.TotalSize() == 4 * 4096, "TotalSize sums every shard");
  }

  // ---- eviction happens once the global budget is exceeded --------------
  {
    TempCacheDir tmp;
    const uint64_t max_size = 8192;
    storage::DiskStorage disk(tmp.path(), max_size, /*read_only=*/false);
    const std::string payload(1024, 'y');
    for (int i = 0; i < 12; ++i) {
      const std::string key = KeyIn("aa", "e" + std::to_string(i));
      disk.Put(key, payload);
      Age(EntryPath(tmp.path(), key), 100 - i);  // e0 oldest, e11 newest
    }
    Check(disk.TotalSize() <= max_size,
          "the cache is brought back under its configured size");
    Check(disk.TotalSize() > 0, "eviction stops at the target rather than emptying the cache");
    std::string got;
    Check(disk.Get(KeyIn("aa", "e11"), &got), "the newest entry survives");
    Check(!disk.Get(KeyIn("aa", "e0"), &got), "the oldest entry is the one evicted");
  }

  // ---- eviction is global, not shard-local ------------------------------
  // An old entry in a quiet shard must be reclaimed to make room for pressure
  // in a busy one. The sizes here are chosen so the two rules disagree: the
  // stale entry sits *below* its own shard's 1/256th share, so per-shard
  // trimming would never touch it no matter how full the cache got, while the
  // cache as a whole is over budget and global eviction takes it first. A
  // smaller cache would not discriminate -- every entry would exceed its
  // shard's share and the old rule would evict the stale one for its own
  // local reasons, passing the test for the wrong reason.
  {
    TempCacheDir tmp;
    const uint64_t max_size = 1 << 20;  // shard share = 4 KiB
    storage::DiskStorage disk(tmp.path(), max_size, /*read_only=*/false);

    const std::string stale = KeyIn("bb", "ancient");
    disk.Put(stale, std::string(1024, 'z'));  // 1 KiB, under its 4 KiB share
    Age(EntryPath(tmp.path(), stale), 10000);

    const std::string chunk(16 * 1024, 'z');
    for (int i = 0; i < 64; ++i) {  // 1 MiB into one shard
      const std::string key = KeyIn("aa", "hot" + std::to_string(i));
      disk.Put(key, chunk);
      Age(EntryPath(tmp.path(), key), 10);
    }

    std::string got;
    Check(!disk.Get(stale, &got),
          "an old entry below its own shard's share is still evicted for global pressure");
    Check(disk.Get(KeyIn("aa", "hot63"), &got),
          "the newer entries that caused the pressure are kept");
  }

  // ---- a hit refreshes the LRU timestamp --------------------------------
  {
    TempCacheDir tmp;
    const uint64_t max_size = 8192;
    storage::DiskStorage disk(tmp.path(), max_size, /*read_only=*/false);
    const std::string payload(1024, 'w');

    const std::string touched = KeyIn("cc", "touched");
    disk.Put(touched, payload);
    Age(EntryPath(tmp.path(), touched), 10000);

    std::string got;
    Check(disk.Get(touched, &got), "the entry is present before the read");
    // The read above rewrote its mtime, so it is now the newest thing here.

    for (int i = 0; i < 10; ++i) {
      const std::string key = KeyIn("dd", "fill" + std::to_string(i));
      disk.Put(key, payload);
      Age(EntryPath(tmp.path(), key), 500);
    }
    Check(disk.Get(touched, &got),
          "a read protects an otherwise stale entry from the next eviction");
  }

  // ---- explicit Trim ----------------------------------------------------
  {
    TempCacheDir tmp;
    const uint64_t max_size = 4096;
    storage::DiskStorage disk(tmp.path(), max_size, /*read_only=*/false);
    // Write past the budget without going through Put's own trigger, so this
    // case exercises Trim() on its own.
    util::MakeDirs(tmp.path() + "/ee");
    for (int i = 0; i < 8; ++i) {
      util::WriteFileAtomic(tmp.path() + "/ee/manual" + std::to_string(i),
                            std::string(1024, 'q'));
      Age(tmp.path() + "/ee/manual" + std::to_string(i), 100 - i);
    }
    Check(disk.TotalSize() == 8 * 1024, "the cache starts over its budget");
    disk.Trim();
    Check(disk.TotalSize() <= max_size, "Trim enforces the global budget");
    Check(util::ReadFile(tmp.path() + "/ee/manual7").has_value(),
          "Trim keeps the newest entries");
    Check(!util::ReadFile(tmp.path() + "/ee/manual0").has_value(),
          "Trim drops the oldest entries first");
  }

  // ---- a removal that genuinely failed is not counted as freed ----------
  // The trimmer subtracts an entry's size whenever the entry is gone, which
  // includes a concurrent vcache having unlinked it first (ENOENT). That rule
  // must not extend to a removal that actually failed: those bytes are still
  // in the cache, and crediting them lets the trimmer stop early believing it
  // freed space it never freed.
  //
  // The sizes make the two rules disagree. The four oldest entries sit in an
  // unwritable shard and cannot be unlinked; crediting them alone reaches the
  // target exactly, so a trimmer that counts failures stops right there and
  // frees nothing at all. A correct one keeps going into the writable shard.
  //
  // An unwritable directory is the deterministic way to get a failing
  // unlink(2). Root ignores directory permissions, so skip there rather than
  // assert something untrue.
  if (::geteuid() != 0) {
    TempCacheDir tmp;
    const uint64_t max_size = 5120;  // target = 0.8 * 5120 = 4096

    const std::string locked = tmp.path() + "/aa";
    util::MakeDirs(locked);
    for (int i = 0; i < 4; ++i) {
      const std::string f = locked + "/stuck" + std::to_string(i);
      util::WriteFileAtomic(f, std::string(1024, 's'));
      Age(f, 100 - i);  // oldest, so the trimmer reaches these first
    }

    const std::string open_shard = tmp.path() + "/bb";
    util::MakeDirs(open_shard);
    for (int i = 0; i < 4; ++i) {
      const std::string f = open_shard + "/free" + std::to_string(i);
      util::WriteFileAtomic(f, std::string(1024, 'f'));
      Age(f, 10 - i);  // newer, so they are only reached if the stuck four
                       // are correctly not credited
    }

    ::chmod(locked.c_str(), 0555);
    storage::DiskStorage disk(tmp.path(), max_size, /*read_only=*/false);
    disk.Trim();
    ::chmod(locked.c_str(), 0755);

    Check(CountFiles(locked) == 4, "entries that cannot be unlinked stay put");
    Check(CountFiles(open_shard) == 0,
          "and the trimmer keeps evicting past them rather than crediting "
          "bytes it never freed");
    Check(disk.TotalSize() == 4 * 1024,
          "so the cache really is trimmed to what could be removed");
  }

  // ---- a read-only layer never mutates the cache ------------------------
  {
    TempCacheDir tmp;
    util::MakeDirs(tmp.path() + "/ff");
    for (int i = 0; i < 8; ++i) {
      util::WriteFileAtomic(tmp.path() + "/ff/ro" + std::to_string(i),
                            std::string(1024, 'r'));
    }
    storage::DiskStorage disk(tmp.path(), 1024, /*read_only=*/true);
    Check(!disk.writable(), "a read-only layer reports itself unwritable");
    Check(!disk.Put("aabbcc", "nope"), "a read-only layer refuses to store");
    disk.Trim();
    Check(CountFiles(tmp.path()) == 8,
          "a read-only layer evicts nothing even when far over budget");
  }
}

}  // namespace

// A replayed artifact has to be as readable as a compiled one. mkstemp creates
// at 0600, so before this was fixed every cache hit produced an object only its
// writer could read -- invisible on a developer laptop, and a hard failure the
// moment a container builds as root and CI hashes the output as someone else.
void TestWrittenFileMode() {
  Section("written file permissions");

  const std::string dir = "/tmp/vcache-mode-test";
  fs::remove_all(dir);
  const std::string path = dir + "/artifact.o";
  const std::string strict_path = dir + "/strict.o";

  // The umask is read once per process, which is right for a tool that lives
  // for one compilation and wrong for a test that wants two umasks. Fork for
  // the restrictive case, and do it before this process has written anything
  // so the child is the first caller and initialises its own copy.
  const pid_t child = ::fork();
  if (child == 0) {
    ::umask(077);
    _exit(vcache::util::WriteFileAtomic(strict_path, "object bytes") ? 0 : 1);
  }
  int child_status = 0;
  ::waitpid(child, &child_status, 0);
  Check(child_status == 0, "WriteFileAtomic succeeds under a strict umask");

  const mode_t previous = ::umask(022);
  const bool wrote = vcache::util::WriteFileAtomic(path, "object bytes");
  ::umask(previous);

  Check(wrote, "WriteFileAtomic succeeds");

  struct ::stat st {};
  const bool statted = ::stat(path.c_str(), &st) == 0;
  Check(statted, "the file exists");
  const mode_t mode = st.st_mode & 07777;

  Check(mode == 0644, "umask 022 yields 0644, not mkstemp's 0600");
  Check((mode & S_IRGRP) != 0, "group can read it");
  Check((mode & S_IROTH) != 0, "another uid can read it");
  Check((mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0, "and it is not executable");

  // umask is honoured rather than hardcoded, so a deliberately private build
  // stays private.
  struct ::stat strict_st {};
  ::stat(strict_path.c_str(), &strict_st);
  Check((strict_st.st_mode & 07777) == 0600, "umask 077 still yields 0600");

  fs::remove_all(dir);
}

// Callers report std::strerror(errno) when a write fails, so errno has to
// survive the failure. It did not: MakeDirs left whatever the probing stat()
// set, and WriteFileAtomic ran close()/unlink() before returning. In CI this
// showed up as "disk: write <entry>: No such file or directory" for what was
// really a permission problem -- a diagnosis that sent the reader looking for
// a missing directory that was never missing.
void TestWriteErrnoIsPreserved() {
  Section("write failures report their own errno");

  const std::string dir = "/tmp/vcache-errno-test";
  fs::remove_all(dir);
  fs::create_directories(dir);

  const std::string locked = dir + "/locked";
  fs::create_directories(locked);
  ::chmod(locked.c_str(), 0555);

  // Creating a shard below an unwritable directory. The failing call is
  // mkdir(2) with EACCES; the last call before it is a stat(2) that failed
  // with ENOENT, which is exactly the value that used to be reported.
  errno = 0;
  const bool made = vcache::util::MakeDirs(locked + "/shard");
  const int made_errno = errno;
  Check(!made, "MakeDirs fails below an unwritable directory");
  Check(made_errno == EACCES,
        "and reports EACCES, not the ENOENT its own stat() left behind");

  // Same story one level up: the whole point is what the caller prints.
  errno = 0;
  const bool wrote = vcache::util::WriteFileAtomic(locked + "/shard/entry", "x");
  const int wrote_errno = errno;
  Check(!wrote, "WriteFileAtomic fails when the shard cannot be created");
  Check(wrote_errno == EACCES, "and it too reports EACCES");

  // A rename that cannot land: the target path is a directory. close() and
  // unlink() run between the failure and the return, and must not speak over
  // it.
  const std::string occupied = dir + "/occupied";
  fs::create_directories(occupied);
  errno = 0;
  const bool clobbered = vcache::util::WriteFileAtomic(occupied, "x");
  const int clobbered_errno = errno;
  Check(!clobbered, "WriteFileAtomic fails when the target is a directory");
  Check(clobbered_errno == EISDIR || clobbered_errno == ENOTEMPTY,
        "and reports the rename's errno, not the cleanup's");

  ::chmod(locked.c_str(), 0755);
  fs::remove_all(dir);
}

#if defined(__linux__)
// Armed by TestWriteCloseFailure: the next close(2) of a vcache temp file
// really closes it and then fails with EIO, which is how NFS or an exhausted
// quota reports a write the kernel accepted and could not complete. Every other
// close goes straight to the system call.
static std::atomic<bool> g_fail_next_temp_close{false};

extern "C" int close(int fd) {
  if (g_fail_next_temp_close.load()) {
    char link[64];
    std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    char target[PATH_MAX];
    const ssize_t n = ::readlink(link, target, sizeof(target) - 1);
    if (n > 0) {
      target[n] = '\0';
      if (std::strstr(target, "/.vcache-tmp-") != nullptr &&
          g_fail_next_temp_close.exchange(false)) {
        ::syscall(SYS_close, fd);
        errno = EIO;
        return -1;
      }
    }
  }
  return static_cast<int>(::syscall(SYS_close, fd));
}
#endif

// Without an fsync, close() is the only call that can report a write the
// kernel accepted and then failed to complete. Renaming after that error
// published a short file as a complete cache entry.
void TestWriteCloseFailure() {
  Section("WriteFileAtomic checks close");
#if defined(__linux__)
  auto dir = vcache::util::MakeTempDir("vcache-close-test-");
  Check(dir.has_value(), "close test scratch exists");
  if (!dir) return;
  struct CloseGuard {
    std::string path;
    ~CloseGuard() { vcache::util::RemoveRecursive(path); }
  } guard{*dir};

  const std::string path = *dir + "/entry";
  g_fail_next_temp_close = true;
  errno = 0;
  const bool wrote = vcache::util::WriteFileAtomic(path, "bytes never written back", false);
  const int wrote_errno = errno;
  const bool fault_reached = !g_fail_next_temp_close.exchange(false);
  Check(fault_reached, "the injected close failure was reached");
  Check(!wrote, "WriteFileAtomic fails when close reports a delayed write error");
  Check(wrote_errno == EIO, "and reports close's EIO");
  Check(!vcache::util::FileExists(path), "nothing is published under the destination");
  Check(fs::is_empty(*dir), "the temp file is removed");
#endif
}

// CloneFile places link outputs and must not disturb another writer's temp:
// a stale one, or one in flight from a process in another PID namespace that
// shares the store and happens to have the same pid.
void TestCloneFile() {
  Section("util::CloneFile");
  auto dir = vcache::util::MakeTempDir("vcache-clone-test-");
  Check(dir.has_value(), "clone test scratch exists");
  if (!dir) return;
  struct CloneGuard {
    std::string path;
    ~CloneGuard() { vcache::util::RemoveRecursive(path); }
  } guard{*dir};

  const std::string from = *dir + "/stored";
  vcache::util::WriteFileAtomic(from, "cached output");
  const std::string to = *dir + "/out/artifact";
  const std::string pid_named_temp = to + ".tmp." + std::to_string(::getpid());
  vcache::util::WriteFileAtomic(pid_named_temp, "another writer's bytes");

  Check(vcache::util::CloneFile(from, to), "CloneFile succeeds beside another writer's temp");
  Check(vcache::util::ReadFile(to) == std::optional<std::string>("cached output"),
        "the destination holds the source bytes");
  Check(vcache::util::ReadFile(pid_named_temp) ==
            std::optional<std::string>("another writer's bytes"),
        "the other writer's temp is neither truncated nor renamed away");
  size_t leftover_temps = 0;
  for (const auto& entry : fs::directory_iterator(*dir + "/out")) {
    if (entry.path().filename().string().starts_with(".vcache-tmp-")) ++leftover_temps;
  }
  Check(leftover_temps == 0, "CloneFile leaves no temp of its own behind");

  const std::string nested = *dir + "/new/sub/artifact";
  Check(vcache::util::CloneFile(from, nested), "CloneFile creates the destination's directory");
  Check(vcache::util::ReadFile(nested) == std::optional<std::string>("cached output"),
        "and the clone holds the source bytes");
  struct ::stat st {};
  Check(::stat(nested.c_str(), &st) == 0 &&
            (st.st_mode & 07777) == vcache::util::DefaultFileMode(),
        "the clone gets the umask's default mode, not mkstemp's 0600");
}

void TestStats() {
  Section("core::Stats reasons");
  using core::Counter;
  using core::Reason;

  const std::string old_format = "1\n2\n3\n4\n5\n6\n7\n8\n9\n";
  const core::Stats old_stats = core::ParseStats(old_format);
  Check(old_stats.Get(Counter::kHitDisk) == 1 && old_stats.Get(Counter::kUncacheable) == 4 &&
            old_stats.Get(Counter::kCacheMediaError) == 9,
        "a nine-line file from an older version parses positionally");
  Check(old_stats.reasons.empty(), "and has no reasons");
  CheckEq(core::RenderStats(old_stats), old_format, "and renders back byte for byte");

  core::Stats stats;
  stats.Add(Counter::kMiss, 5);
  stats.AddReason(Reason::kPreprocessOnly);
  stats.AddReason(Reason::kPreprocessOnly);
  stats.AddReason(Reason::kNotCompileOnly);
  stats.AddReason(Reason::kNoRustDepInfo);
  stats.AddReason(Reason::kNoTempDir);
  Check(stats.Get(Counter::kUncacheable) == 3,
        "an uncacheable reason also counts towards uncacheable");
  Check(stats.Get(Counter::kPreprocessFailed) == 1,
        "a preprocess reason also counts towards preprocess failed");
  Check(stats.GetReason(Reason::kNoTempDir) == 1 && stats.GetReason(Reason::kIncbin) == 0,
        "a passthrough reason is counted on its own");
  const std::string rendered = core::RenderStats(stats);
  CheckEq(rendered,
          "0\n0\n5\n3\n0\n0\n0\n1\n0\n"
          "reason\tlink\t1\n"
          "reason\tno rust dep-info\t1\n"
          "reason\tno temp dir\t1\n"
          "reason\tpreprocess only\t2\n",
          "reason lines follow the nine positional counters");
  const core::Stats reparsed = core::ParseStats(rendered);
  Check(reparsed.reasons == stats.reasons &&
            std::equal(std::begin(reparsed.values), std::end(reparsed.values),
                       std::begin(stats.values)),
        "ParseStats reads back what RenderStats wrote");

  const core::Stats extended = core::ParseStats(old_format +
                                                "reason\tlink\t4\n"
                                                "histogram 1 2 3\n"
                                                "\n"
                                                "42\n"
                                                "reason\tfrom a newer version\t6\n");
  Check(extended.Get(Counter::kUncacheable) == 4 && extended.Get(Counter::kCacheMediaError) == 9,
        "unknown trailing lines leave the positional counters alone");
  Check(extended.GetReason(Reason::kNotCompileOnly) == 4 && extended.reasons.size() == 2,
        "unknown trailing lines are ignored");
  CheckEq(core::RenderStats(extended),
          old_format + "reason\tfrom a newer version\t6\nreason\tlink\t4\n",
          "a reason this version does not know is carried through");

  const core::Stats damaged = core::ParseStats(old_format +
                                               "reason\tlink\tx1\n"
                                               "reason\tlink\t7x\n"
                                               "reason\tlink\t-3\n"
                                               "reason\tlink\t\n"
                                               "reason\tlink\n"
                                               "reason\tlink\t1\textra\n"
                                               "reason\t\t1\n"
                                               "reason\tlink\t99999999999999999999999\n"
                                               "reason\tpreprocess only\t2\n");
  Check(damaged.GetReason(Reason::kNotCompileOnly) == 0,
        "a reason line with a bad count is ignored");
  Check(damaged.GetReason(Reason::kPreprocessOnly) == 2 && damaged.reasons.size() == 1,
        "and the lines after it still parse");
  Check(damaged.Get(Counter::kUncacheable) == 4, "and the counters are untouched");

  const std::string shown = core::FormatStats(stats, "/c", 0, 1);
  Check(shown.find("uncacheable         3\n"
                   "  preprocess only     2\n"
                   "  link                1\n"
                   "passthrough         1\n"
                   "  no temp dir         1\n"
                   "compile failed      0\n") != std::string::npos,
        "--show-stats lists reasons under their counter, largest first");
  Check(shown.find("preprocess failed   1\n"
                   "  no rust dep-info    1\n"
                   "cache media errors  0\n") != std::string::npos,
        "preprocess failures are broken down too");
  const std::string plain = core::FormatStats(core::Stats{}, "/c", 0, 1);
  Check(plain.find("passthrough") == std::string::npos && plain.find("\n  ") == std::string::npos,
        "--show-stats adds nothing while every reason is zero");

  TempCacheDir dir;
  core::RecordDecision(dir.path(), Reason::kNotCompileOnly);
  core::RecordDecision(dir.path(), {Reason::kUnsupportedFlag, "-save-temps"});
  core::RecordCounter(dir.path(), Counter::kMiss);
  const core::Stats recorded = core::ReadStats(dir.path());
  Check(recorded.Get(Counter::kUncacheable) == 2 && recorded.Get(Counter::kMiss) == 1 &&
            recorded.GetReason(Reason::kNotCompileOnly) == 1 &&
            recorded.GetReason(Reason::kUnsupportedFlag) == 1,
        "RecordDecision persists the reason and its counter");
  Check(core::ZeroStats(dir.path()) && core::ReadStats(dir.path()).reasons.empty(),
        "--zero-stats clears the reasons");

  CheckEq(core::Decision(Reason::kUnsupportedFlag, "-MG").Describe(), "unsupported flag: -MG",
          "the logged text starts with the reason's table name");
  Check(args::Parse({"g++", "-E", "a.cc"}).uncacheable->reason == Reason::kPreprocessOnly,
        "-E is declined as preprocess only");
  Check(args::Parse({"cc", "a.o", "-o", "a"}).uncacheable->reason == Reason::kNotCompileOnly,
        "a link through the compile path is declined as link");
  Check(args::ParseRustc({"rustc", "--emit=link", "a.rs"}).uncacheable->reason == Reason::kNoOutDir,
        "rustc without --out-dir is declined as no --out-dir");
  Check(args::ParseLink({"cc", "a.o"}).uncacheable->reason == Reason::kNoLinkOutput,
        "a link without -o is declined as no -o");
}

namespace {

std::string AllocHelperPath() {
  if (util::FileExists("bin/vcache_test_alloc")) return "bin/vcache_test_alloc";
  if (auto self = util::SelfPath()) {
    const std::string beside = util::DirName(*self) + "/vcache_test_alloc";
    if (util::FileExists(beside)) return beside;
  }
  return {};
}

void TestRunRusage() {
  Section("util::Run rusage");

  Check(util::RssKbFromRuMaxrss(65536, false) == 65536,
        "Linux ru_maxrss is already kibibytes");
  Check(util::RssKbFromRuMaxrss(static_cast<long>(65536) * 1024, true) == 65536,
        "macOS ru_maxrss in bytes converts to kibibytes");
  Check(util::RssKbFromRuMaxrss(0, true) == 0 && util::RssKbFromRuMaxrss(-1, false) == 0,
        "a non-positive ru_maxrss is zero kibibytes");

  const util::ProcResult exited = util::Run({"/bin/sh", "-c", "exit 3"});
  Check(exited.exit_code == 3 && !exited.signalled,
        "wait4 reports the same exit code as waitpid");

  const util::ProcResult killed = util::Run({"/bin/sh", "-c", "kill -ABRT $$"});
  Check(killed.signalled && killed.exit_code == 128 + SIGABRT,
        "a child killed by a signal is reported with exit 128 plus the signal");

  const util::ProcResult slept = util::Run({"/bin/sleep", "0.2"});
  Check(slept.exit_code == 0 && slept.wall_ms >= 150 && slept.wall_ms < 10000,
        "wall_ms covers the child and stays below 10000");

  const std::string helper = AllocHelperPath();
  Check(!helper.empty(), "bin/vcache_test_alloc exists");
  if (helper.empty()) return;

  const util::ProcResult big = util::Run({helper, "64"});
  Check(big.exit_code == 0 && big.max_rss_kb >= 65536,
        "64 MiB child reports max_rss_kb >= 65536 (got " + std::to_string(big.max_rss_kb) + ")");
  Check(big.wall_ms < 10000, "64 MiB child wall_ms is below 10000");

  const util::ProcResult small = util::Run({helper, "1"});
  Check(small.exit_code == 0 && small.max_rss_kb < 65536 && small.wall_ms < 10000,
        "1 MiB child reports max_rss_kb < 65536 (got " + std::to_string(small.max_rss_kb) + ")");
}

void TestCost() {
  Section("core::cost");

  TempCacheDir tree_a;
  TempCacheDir tree_b;
  const core::RootMap roots_a = MakeRoots({tree_a.path() + "=proj"});
  const core::RootMap roots_b = MakeRoots({tree_b.path() + "=proj"});
  Check(!roots_a.roots().empty() && !roots_b.roots().empty(), "cost-key roots resolved");
  if (roots_a.roots().empty() || roots_b.roots().empty()) return;

  const std::string file_a = roots_a.roots().front().path + "/a.cc";
  const std::string file_b = roots_b.roots().front().path + "/a.cc";
  util::WriteFileAtomic(file_a, "int a(){return 1;}");
  util::WriteFileAtomic(file_b, "int a(){return 2;} /* different bytes */");

  const std::vector<std::string> noisy = {"-O2", "-g", "-I", "/tmp/inc", "-DFOO", "-include",
                                          "x.h", "-o", "a.o", "/tmp/a.cc", "-fPIC"};
  const std::vector<std::string> bare = {"-O2", "-g", "-fPIC"};
  CheckEq(core::ComputeCostKey("compile", file_a, "c++", noisy, roots_a),
          core::ComputeCostKey("compile", file_b, "c++", bare, roots_b),
          "different contents and roots with the same canonical path share a cost key");
  Check(core::ComputeCostKey("compile", file_a, "c++", {"-O2"}, roots_a) !=
            core::ComputeCostKey("compile", file_a, "c++", {"-O3"}, roots_a),
        "-O2 and -O3 are different cost keys");
  Check(core::ComputeCostKey("compile", file_a, "c++", {"-O2"}, roots_a) !=
            core::ComputeCostKey("link", file_a, "c++", {"-O2"}, roots_a),
        "compile and link are different cost keys");
  Check(core::ComputeCostKey("compile", file_a, "c", {"-O2"}, roots_a) !=
            core::ComputeCostKey("compile", file_a, "c++", {"-O2"}, roots_a),
        "C and C++ are different cost keys");
  Check(core::ComputeCostKey("compile", file_a, "c++", {"-O2", "-march=x86-64"}, roots_a) !=
            core::ComputeCostKey("compile", file_a, "c++", {"-O2"}, roots_a),
        "-march stays in the cost key");
  Check(core::ComputeCostKey("rustc", file_a, "rust", {"-C", "opt-level=2"}, roots_a) !=
            core::ComputeCostKey("rustc", file_a, "rust", {"-C", "opt-level=3"}, roots_a),
        "-C opt-level stays in the rustc cost key");
  CheckEq(core::ComputeCostKey("rustc", file_a, "rust",
                               {"-C", "opt-level=2", "-C", "metadata=aaa", "-C",
                                "extra-filename=-aaa"},
                               roots_a),
          core::ComputeCostKey("rustc", file_a, "rust",
                               {"-C", "opt-level=2", "-C", "metadata=bbb", "-C",
                                "extra-filename=-bbb"},
                               roots_a),
          "rustc metadata and extra-filename share one cost key");
  Check(core::ComputeCostKey("rustc", file_a, "rust", {"--crate-name", "serde", "-C", "opt-level=2"},
                             roots_a) !=
            core::ComputeCostKey("rustc", file_a, "rust",
                                 {"--crate-name", "syn", "-C", "opt-level=2"}, roots_a),
        "two crates compiling the same canonical source have different cost keys");
  CheckEq(core::ComputeCostKey("rustc", file_a, "rust", {"--crate-name", "serde"}, roots_a),
          core::ComputeCostKey("rustc", file_b, "rust", {"--crate-name", "serde"}, roots_b),
          "one crate in two worktrees shares a cost key");
  CheckEq(core::ComputeCostKey("compile", file_a, "c++",
                               {"-O2", "-fdebug-prefix-map=/a=/b"}, roots_a),
          core::ComputeCostKey("compile", file_a, "c++", {"-O2"}, roots_a),
          "a debug prefix map is not part of the cost key");
  CheckEq(core::ComputeCostKey("compile", file_a, "c++",
                               {"-O2", "-fprofile-use=/a/x.profdata"}, roots_a),
          core::ComputeCostKey("compile", file_a, "c++",
                               {"-O2", "-fprofile-use=/b/x.profdata"}, roots_a),
          "a path-valued -fprofile-use is not part of the cost key");
  Check(core::ComputeCostKey("compile", file_a, "c++", {"-O2", "-flto=thin"}, roots_a) !=
            core::ComputeCostKey("compile", file_a, "c++", {"-O2", "-flto=full"}, roots_a),
        "-flto=thin and -flto=full are different cost keys");
  Check(core::ComputeCostKey("rustc", file_a, "rust", {"--edition", "2021"}, roots_a) !=
            core::ComputeCostKey("rustc", file_a, "rust", {"--edition", "2018"}, roots_a),
        "--edition stays in the cost key");
  CheckEq(core::ComputeCostKey("compile", file_a, "c++", {"-O2", "-g1"}, roots_a),
          core::ComputeCostKey("compile", file_a, "c++", {"-O2", "-g1", "-DDEBUG"}, roots_a),
          "-D is not part of the cost key");
  CheckEq(core::ComputeCostKey("compile", file_a, "c++", {"-O2", "-isystem", "/usr/include"},
                               roots_a),
          core::ComputeCostKey("compile", file_a, "c++", {"-O2"}, roots_a),
          "-isystem is not part of the cost key");

  TempCacheDir cache;
  const core::RootMap roots = MakeRoots({cache.path() + "=proj"});
  const std::string source = roots.roots().front().path + "/a.cc";
  util::WriteFileAtomic(source, "int x;");
  const std::string log_path = cache.path() + "/run.log";
  ::setenv("VCACHE_LOG", log_path.c_str(), 1);
  util::InitLogging();

  auto record = [&](uint64_t rss, int exit_code, bool signalled) {
    util::ProcResult proc;
    proc.exit_code = exit_code;
    proc.signalled = signalled;
    proc.max_rss_kb = rss;
    proc.wall_ms = rss;
    core::RecordCompileCost(cache.path(), "compile", source, "c++", {"-O2"}, roots, proc);
  };

  record(10, 0, false);
  const std::string key = core::ComputeCostKey("compile", source, "c++", {"-O2"}, roots);
  core::CompileCost one = core::LoadCompileCost(cache.path(), key);
  Check(one.observations.size() == 1 && one.observations.back().max_rss_kb == 10,
        "the first recorded compile creates a one-line cost file");

  record(11, 0, false);
  core::CompileCost two = core::LoadCompileCost(cache.path(), key);
  Check(two.observations.size() == 2 && two.observations.back().max_rss_kb == 11 &&
            two.observations.front().max_rss_kb == 10,
        "a cost file under 8 observations appends, newest last");

  record(100, 0, false);
  record(40, 0, false);
  const auto estimate = core::EstimateMaxRssKb(cache.path(), key);
  Check(estimate.has_value() && *estimate == 100,
        "the estimate is the max, not the mean and not the last observation");

  for (uint64_t rss = 1; rss <= 6; ++rss) record(rss, 0, false);
  // Observations so far: 10, 11, 100, 40, then 1..6. That is 10. The last 8
  // are 100, 40, 1, 2, 3, 4, 5, 6.
  core::CompileCost capped = core::LoadCompileCost(cache.path(), key);
  Check(capped.observations.size() == 8, "a cost file keeps only the last 8 observations");
  Check(capped.observations.front().max_rss_kb == 100 && capped.observations.back().max_rss_kb == 6,
        "the oldest observations are dropped and the newest stays last");

  util::WriteFileAtomic(cache.path() + "/costs/" + key, "not a cost file\n");
  Check(core::LoadCompileCost(cache.path(), key).observations.empty(),
        "a corrupt cost file reads as empty");
  Check(!core::EstimateMaxRssKb(cache.path(), key).has_value(),
        "a corrupt cost file has no estimate");
  record(7, 0, false);
  core::CompileCost rewritten = core::LoadCompileCost(cache.path(), key);
  Check(rewritten.observations.size() == 1 && rewritten.observations.front().max_rss_kb == 7,
        "the next record rewrites a corrupt cost file");

  const std::string shown = core::FormatCosts(cache.path());
  Check(shown.find("compile records 1 ") != std::string::npos,
        "--show-costs prints a compile row with records 1");
  Check(shown.find("max_rss_kb p50=7 max=7") != std::string::npos,
        "--show-costs p50 of one observation is that observation");
  Check(shown.find(rewritten.source) != std::string::npos,
        "--show-costs prints the source path of a cost key");

  record(9, 1, false);
  auto logged = util::ReadFile(log_path);
  Check(logged && logged->find("cost: compile ") != std::string::npos &&
            logged->find(" exit=1") != std::string::npos,
        "a failed compile is recorded with its exit in the log");

  record(8, 128 + SIGABRT, true);
  logged = util::ReadFile(log_path);
  Check(logged && logged->find(" exit=" + std::to_string(128 + SIGABRT)) != std::string::npos,
        "a signalled compile is recorded with its exit in the log");

  util::ProcResult link_proc;
  link_proc.exit_code = 0;
  link_proc.max_rss_kb = 77;
  link_proc.wall_ms = 5;
  core::RecordCompileCost(cache.path(), "link", source, "", {"-flto"}, roots, link_proc);
  const std::string link_key = core::ComputeCostKey("link", source, "", {"-flto"}, roots);
  const core::CompileCost link_cost = core::LoadCompileCost(cache.path(), link_key);
  Check(link_cost.observations.size() == 1 && link_cost.observations.front().max_rss_kb == 77,
        "a link records the waited-for peak unchanged, including -flto");

  std::string meta = "compiler: g++\nroots:\n";
  core::AppendCostMeta(&meta, 12, 34);
  Check(meta.find("max_rss_kb: 12\n") != std::string::npos &&
            meta.find("wall_ms: 34\n") != std::string::npos,
        "blob meta gains max_rss_kb and wall_ms lines");

  storage::DiskStorage disk(cache.path(), 4096, false);
  Check(disk.Clear(), "clearing the cache succeeds");
  Check(util::FileExists(cache.path() + "/costs/" + key),
        "clear leaves cost records, like compiler memos");

  TempCacheDir blocked;
  util::WriteFileAtomic(blocked.path() + "/costs", "not a directory\n");
  const core::RootMap blocked_roots = MakeRoots({blocked.path() + "=proj"});
  util::ProcResult proc;
  proc.exit_code = 0;
  proc.max_rss_kb = 1;
  proc.wall_ms = 1;
  core::RecordCompileCost(blocked.path(), "compile", source, "c++", {"-O2"}, blocked_roots, proc);
  logged = util::ReadFile(log_path);
  Check(logged && logged->find("cost: could not record (") != std::string::npos,
        "a cost file that cannot be written is logged and does not fail the record");

  TempCacheDir aged_cache;
  const core::RootMap aged_roots = MakeRoots({aged_cache.path() + "=proj"});
  const std::string aged_source = aged_roots.roots().front().path + "/a.cc";
  util::WriteFileAtomic(aged_source, "int x;");
  util::ProcResult aged_proc;
  aged_proc.exit_code = 0;
  aged_proc.max_rss_kb = 4;
  aged_proc.wall_ms = 4;
  core::RecordCompileCost(aged_cache.path(), "compile", aged_source, "c++", {"-O0"}, aged_roots,
                          aged_proc);
  core::RecordCompileCost(aged_cache.path(), "compile", aged_source, "c++", {"-O2"}, aged_roots,
                          aged_proc);
  const std::string old_key =
      core::ComputeCostKey("compile", aged_source, "c++", {"-O0"}, aged_roots);
  const std::string fresh_key =
      core::ComputeCostKey("compile", aged_source, "c++", {"-O2"}, aged_roots);
  const std::string old_path = aged_cache.path() + "/costs/" + old_key;
  auto old_text = util::ReadFile(old_path);
  Check(old_text.has_value(), "the production writer created the cost file to age");
  if (old_text) {
    std::string text = *old_text;
    if (!text.empty() && text.back() == '\n') text.pop_back();
    const size_t nl = text.rfind('\n');
    const std::string line = nl == std::string::npos ? text : text.substr(nl + 1);
    const size_t sp = line.rfind(' ');
    const uint64_t forty_days_ago =
        static_cast<uint64_t>(std::time(nullptr)) - 40ull * 24 * 60 * 60;
    const std::string aged_line = line.substr(0, sp + 1) + std::to_string(forty_days_ago);
    const std::string rewritten =
        (nl == std::string::npos ? std::string() : text.substr(0, nl + 1)) + aged_line + "\n";
    util::WriteFileAtomic(old_path, rewritten);
  }
  storage::DiskStorage trimmer(aged_cache.path(), 1ull << 30, false);
  trimmer.Trim();
  Check(!util::FileExists(old_path), "a cost file 40 days old is removed by trim");
  Check(util::FileExists(aged_cache.path() + "/costs/" + fresh_key),
        "a cost file from today survives trim");
}

void TestRustcFingerprint() {
  Section("rust::fingerprint");

  auto scratch = util::MakeTempDir("vcache-rustc-memo-");
  Check(scratch.has_value(), "rustc memo scratch exists");
  if (!scratch) return;
  struct ScratchGuard {
    std::string path;
    ~ScratchGuard() { util::RemoveRecursive(path); }
  } guard{*scratch};

  const std::string banner_a =
      "#!/bin/sh\n"
      "if [ \"$1\" = \"-vV\" ]; then printf 'release: 9.9.1\\n'; exit 0; fi\n"
      "exit 0\n";
  const std::string banner_b =
      "#!/bin/sh\n"
      "if [ \"$1\" = \"-vV\" ]; then printf 'release: 9.9.2\\n'; exit 0; fi\n"
      "exit 0\n";
  Check(banner_a.size() == banner_b.size(), "the two fake rustc scripts have the same size");

  const std::string rustc = *scratch + "/rustc";
  const std::string copy = *scratch + "/rustc-copy";
  auto install = [](const std::string& path, const std::string& text) {
    util::WriteFileAtomic(path, text);
    ::chmod(path.c_str(), 0755);
  };
  install(rustc, banner_a);
  const std::string first = rust::ResolveRustcFingerprint(rustc, *scratch);
  CheckEq(rust::ResolveRustcFingerprint(rustc, *scratch), first,
          "an unchanged rustc reuses its fingerprint");

  install(rustc, banner_b);
  Check(util::FileSize(rustc).value_or(0) == banner_a.size(),
        "the rewritten rustc keeps the same size");
  Age(rustc, -2);
  const std::string rebuilt = rust::ResolveRustcFingerprint(rustc, *scratch);
  Check(rebuilt != first, "a same-size rustc with a new mtime gets a new fingerprint");

  install(copy, banner_a);
  Age(copy, 100);
  CheckEq(rust::ResolveRustcFingerprint(copy, *scratch), first,
          "two rustc files with one banner share a fingerprint");
}

void TestRustOutputNames() {
  Section("rust::output names");

  Check(rust::IsSafeOutputName("a/b/c.rmeta"), "a/b/c.rmeta is a safe output name");
  Check(rust::IsSafeOutputName("./a.rlib"), "./a.rlib is a safe output name");
  Check(!rust::IsSafeOutputName("../x"), "../x is not a safe output name");
  Check(!rust::IsSafeOutputName("a/../../x"), "a/../../x is not a safe output name");
  Check(!rust::IsSafeOutputName("/etc/x"), "/etc/x is not a safe output name");
  Check(!rust::IsSafeOutputName(""), "an empty output name is not safe");
  const std::string nul_name("a\0b", 3);
  Check(!rust::IsSafeOutputName(nul_name), "an output name with a NUL is not safe");
  Check(rust::IsSafeOutputName("a/..b/c"), "a/..b/c is a safe output name");

  auto scratch = util::MakeTempDir("vcache-rust-names-");
  Check(scratch.has_value(), "output-name scratch exists");
  if (!scratch) return;
  struct ScratchGuard {
    std::string path;
    ~ScratchGuard() { util::RemoveRecursive(path); }
  } guard{*scratch};

  const std::string out = *scratch + "/out";
  util::MakeDirs(out);
  const auto parent_before = util::ListFilesRecursive(*scratch);
  storage::BlobFile safe;
  safe.name = "kept.rlib";
  safe.contents = "kept";
  storage::BlobFile escape;
  escape.name = "../escape";
  escape.contents = "nope";
  const core::RootMap roots;
  Check(!rust::RestoreOutputs({safe, escape}, out, roots, {}),
        "a blob named ../escape is refused");
  Check(!util::FileExists(out + "/kept.rlib"), "a refused restore writes nothing");
  Check(!util::FileExists(*scratch + "/escape"),
        "a refused restore writes nothing outside the output directory");
  Check(util::ListFilesRecursive(*scratch).size() == parent_before.size(),
        "a refused restore leaves the parent directory unchanged");

  const std::string cap = *scratch + "/cap";
  util::MakeDirs(cap);
  util::WriteFileAtomic(cap + "/lib.rlib", "ok");
  util::WriteFileAtomic(*scratch + "/outside-target", "out");
  Check(::symlink((*scratch + "/outside-target").c_str(), (cap + "/link").c_str()) == 0,
        "a symlink out of the capture directory can be planted");
  std::vector<storage::BlobFile> captured;
  Check(!rust::CaptureOutputs(cap, roots, {}, &captured),
        "capturing a name that escapes the output directory fails");
}

void TestUploadGeneration() {
  Section("daemon::upload generation");

  auto scratch = util::MakeTempDir("vcache-upload-");
  Check(scratch.has_value(), "upload scratch exists");
  if (!scratch) return;
  struct ScratchGuard {
    std::string path;
    ~ScratchGuard() { util::RemoveRecursive(path); }
  } guard{*scratch};
  const std::string journal = *scratch + "/pending";
  util::MakeDirs(journal);
  const auto now = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point soonest;

  daemon::UploadQueue queued(journal);
  const auto first_blob = std::make_shared<const std::string>("v1");
  const auto second_blob = std::make_shared<const std::string>("v2-value");
  Check(queued.Enqueue("k", first_blob, true), "the first store is queued");
  Check(queued.Enqueue("k", second_blob, true), "a rewrite while queued is accepted");
  const auto yielded = queued.TakeReady(now, &soonest);
  Check(yielded.has_value() && yielded->blob && *yielded->blob == "v2-value" &&
            yielded->generation == 2,
        "dequeue yields the rewritten blob once");
  Check(!queued.TakeReady(now, &soonest).has_value(), "a rewrite while queued is one upload");
  Check(queued.held_bytes() == second_blob->size(),
        "replacing a held blob adjusts the held byte count");

  const std::string disk_journal = *scratch + "/disk-pending";
  util::MakeDirs(disk_journal);
  daemon::UploadQueue disk(disk_journal);
  Check(disk.Enqueue("m", nullptr, true), "a disk store is queued");
  const auto old = disk.TakeReady(now, &soonest);
  Check(old.has_value() && old->generation == 1, "the first disk upload is generation 1");
  Check(disk.Enqueue("m", nullptr, true), "a rewrite during upload is accepted");
  Check(disk.Finish(*old), "finishing the old generation re-queues");
  Check(disk.JournalExists("m"), "the journal stays while a rewrite is re-queued");
  const auto newer = disk.TakeReady(now, &soonest);
  Check(newer.has_value() && newer->generation == 2,
        "the re-queued upload is the newer generation");
  Check(!disk.Finish(*newer) && disk.empty(),
        "finishing the current generation empties the queue");

  daemon::UploadQueue once("");
  const auto only = std::make_shared<const std::string>("only");
  Check(once.Enqueue("once", only, false), "a single store is queued");
  const auto one = once.TakeReady(now, &soonest);
  Check(one.has_value() && !once.Finish(*one) && !once.TakeReady(now, &soonest).has_value(),
        "a single store is uploaded once");

  daemon::UploadQueue tiny("", 10);
  const auto small = std::make_shared<const std::string>(std::string(6, 'a'));
  const auto big = std::make_shared<const std::string>(std::string(20, 'b'));
  Check(tiny.Enqueue("z", small, false), "a blob under the cap is held");
  Check(!tiny.Enqueue("z", big, false), "a blob over the cap is refused");
  Check(tiny.empty() && tiny.Held("z") == nullptr, "a refused re-put drops the queued blob");

  daemon::UploadQueue first_refuse("", 10);
  Check(!first_refuse.Enqueue("n", big, false) && first_refuse.generation("n") == 0,
        "a refused first store does not bump the generation");

  daemon::UploadQueue flying_refuse("", 10);
  Check(flying_refuse.Enqueue("f", small, false), "an in-flight cap store is queued");
  const auto inflight = flying_refuse.TakeReady(now, &soonest);
  Check(inflight.has_value(), "the in-flight cap store is taken");
  Check(!flying_refuse.Enqueue("f", big, false), "a re-put during upload over the cap is refused");
  Check(flying_refuse.Held("f") == nullptr, "a refused re-put drops the held blob");
  if (inflight) {
    Check(!flying_refuse.Finish(*inflight) &&
              !flying_refuse.TakeReady(now, &soonest).has_value(),
          "a refused re-put during upload does not re-queue the old blob");
  }

  // A later disk store must not upload the blob that was held when the disk
  // write failed. A null blob is what tells the worker to read the disk value.
  daemon::UploadQueue replaced(journal);
  const auto held_v1 = std::make_shared<const std::string>("v1");
  Check(replaced.Enqueue("h", held_v1, true), "the held value is queued");
  Check(replaced.Enqueue("h", nullptr, true), "a disk store of the same key is accepted");
  const auto from_disk = replaced.TakeReady(now, &soonest);
  const std::string replaced_blob =
      from_disk && from_disk->blob ? *from_disk->blob : std::string();
  CheckEq(replaced_blob, "", "a disk store replaces a held blob");

  daemon::UploadQueue reread(journal);
  Check(reread.Enqueue("r", held_v1, false), "the in-flight held value is queued");
  const auto old_held = reread.TakeReady(now, &soonest);
  Check(old_held.has_value(), "the held value is in flight");
  Check(reread.Enqueue("r", nullptr, false), "a disk store during that upload is accepted");
  Check(reread.Held("r") == nullptr, "the disk store drops the held value");
  Check(reread.held_bytes() == held_v1->size(),
        "an in-flight blob stays in the held byte count");
  if (old_held) {
    Check(reread.Finish(*old_held), "finishing the held upload re-queues the disk store");
    const auto disk_again = reread.TakeReady(now, &soonest);
    const std::string reread_blob =
        disk_again && disk_again->blob ? *disk_again->blob : std::string();
    CheckEq(reread_blob, "", "a disk store during upload is re-read from disk");
  }

  daemon::UploadQueue pins("");
  const auto pin_old = std::make_shared<const std::string>("aaaa");
  const auto pin_new = std::make_shared<const std::string>("bbbbbbbb");
  Check(pins.Enqueue("p", pin_old, false), "the pinned store is queued");
  const auto pin_flight = pins.TakeReady(now, &soonest);
  Check(pin_flight.has_value() && pins.Enqueue("p", pin_new, false),
        "a rewrite while uploading is held too");
  Check(pins.held_bytes() == pin_old->size() + pin_new->size(),
        "an in-flight blob stays in the held byte count beside the new one");

  daemon::UploadQueue budget("");
  const auto budget_old = std::make_shared<const std::string>("old");
  const auto budget_new = std::make_shared<const std::string>("new");
  Check(budget.Enqueue("t", budget_old, false), "a budget store is queued");
  auto budget_item = budget.TakeReady(now, &soonest);
  Check(budget_item.has_value(), "the budget upload is in flight");
  if (budget_item) {
    budget_item->attempts = 2;
    Check(budget.Enqueue("t", budget_new, false), "a rewrite during the budget upload is accepted");
    budget.Requeue(*budget_item);
    int tries = 0;
    auto cur = budget.TakeReady(now, &soonest);
    while (cur) {
      ++tries;
      if (++cur->attempts >= 3) break;
      budget.Requeue(*cur);
      cur = budget.TakeReady(now, &soonest);
    }
    CheckEq(std::to_string(tries), "3", "a rewritten upload gets three attempts");
  }

  daemon::UploadQueue accounted("");
  uint64_t queued_n = 0;
  bool merged = false;
  Check(accounted.Enqueue("k", first_blob, false, &merged), "the counted store is queued");
  if (!merged) ++queued_n;
  merged = false;
  Check(accounted.Enqueue("k", second_blob, false, &merged), "the counted rewrite is accepted");
  if (!merged) ++queued_n;
  const uint64_t pending = accounted.size();
  CheckEq(std::to_string(queued_n), std::to_string(pending),
          "a merged re-put is not counted as queued");

  auto accounted_for = [](uint64_t done, const daemon::UploadQueue& q) {
    const uint64_t pending = q.size() + q.in_flight_count() - q.superseded_count();
    return std::to_string(done + pending + q.superseded_count());
  };

  daemon::UploadQueue waiting_id("");
  bool merged_id = false;
  bool flying_id = false;
  uint64_t queued_id = 0;
  uint64_t done_id = 0;
  Check(waiting_id.Enqueue("a", first_blob, false, &merged_id, &flying_id),
        "the identity's first store is queued");
  if (!merged_id && !flying_id) ++queued_id;
  merged_id = false;
  flying_id = false;
  Check(waiting_id.Enqueue("a", second_blob, false, &merged_id, &flying_id),
        "the identity's waiting rewrite is accepted");
  if (!merged_id && !flying_id) ++queued_id;
  CheckEq(std::to_string(queued_id), accounted_for(done_id, waiting_id),
          "a merged waiting re-put keeps queued equal to done, pending and superseded");

  auto waiting_item = waiting_id.TakeReady(now, &soonest);
  Check(waiting_item.has_value(), "the identity store can be taken");
  merged_id = false;
  flying_id = false;
  bool counted_as_merged = false;
  if (waiting_item) {
    Check(waiting_id.Enqueue("a", first_blob, false, &merged_id, &flying_id),
          "an in-flight rewrite is accepted");
    counted_as_merged = merged_id;
    Check(!merged_id && flying_id, "an in-flight rewrite is not a merged waiting store");
    if (!merged_id && !flying_id) ++queued_id;
    bool count_requeue = false;
    const bool requeued = waiting_id.Finish(*waiting_item, &count_requeue);
    if (requeued && count_requeue && !counted_as_merged) ++queued_id;
    ++done_id;
    CheckEq(std::to_string(queued_id), accounted_for(done_id, waiting_id),
            "an in-flight rewrite keeps queued equal to done, pending and superseded");
  }

  daemon::UploadQueue refused_id("", 10);
  uint64_t refused_queued = 0;
  bool refused_merged = false;
  bool refused_flying = false;
  Check(refused_id.Enqueue("z", small, false, &refused_merged, &refused_flying),
        "the refused identity store is queued");
  if (!refused_merged && !refused_flying) ++refused_queued;
  auto refused_item = refused_id.TakeReady(now, &soonest);
  Check(refused_item.has_value(), "the refused identity store is in flight");
  refused_flying = false;
  Check(!refused_id.Enqueue("z", big, false, nullptr, &refused_flying) && refused_flying,
        "the refused identity re-put is in flight");
  CheckEq(std::to_string(refused_queued), accounted_for(0, refused_id),
          "a refused in-flight re-put keeps queued equal to done, pending and superseded");

  daemon::UploadQueue after(journal, 10);
  const auto v1 = std::make_shared<const std::string>("v1");
  const auto v4 = std::make_shared<const std::string>("v4");
  Check(after.Enqueue("k", v1, true), "v1 is queued");
  auto v1_item = after.TakeReady(now, &soonest);
  Check(v1_item.has_value() && after.in_flight("k"), "v1 is in flight");
  Check(!after.Enqueue("k", big, true), "v3 over the cap is refused");
  Check(after.Enqueue("k", v4, true), "v4 under the cap is accepted");
  if (v1_item) {
    Check(after.Finish(*v1_item), "finishing v1 re-queues the store accepted after the refusal");
    auto v4_item = after.TakeReady(now, &soonest);
    const std::string got = v4_item && v4_item->blob ? *v4_item->blob : std::string();
    CheckEq(got, "v4", "the re-queued store is v4");
    Check(after.JournalExists("k"), "the journal stays for the store accepted after the refusal");
  }

  daemon::UploadQueue wake(journal, 10);
  Check(wake.Enqueue("w", v1, false), "the waited store is queued");
  auto wake_item = wake.TakeReady(now, &soonest);
  Check(wake_item.has_value(), "the waited store is in flight");
  uint64_t refusal_id = 0;
  Check(!wake.Enqueue("w", big, false, nullptr, nullptr, &refusal_id),
        "the waited re-put is refused");
  const auto v3 = std::make_shared<const std::string>("v3");
  Check(wake.Enqueue("w", v3, false), "a newer store overtakes the refusal");
  Check(wake.RefusalStillInFlight(refusal_id), "the refusal still waits for v1");
  Check(wake.RefusalOvertaken(refusal_id), "the refused value is overtaken");
  if (wake_item) {
    std::mutex mu;
    std::condition_variable cv;
    bool parked = false;
    bool released = false;
    std::thread waiter([&] {
      std::unique_lock<std::mutex> lock(mu);
      parked = true;
      cv.notify_all();
      const bool woke = cv.wait_for(lock, std::chrono::seconds(2), [&] {
        return !wake.RefusalStillInFlight(refusal_id);
      });
      released = woke && !wake.RefusalStillInFlight(refusal_id);
    });
    {
      std::unique_lock<std::mutex> lock(mu);
      cv.wait(lock, [&] { return parked; });
    }
    Check(!released, "the waiter stays parked while the refused upload is in flight");
    {
      std::lock_guard<std::mutex> lock(mu);
      wake.Finish(*wake_item);
      wake.TakeReady(now, &soonest);
    }
    cv.notify_all();
    waiter.join();
    Check(released, "the waiter is released by finish of v1 even when v3 is in flight");
    Check(wake.in_flight("w"), "v3 stays in flight after v1 finishes");
    Check(wake.TakeRefusal(refusal_id), "taking the refusal reports it was overtaken");
    CheckEq(std::to_string(wake.refusal_count()), "0",
            "a finished refusal leaves no record");
  }

  // Finish of a superseded flight clears generation_, so the next store starts
  // again at 1. That store is a new flight and must not keep the old refusal
  // waiting.
  daemon::UploadQueue reset_flight(journal, 10);
  Check(reset_flight.Enqueue("s", v1, false), "the reset store is queued");
  auto reset_item = reset_flight.TakeReady(now, &soonest);
  uint64_t reset_refusal = 0;
  Check(!reset_flight.Enqueue("s", big, false, nullptr, nullptr, &reset_refusal),
        "the reset re-put is refused");
  if (reset_item) {
    Check(!reset_flight.Finish(*reset_item), "finishing the superseded flight drops it");
  }
  const auto v_next = std::make_shared<const std::string>("vn");
  Check(reset_flight.Enqueue("s", v_next, false), "a store after the drop is accepted");
  auto next_item = reset_flight.TakeReady(now, &soonest);
  Check(next_item.has_value() && reset_flight.in_flight("s"),
        "the later store is in flight");
  Check(!reset_flight.RefusalStillInFlight(reset_refusal),
        "a new flight does not keep the old refusal waiting");

  daemon::UploadQueue sync_upload(journal, 100);
  const uint64_t sync_generation = sync_upload.BeginSync("y");
  Check(sync_generation != 0 && sync_upload.in_flight("y") && sync_upload.held_bytes() == 0,
        "a synchronous upload is in flight and pins nothing");
  const auto sync_later = std::make_shared<const std::string>("later");
  Check(sync_upload.Enqueue("y", sync_later, false),
        "a store during the synchronous upload is accepted");
  daemon::UploadItem sync_done;
  sync_done.key = "y";
  sync_done.generation = sync_generation;
  bool sync_requeue = false;
  Check(sync_upload.Finish(sync_done, &sync_requeue) && sync_requeue,
        "finishing the synchronous upload re-queues the later store");
  auto sync_again = sync_upload.TakeReady(now, &soonest);
  const std::string sync_blob = sync_again && sync_again->blob ? *sync_again->blob : std::string();
  CheckEq(sync_blob, "later", "the re-queued store is the later value");

  daemon::UploadQueue watched(journal, 100);
  const uint64_t watched_generation = watched.BeginSync("q");
  const uint64_t watch_id = watched.WatchFlight("q");
  Check(watch_id != 0 && watched.RefusalStillInFlight(watch_id),
        "a watch follows the flight already in progress");
  Check(watched.BeginSync("q") == 0, "a second synchronous upload waits for that flight");
  Check(watched.Enqueue("q", sync_later, false),
        "a newer store during the watch is accepted");
  Check(watched.RefusalOvertaken(watch_id), "the watch is overtaken by the newer store");
  daemon::UploadItem watched_done;
  watched_done.key = "q";
  watched_done.generation = watched_generation;
  bool watched_requeue = false;
  Check(watched.Finish(watched_done, &watched_requeue) && watched_requeue,
        "finishing the watched flight re-queues the newer store");
  Check(!watched.RefusalStillInFlight(watch_id), "the watch ends when that flight finishes");
}

void TestHeldHitLayer() {
  Section("daemon::held hit");
  CheckEq(std::string(daemon::HeldHitLayerName()), "memory",
          "a held hit replies with layer memory");
}

void TestJobserver() {
  Section("daemon::jobserver");

  CheckEq(daemon::JobserverMakeFlagsLine("/tmp/vcache-jobserver.fifo"),
          "MAKEFLAGS=-j --jobserver-auth=fifo:/tmp/vcache-jobserver.fifo\n",
          "--jobserver-env output format");

  TempCacheDir dir;
  const std::string path = dir.path() + "/jobserver.fifo";
  ::mkfifo(path.c_str(), 0600);
  {
    const int stale = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (stale >= 0) {
      const char extra = 'x';
      ssize_t ignored = ::write(stale, &extra, 1);
      (void)ignored;
      ::close(stale);
    }
  }
  std::string error;
  auto pool = daemon::JobserverPool::Open(path, 3, &error);
  Check(pool.has_value(), "a stale fifo is replaced (" + error + ")");
  if (!pool) return;

  struct stat st {};
  Check(::stat(path.c_str(), &st) == 0 && S_ISFIFO(st.st_mode) && (st.st_mode & 0777) == 0600,
        "the fifo is mode 0600");
  Check(pool->total() == 3 && pool->fifo_bytes() == 2 && pool->free_tokens() == 3 &&
            pool->withdrawn() == 0,
        "the fifo holds slots-1 bytes and reports N jobs free");

  const int reader = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
  char token = 0;
  Check(reader >= 0 && ::read(reader, &token, 1) == 1 && token == '+',
        "a client can take one token");
  Check(pool->free_tokens() == 2, "taking a token leaves N-1");
  const int writer = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
  const char back = '+';
  Check(writer >= 0 && ::write(writer, &back, 1) == 1, "a client can return a token");
  Check(pool->free_tokens() == 3, "returning a token restores N");
  if (reader >= 0) ::close(reader);
  if (writer >= 0) ::close(writer);

  pool.reset();
  Check(::lstat(path.c_str(), &st) != 0, "destroying the pool removes the fifo");

  auto replaced_pool = daemon::JobserverPool::Open(path, 2, &error);
  Check(replaced_pool.has_value(), "opened a pool whose fifo will be replaced (" + error + ")");
  if (replaced_pool) {
    Check(::unlink(path.c_str()) == 0, "the pool fifo can be removed by path");
    Check(::mkfifo(path.c_str(), 0600) == 0, "a new fifo can take the same path");
    struct stat replaced {};
    const bool replaced_ok = ::lstat(path.c_str(), &replaced) == 0 && S_ISFIFO(replaced.st_mode);
    const ino_t replaced_ino = replaced.st_ino;
    replaced_pool.reset();
    struct stat after {};
    Check(replaced_ok && ::lstat(path.c_str(), &after) == 0 && S_ISFIFO(after.st_mode) &&
              after.st_ino == replaced_ino,
          "destroying the pool leaves a replaced fifo in place");
    ::unlink(path.c_str());
  }

  ::setenv("VCACHE_DAEMON_JOBSERVER", "1", 1);
  ::setenv("VCACHE_DAEMON_JOBSERVER_JOBS", "-2", 1);
  const core::Config negative = core::LoadConfig();
  bool warned = false;
  for (const std::string& warning : negative.warnings) {
    if (warning.find("VCACHE_DAEMON_JOBSERVER_JOBS") != std::string::npos) warned = true;
  }
  Check(negative.daemon.jobserver && negative.daemon.jobserver_jobs == 0 && warned,
        "a negative jobserver_jobs warns and uses the online-CPU default");
  ::setenv("VCACHE_DAEMON_JOBSERVER_JOBS", "0", 1);
  const core::Config zero = core::LoadConfig();
  warned = false;
  for (const std::string& warning : zero.warnings) {
    if (warning.find("VCACHE_DAEMON_JOBSERVER_JOBS") != std::string::npos) warned = true;
  }
  Check(zero.daemon.jobserver_jobs == 0 && warned,
        "jobserver_jobs of 0 warns and uses the online-CPU default");

  ::setenv("VCACHE_DAEMON_JOBSERVER_JOBS", "2147483648", 1);
  const core::Config huge_env = core::LoadConfig();
  warned = false;
  for (const std::string& warning : huge_env.warnings) {
    if (warning.find("VCACHE_DAEMON_JOBSERVER_JOBS") != std::string::npos) warned = true;
  }
  Check(huge_env.daemon.jobserver_jobs == 0 && warned,
        "a jobserver_jobs above INT_MAX from the environment warns and uses the online-CPU default");

  ::unsetenv("VCACHE_DAEMON_JOBSERVER_JOBS");
  TempCacheDir toml_dir;
  const std::string toml_path = toml_dir.path() + "/vcache.toml";
  util::WriteFileAtomic(toml_path, "[daemon]\njobserver_jobs = 3000000000\n");
  ::setenv("VCACHE_CONFIG", toml_path.c_str(), 1);
  const core::Config huge_toml = core::LoadConfig();
  warned = false;
  for (const std::string& warning : huge_toml.warnings) {
    if (warning.find("daemon.jobserver_jobs") != std::string::npos) warned = true;
  }
  Check(huge_toml.daemon.jobserver_jobs == 0 && warned,
        "a jobserver_jobs above INT_MAX from toml warns and uses the online-CPU default");
  ::unsetenv("VCACHE_CONFIG");
  ::unsetenv("VCACHE_DAEMON_JOBSERVER");
  ::unsetenv("VCACHE_DAEMON_JOBSERVER_JOBS");
}

void TestElasticJobserver() {
  Section("daemon::elastic jobserver");
  struct PolicyCase {
    int free, withdrawn;
    uint64_t waiting, available_kb;
    int floor, expected;
    const char* name;
  };
  for (const auto& row : std::vector<PolicyCase>{
      {3, 0, 1, 0, 2, -1, "one waiter withdraws one shared token"},
      {2, 1, 9, 0, 2, -1, "withdrawal stops at the configured floor"},
      {1, 2, 9, 0, 2, 0, "the floor prevents another withdrawal"},
      {1, 0, 9, 0, 2, -1, "withdrawal cannot take a token held by a client"},
      {0, 0, 1, 0, 2, 0, "no free shared token means no withdrawal"},
      {-1, 0, 1, 0, 2, 0, "an unknown free count means no withdrawal"},
      {1, 2, 0, 8192, 2, 1, "recovery restores only one token per tick"},
      {1, 2, 0, 4096, 2, 0, "memory equal to the default does not restore"},
      {1, 2, 0, 4095, 2, 0, "low memory retains withdrawn tokens"},
      {3, 0, 0, 8192, 2, 0, "admission with no waiting requests leaves the pool fixed"},
      {3, 1, 9, 0, 0, -2, "a zero policy floor is raised to one implicit slot"},
      {3, 0, 9, 0, 9, 0, "a floor above total is clamped to the total"}}) {
    Check(daemon::JobserverSignedTokenChange(4, row.free, row.withdrawn, row.waiting,
                                           row.available_kb, row.floor, 4096) == row.expected,
          row.name);
  }
  TempCacheDir dir;
  std::string error;
  auto pool = daemon::JobserverPool::Open(dir.path() + "/elastic.fifo", 4, &error);
  Check(pool && pool->Withdraw(2) == 2 && pool->withdrawn() == 2 && pool->fifo_bytes() == 1,
        "the production pool accounts for tokens actually withdrawn");
  if (!pool) return;
  Check(pool->Restore(1) == 1 && pool->withdrawn() == 1 && pool->restored_total() == 1 &&
            pool->fifo_bytes() == 2, "restoring one token updates live and lifetime counters");
  daemon::JobserverPool moved(std::move(*pool));
  Check(moved.withdrawn() == 1 && moved.restored_total() == 1 && pool->withdrawn() == 0,
        "moving a pool preserves withdrawn ownership and counters");
  const int client = ::open(moved.path().c_str(), O_RDWR | O_NONBLOCK);
  char tokens[3]{};
  const int requested = -daemon::JobserverSignedTokenChange(moved.total(), moved.fifo_bytes(),
      moved.withdrawn(), 1, 0, 2, 4096);
  const bool two_taken = client >= 0 && ::read(client, tokens, 2) == 2;
  Check(moved.withdrawn() == 1, "the client race begins with one daemon-owned token");
  struct WithdrawalResult {
    int taken = -1, before = -1, after = -1;
    int fifo_before = -1, restored = -1, fifo_after = -1;
  } observation;
  int result[2];
  const bool piped = ::pipe(result) == 0;
  const pid_t reader = piped ? ::fork() : -1;
  if (reader == 0) {
    ::close(result[0]);
    observation.before = moved.withdrawn();
    observation.taken = moved.Withdraw(requested);
    observation.after = moved.withdrawn();
    observation.fifo_before = moved.fifo_bytes();
    observation.restored = moved.Restore(moved.withdrawn());
    observation.fifo_after = moved.fifo_bytes();
    ::write(result[1], &observation, sizeof(observation));
    ::_exit(0);
  }
  if (piped) ::close(result[1]);
  pollfd ready{piped ? result[0] : -1, POLLIN, 0};
  Check(requested == 1 && two_taken && reader > 0 && ::poll(&ready, 1, 1000) > 0 &&
            ::read(result[0], &observation, sizeof(observation)) == sizeof(observation) &&
            observation.taken == 0,
        "a client taking the last free token makes withdrawal return EAGAIN within one second");
  Check(observation.before == 1 && observation.after == 1 && moved.withdrawn() == 1,
        "a lost withdrawal race retains exactly the previously owned token");
  Check(observation.fifo_before == 0 && observation.restored == 1 &&
            observation.fifo_after == 1 && moved.fifo_bytes() == 1,
        "restoring after a lost race returns only the previously withdrawn fifo byte");
  if (reader > 0) {
    ::kill(reader, SIGKILL);
    ::waitpid(reader, nullptr, 0);
  }
  if (piped) ::close(result[0]);
  if (client >= 0) {
    ::read(client, tokens, sizeof(tokens));
    ::write(client, "++", 2);
    ::close(client);
  }
  auto destination = daemon::JobserverPool::Open(dir.path() + "/assignment.fifo", 4, &error);
  if (destination) {
    destination->Withdraw(1);
    const int previous_fifo = ::open(destination->path().c_str(), O_RDWR | O_NONBLOCK);
    *destination = std::move(moved);
    Check(destination->withdrawn() == 1 && destination->restored_total() == 1 &&
              moved.withdrawn() == 0,
          "move assignment preserves the source's token ownership");
    Check(previous_fifo >= 0 && ::read(previous_fifo, tokens, 3) == 3,
          "move assignment returns the destination's tokens before closing its fifo");
    if (previous_fifo >= 0) ::close(previous_fifo);
    destination->Restore(1);
  } else {
    Check(false, "move assignment fixture pool opens");
  }
  auto ending = daemon::JobserverPool::Open(dir.path() + "/ending.fifo", 4, &error);
  if (!ending) { Check(false, "shutdown fixture pool opens"); return; }
  ending->Withdraw(2);
  const int blocked_fd = ::open(ending->path().c_str(), O_RDONLY);
  int completed[2];
  if (::pipe(completed) != 0) { if (blocked_fd >= 0) ::close(blocked_fd); return; }
  const pid_t blocked = ::fork();
  if (blocked == 0) {
    ::close(completed[0]);
    char token = 0;
    if (::read(blocked_fd, &token, 1) != 1) ::_exit(1);
    ::write(completed[1], "R", 1);
    const bool returned = ::read(blocked_fd, &token, 1) == 1 &&
                          ::read(blocked_fd, &token, 1) == 1;
    ::write(completed[1], returned ? "Y" : "N", 1);
    ::_exit(0);
  }
  ::close(completed[1]);
  pollfd first{completed[0], POLLIN, 0};
  char outcome = 0;
  Check(blocked > 0 && ::poll(&first, 1, 1000) > 0 &&
            ::read(completed[0], &outcome, 1) == 1 && outcome == 'R',
        "shutdown fixture consumes the last free token before blocking");
  ending.reset();
  Check(::poll(&first, 1, 2000) > 0 && ::read(completed[0], &outcome, 1) == 1 && outcome == 'Y',
        "pool destruction returns withdrawn tokens to a blocked reader within two seconds");
  if (blocked > 0) {
    ::kill(blocked, SIGKILL);
    ::waitpid(blocked, nullptr, 0);
  }
  if (blocked_fd >= 0) ::close(blocked_fd);
  ::close(completed[0]);
  std::map<std::string, std::optional<std::string>> saved;
  for (const std::string name : {"VCACHE_CONFIG", "VCACHE_DAEMON_JOBSERVER_MIN_JOBS"}) {
    const char* value = ::getenv(name.c_str());
    saved[name] = value ? std::optional<std::string>(value) : std::nullopt;
    ::unsetenv(name.c_str());
  }
  const std::string config_path = dir.path() + "/min.toml";
  util::WriteFileAtomic(config_path, "[daemon]\njobserver_min_jobs = 3\n");
  ::setenv("VCACHE_CONFIG", config_path.c_str(), 1);
  Check(core::LoadConfig().daemon.jobserver_min_jobs == 3, "the jobserver floor loads from TOML");
  ::setenv("VCACHE_DAEMON_JOBSERVER_MIN_JOBS", "7", 1);
  Check(core::LoadConfig().daemon.jobserver_min_jobs == 7, "the jobserver floor environment overrides TOML");
  core::Config described = core::LoadConfig();
  described.daemon.mode = core::DaemonMode::kOn;
  described.daemon.jobserver = true;
  Check(core::DescribeConfig(described).find("  jobserver min:  7\n") != std::string::npos,
        "show-config displays the requested jobserver floor");
  ::unsetenv("VCACHE_DAEMON_JOBSERVER_MIN_JOBS");
  for (const std::string value : {"0", "-1", "\"bad\"", "1.5", "true"}) {
    util::WriteFileAtomic(config_path, "[daemon]\njobserver_min_jobs = " + value + "\n");
    const auto invalid = core::LoadConfig();
    Check(invalid.daemon.jobserver_min_jobs == 2 && std::any_of(
              invalid.warnings.begin(), invalid.warnings.end(), [](const auto& warning) {
                return warning.find("daemon.jobserver_min_jobs:") != std::string::npos;
              }), "an invalid TOML jobserver floor warns and retains its default: " + value);
  }
  util::WriteFileAtomic(config_path, "[daemon]\njobserver_min_jobs = 3\n");
  for (const std::string value : {"0", "-1", "bad", "18446744073709551616"}) {
    ::setenv("VCACHE_DAEMON_JOBSERVER_MIN_JOBS", value.c_str(), 1);
    const auto invalid = core::LoadConfig();
    Check(invalid.daemon.jobserver_min_jobs == 3 && std::any_of(
              invalid.warnings.begin(), invalid.warnings.end(), [](const auto& warning) {
                return warning.find("VCACHE_DAEMON_JOBSERVER_MIN_JOBS:") != std::string::npos;
              }), "an invalid environment jobserver floor warns and retains TOML: " + value);
  }
  for (const auto& [name, value] : saved) {
    if (value) ::setenv(name.c_str(), value->c_str(), 1); else ::unsetenv(name.c_str());
  }
}

void TestMemoryEstimates() {
  Section("daemon::admission estimates");
  TempCacheDir cache;
  core::Config config;
  config.disk.dir = cache.path();
  const std::string missing_key(64, 'f');
  Check(daemon::MemoryEstimateKb(config, missing_key, false) == 2097152 &&
            daemon::MemoryEstimateKb(config, missing_key, true) == 4194304,
        "missing cost records use separate compile and link defaults");
  config.daemon.mode = core::DaemonMode::kOn;
  config.daemon.admission = true;
  const std::string description = core::DescribeConfig(config);
  Check(description.find("  admission:      on\n") != std::string::npos &&
            description.find("  compile kB:     2097152\n") != std::string::npos &&
            description.find("  link kB:        4194304\n") != std::string::npos,
        "show-config reports client admission and its separate kilobyte defaults");
  const auto roots = MakeRoots({cache.path() + "=project"});
  const std::string source = cache.path() + "/source.cc";
  const std::string key = core::ComputeCostKey("compile", source, "c++", {"-O2"}, roots);
  for (uint64_t rss_kb : {20000, 5000}) {
    util::ProcResult proc;
    proc.max_rss_kb = rss_kb;
    proc.wall_ms = 1000;
    core::RecordCompileCost(cache.path(), "compile", source, "c++", {"-O2"}, roots, proc);
  }
  Check(daemon::MemoryEstimateKb(config, key, false) == 20000,
        "the largest production-written RSS observation supplies the estimate");
  const std::string link_source = cache.path() + "/program";
  util::ProcResult zero;
  zero.wall_ms = 1;
  core::RecordCompileCost(cache.path(), "link", link_source, "", {}, roots, zero);
  const std::string zero_key = core::ComputeCostKey("link", link_source, "", {}, roots);
  Check(daemon::MemoryEstimateKb(config, zero_key, true) == 4194304,
        "a zero-RSS cost record uses the link default");

  std::map<std::string, std::optional<std::string>> saved;
  for (const std::string name : {"VCACHE_CONFIG", "VCACHE_DAEMON_DEFAULT_COMPILE_KB",
                                  "VCACHE_DAEMON_DEFAULT_LINK_KB"}) {
    const char* value = std::getenv(name.c_str());
    saved[name] = value ? std::optional<std::string>(value) : std::nullopt;
    ::unsetenv(name.c_str());
  }
  const std::string path = cache.path() + "/config.toml";
  util::WriteFileAtomic(path, "[daemon]\ndefault_compile_kb = 77\ndefault_link_kb = 88\n");
  ::setenv("VCACHE_CONFIG", path.c_str(), 1);
  Check(core::LoadConfig().daemon.default_compile_kb == 77 &&
            core::LoadConfig().daemon.default_link_kb == 88,
        "the admission default configuration keys load as kilobytes");
  ::setenv("VCACHE_DAEMON_DEFAULT_COMPILE_KB", "99", 1);
  ::setenv("VCACHE_DAEMON_DEFAULT_LINK_KB", "111", 1);
  Check(core::LoadConfig().daemon.default_compile_kb == 99 &&
            core::LoadConfig().daemon.default_link_kb == 111,
        "admission default environment variables override configuration");
  ::setenv("VCACHE_DAEMON_DEFAULT_COMPILE_KB", "0", 1);
  ::setenv("VCACHE_DAEMON_DEFAULT_LINK_KB", "18446744073709551616", 1);
  Check(core::LoadConfig().daemon.default_compile_kb == 77 &&
            core::LoadConfig().daemon.default_link_kb == 88,
        "zero and overflowing defaults leave the configured positive estimates");
  for (const auto& [name, value] : saved) {
    if (value) ::setenv(name.c_str(), value->c_str(), 1);
    else ::unsetenv(name.c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 7 && (std::string(argv[1]) == "--write-lease-cost-fixture" ||
                    std::string(argv[1]) == "--write-admission-cost-fixture")) {
    const bool admission = std::string(argv[1]) == "--write-admission-cost-fixture";
    const std::string cache_dir = argv[2];
    const auto roots = MakeRoots({argv[3]});
    const std::string operation = argv[4];
    util::ProcResult proc;
    proc.wall_ms = admission ? 1000 : std::strtoull(argv[5], nullptr, 10);
    proc.max_rss_kb = admission ? std::strtoull(argv[5], nullptr, 10) : 4096;
    const std::vector<std::string> command(argv + 6, argv + argc);
    std::string source, language;
    std::vector<std::string> key_args;
    if (operation == "compile") {
      const auto parsed = args::Parse(command);
      source = parsed.source;
      language = args::LanguageName(parsed.language);
      key_args = parsed.key_args;
    } else if (operation == "rustc") {
      const auto parsed = args::ParseRustc(command);
      source = parsed.source;
      language = "rust";
      key_args = parsed.key_args;
    } else if (operation == "link") {
      const auto parsed = args::ParseLink(command);
      source = parsed.output;
      key_args = parsed.key_args;
    } else {
      return 2;
    }
    core::RecordCompileCost(cache_dir, operation, source, language, key_args, roots, proc);
    return core::LoadCompileCost(cache_dir, core::ComputeCostKey(
        operation, source, language, key_args, roots)).observations.empty() ? 1 : 0;
  }
  TestStringUtils();
  TestRootMap();
  TestDepFile();
  TestDepFileEnvDeps();
  TestPreprocessedNormalization();
  TestBlob();
  TestCacheChain();
  TestCacheChainRemote();
  TestDaemonProtocol();
  TestCompileSessions();
  TestCompileSessionShutdownRace();
  TestCompileSessionStalledSend();
  TestKeyLeases();
  TestKeyLeaseValidation();
  TestKeyLeasePutClose();
  TestMemoryAdmission();
  TestRunSpawnCallback();
  TestLeaseReviewFixes();
  TestLeaseOrphanPut();
  TestHasher();
  TestSha256();
  TestLinkArgs();
  TestCompilerArgs();
  TestClangArgs();
  TestDepScanKeyArgs();
  TestRustcArgs();
  TestSigV4();
  TestS3ResponseParsing();
  TestWrittenFileMode();
  TestWriteErrnoIsPreserved();
  TestWriteCloseFailure();
  TestCloneFile();
  // Must run after TestWrittenFileMode. util::DefaultFileMode() caches the
  // umask in a function-local static on first use, so that test forks before
  // this process has written anything, to get a child that initialises its own
  // copy under umask(077). Anything calling WriteFileAtomic earlier -- as the
  // disk cache does on every Put -- primes the cache in the parent, the child
  // inherits it, and the strict-umask assertion fails.
  TestDiskStorageEviction();
  TestDiskTrimPins();
  // Also after TestWrittenFileMode, and for the same reason: it writes
  // files, which primes util::DefaultFileMode()'s cached umask.
  TestLinkTraceClassification();
  TestStats();
  // Writes files too.
  TestRustManifest();
  TestManifestMerge();
  TestManifestMediaCount();
  TestRunRusage();
  // After the rusage check. ReadFile's 64 MiB fixture stays in the allocator,
  // and a forked child is charged that high-water mark until exec replaces it.
  TestScratchDirs();
  TestReadFile();
  TestCost();
  TestMemoryEstimates();
  TestRustcFingerprint();
  TestRustOutputNames();
  TestUploadGeneration();
  TestHeldHitLayer();
  TestJobserver();
  TestElasticJobserver();
  TestAdmissionProcessTree();
  TestAdmissionReview();
  TestManifestPause();
  TestAdmissionBoundsAndTreePeaks();

  std::printf("\n\033[1munit: %d passed, %d failed\033[0m\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
