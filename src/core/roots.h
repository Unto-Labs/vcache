// Path canonicalisation -- the core of vcache's answer to the preprocessor
// problem.
//
// gcc and clang bake absolute paths into four places that all defeat naive
// cross-directory caching:
//   1. the -fworking-directory linemarker in preprocessed output (implied by -g)
//   2. `# <line> "<path>"` linemarkers for the source and every include
//   3. __FILE__ / __BASE_FILE__ expansions
//   4. DW_AT_comp_dir and DW_AT_name in the object file's debug info
//
// vcache handles all four by rewriting the compiler command line: each
// configured root is mapped to a stable canonical prefix via -ffile-prefix-map,
// which covers (2), (3) and (4), and -fno-working-directory covers (1) during
// hashing. The result is that the same source compiled from /home/a/proj and
// /work/b/proj produces a byte-identical object file, so one cache entry serves
// both.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace vcache::core {

// A single source-tree root and the canonical prefix it is rewritten to.
struct Root {
  std::string path;       // absolute, lexically normalised, no trailing slash
  std::string canonical;  // absolute canonical prefix, e.g. "/vcache/proj"
};

enum class PrefixMapStyle {
  kC,     // -ffile-prefix-map=old=new     (gcc/clang)
  kRust,  // --remap-path-prefix=old=new   (rustc)
};

// How to treat prefix-map flags that the build system passed in to vcache.
//
// The default is kError. A caller-supplied prefix map and vcache's own would
// silently interact under gcc's last-match-wins rule, so the safe response to
// an unexpected one is to stop and make the operator choose, rather than to
// quietly change the mapping their build asked for.
enum class IncomingMapPolicy {
  kError,  // refuse to run (default)
  kStrip,  // remove them; vcache owns path rewriting
  kKeep,   // pass through -- disables caching, since results become ambiguous
};

// Parses a policy name. Returns false if `name` is not one of the three.
bool ParseIncomingMapPolicy(std::string_view name, IncomingMapPolicy* out);

const char* IncomingMapPolicyName(IncomingMapPolicy policy);

class RootMap {
 public:
  RootMap() = default;

  // Builds a map from specs of the form "PATH" or "PATH=TARGET".
  //   "PATH"          -> canonical prefix "/vcache/<basename>"
  //   "PATH=name"     -> canonical prefix "/vcache/name"
  //   "PATH=/abs/dir" -> canonical prefix "/abs/dir" verbatim
  // Relative PATHs are resolved against the current directory. Duplicate
  // canonical names are disambiguated by appending an index. Malformed specs
  // are appended to `errors` and skipped.
  static RootMap FromSpecs(const std::vector<std::string>& specs,
                           std::vector<std::string>* errors);

  // Adds `dir` as a root named `name` unless it is already covered by an
  // existing root. Returns true if it was added. Used to bring the build
  // directory under canonicalisation, since DW_AT_comp_dir records the cwd.
  bool AddIfUncovered(const std::string& dir, const std::string& name);

  bool empty() const { return roots_.empty(); }
  const std::vector<Root>& roots() const { return roots_; }

  // Returns the flags to append to the compiler command line.
  //
  // Ordering matters: gcc resolves overlapping maps last-match-wins, so the
  // most general root must come first and the most specific last. The returned
  // vector is already ordered that way.
  std::vector<std::string> PrefixMapArgs(PrefixMapStyle style) const;

  // Applies the mapping to `path` in-process, for hashing normalised argv.
  // Uses the same most-specific-wins rule as the emitted flags.
  std::string Canonicalize(std::string_view path) const;

  // Inverse of Canonicalize: rewrites a canonical prefix back to the local
  // root. Needed when replaying cached dependency files, whose paths must name
  // files that exist on this machine.
  std::string Localize(std::string_view path) const;

  // True if `path` lies inside any root.
  bool Covers(std::string_view path) const;

  // Substring rewrite over free-form text, used for compiler diagnostics.
  // gcc reports warnings using the path as spelled on the command line rather
  // than the remapped one, so a cached stderr replayed in another directory
  // would otherwise cite paths that do not exist there.
  std::string CanonicalizeText(const std::string& text) const;
  std::string LocalizeText(const std::string& text) const;

  // Machine-independent form of the mapping, mixed into the cache key.
  //
  // Only the canonical targets appear here, never the local root paths. Those
  // paths are precisely what vcache exists to abstract away: including them
  // would make /home/a/proj and /work/b/proj derive different keys even though
  // they canonicalise to the same thing and produce identical objects.
  std::string Fingerprint() const;

  // Full mapping including local paths, for --show-roots and entry metadata.
  std::string DebugString() const;

 private:
  // Sorted most-general (shortest path) first.
  std::vector<Root> roots_;

  void SortRoots();
};

// True if `arg` is a prefix-mapping flag vcache must take over:
// -ffile-prefix-map, -fdebug-prefix-map, -fmacro-prefix-map, -fprofile-prefix-map,
// or rustc's --remap-path-prefix.
bool IsPrefixMapFlag(std::string_view arg);

}  // namespace vcache::core
