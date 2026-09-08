// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
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
#include <string_view>

#include "core/roots.h"
#include "hash/hasher.h"

namespace vcache::core {

// Streams `path`, rewriting linemarker paths through `roots`, and feeds the
// result into `hasher`. Returns false if the file could not be read.
//
// Hand-written rather than grammar-driven: this runs over the full preprocessed
// text of every compilation, which is routinely tens of megabytes, and all it
// needs is to recognise lines beginning with '#'.
//
// `saw_incbin`, when non-null, is set if the text contains an `.incbin`
// assembler directive. That directive names a file the *assembler* opens, long
// after preprocessing, so its contents never reach this text -- see
// ContainsIncbin.
bool HashNormalizedPreprocessedOutput(const std::string& path,
                                      const RootMap& roots,
                                      hash::Hasher* hasher,
                                      bool* saw_incbin = nullptr);

// True if `line` contains an `.incbin` directive.
//
// The preprocessed text is vcache's stand-in for everything the compiler will
// read. `.incbin` breaks that: it tells the assembler to splice a file in
// verbatim, and neither the file's name nor its contents are visible to the
// preprocessor, so two compilations whose preprocessed text is identical can
// legitimately produce different objects. The Linux kernel does exactly this
// -- kernel/kheaders.c embeds a tar of the tree's headers, kernel/configs.c the
// build's own config -- and a cache that ignores it serves an object holding
// some other build's payload, with nothing to show that anything went wrong.
//
// Exposed for testing.
bool ContainsIncbin(std::string_view line);

// Rewrites a single line, returning it unchanged when it is not a linemarker.
// Exposed for testing.
std::string NormalizeLinemarker(const std::string& line, const RootMap& roots);

}  // namespace vcache::core
