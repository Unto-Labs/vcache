// SPDX-License-Identifier: GPL-3.0-only
// Unit tests for vcache. Deliberately dependency-free: a tiny harness keeps the
// build to plain make, as the plan asks.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "args/compiler_args.h"
#include "args/rustc_args.h"
#include "core/config.h"
#include "core/depfile.h"
#include "core/preprocessed.h"
#include "core/roots.h"
#include "hash/hasher.h"
#include "hash/sha256.h"
#include "storage/s3_storage.h"
#include "storage/storage.h"
#include "util/str.h"

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
      "PUT", "/vcache/ab/cdef", "bkt.s3.us-east-1.amazonaws.com",
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
      "GET", "/k", "h", "20150830T123600Z", "20150830", Sha256Hex(""), "TOKEN",
      "us-east-1", "AKIDEXAMPLE", secret);
  Check(with_token.find("x-amz-security-token") != std::string::npos,
        "session token is included in SignedHeaders");
}

}  // namespace

int main() {
  TestStringUtils();
  TestRootMap();
  TestDepFile();
  TestPreprocessedNormalization();
  TestBlob();
  TestHasher();
  TestSha256();
  TestCompilerArgs();
  TestRustcArgs();
  TestSigV4();

  std::printf("\n\033[1munit: %d passed, %d failed\033[0m\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
