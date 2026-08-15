// Normalisation of preprocessed output before hashing.
//
// -ffile-prefix-map covers __FILE__ expansions and the paths recorded in debug
// info, but gcc does *not* apply it to the `# <line> "<file>"` linemarkers it
// writes in -E output. Those linemarkers therefore still carry the absolute
// path of the source and of every include, and hashing the raw text would miss
// on every directory change even though the resulting object file is identical.
//
// Dropping linemarkers entirely (-P) is not an option: they determine the line
// table in debug info, so two inputs that differ only in linemarkers really can
// produce different objects.
//
// The fix is to rewrite just the path inside each linemarker through the root
// mapping, then hash the result.
#pragma once

#include <string>

#include "core/roots.h"
#include "hash/hasher.h"

namespace vcache::core {

// Streams `path`, rewriting linemarker paths through `roots`, and feeds the
// result into `hasher`. Returns false if the file could not be read.
//
// Hand-written rather than grammar-driven: this runs over the full preprocessed
// text of every compilation, which is routinely tens of megabytes, and all it
// needs is to recognise lines beginning with '#'.
bool HashNormalizedPreprocessedOutput(const std::string& path,
                                      const RootMap& roots,
                                      hash::Hasher* hasher);

// Rewrites a single line, returning it unchanged when it is not a linemarker.
// Exposed for testing.
std::string NormalizeLinemarker(const std::string& line, const RootMap& roots);

}  // namespace vcache::core
