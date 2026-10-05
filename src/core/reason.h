// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// Why vcache ran the real compiler instead of serving or storing an entry.
//
// Every such decision names one Reason. Its table entry supplies both the text
// logged for the decision and the key of its line in the stats file, so the log
// and `--show-stats` cannot describe the same decision differently.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

namespace vcache::core {

// The counter a reason is reported under. Passthrough has no positional
// counter of its own; --show-stats shows the sum of its reasons.
enum class Outcome : uint8_t {
  kUncacheable,
  kPreprocessFailed,
  kPassthrough,
};

constexpr std::string_view OutcomeName(Outcome outcome) {
  switch (outcome) {
    case Outcome::kUncacheable: return "uncacheable";
    case Outcome::kPreprocessFailed: return "preprocess failed";
    case Outcome::kPassthrough: return "passthrough";
  }
  return "";
}

enum class Reason : uint8_t {
  // Command-line shapes, shared by the compiler, link and rustc parsers.
  kEmptyCommandLine,
  kBadResponseFile,
  kSourceFromStdin,
  kNoInputFile,
  kMultipleInputs,
  kOutputNotAFile,
  // Compiler command lines.
  kUnsupportedFlag,
  kSecondOutputFile,
  kPreprocessOnly,
  kNotCompileOnly,
  kUnknownLanguage,
  kPlainAssembly,
  kDepfileNotAFile,
  // The C/C++ pipeline.
  kDepScanPolicy,
  kNativeTargetPolicy,
  kNativeTargetUnresolved,
  kKeptPrefixMaps,
  kIncbin,
  // Link command lines and the link pipeline.
  kMissingFlagValue,
  kBuildIdUuid,
  kCompileAndLink,
  kNoLinkOutput,
  kNoTracer,
  kLinkWithoutDisk,
  kInheritedPreload,
  kIncompleteTrace,
  kUntracedLinker,
  // rustc command lines.
  kNoOutDir,
  kExplicitOutput,
  kNoEmit,
  kEmitWithPath,
  // Preprocessing, or rustc's dependency pass, did not succeed.
  kPreprocessorError,
  kNoRustDepInfo,
  // Local failures that fall back to the real compiler.
  kNoTempDir,
  kNoCacheKey,
  kUnreadableSource,
  kNoDepScanOutput,
  kDepfileUnwritable,
  kOutputUnplaceable,
  kCaptureFailed,
  kOutDirUnwritable,
  kCount,  // sentinel
};

struct ReasonInfo {
  Reason reason;
  Outcome outcome;
  std::string_view name;
};

// Names are persisted in the stats file: renaming one orphans its count.
inline constexpr ReasonInfo kReasons[] = {
    {Reason::kEmptyCommandLine, Outcome::kUncacheable, "empty command line"},
    {Reason::kBadResponseFile, Outcome::kUncacheable, "bad response file"},
    {Reason::kSourceFromStdin, Outcome::kUncacheable, "source from stdin"},
    {Reason::kNoInputFile, Outcome::kUncacheable, "no input file"},
    {Reason::kMultipleInputs, Outcome::kUncacheable, "multiple inputs"},
    {Reason::kOutputNotAFile, Outcome::kUncacheable, "output not a file"},
    {Reason::kUnsupportedFlag, Outcome::kUncacheable, "unsupported flag"},
    {Reason::kSecondOutputFile, Outcome::kUncacheable, "second output file"},
    {Reason::kPreprocessOnly, Outcome::kUncacheable, "preprocess only"},
    {Reason::kNotCompileOnly, Outcome::kUncacheable, "link"},
    {Reason::kUnknownLanguage, Outcome::kUncacheable, "unknown language"},
    {Reason::kPlainAssembly, Outcome::kUncacheable, "plain assembly"},
    {Reason::kDepfileNotAFile, Outcome::kUncacheable, "depfile not a file"},
    {Reason::kDepScanPolicy, Outcome::kUncacheable, "dep scan policy"},
    {Reason::kNativeTargetPolicy, Outcome::kUncacheable, "native policy"},
    {Reason::kNativeTargetUnresolved, Outcome::kUncacheable, "native unresolved"},
    {Reason::kKeptPrefixMaps, Outcome::kUncacheable, "kept prefix maps"},
    {Reason::kIncbin, Outcome::kUncacheable, ".incbin"},
    {Reason::kMissingFlagValue, Outcome::kUncacheable, "missing flag value"},
    {Reason::kBuildIdUuid, Outcome::kUncacheable, "build-id=uuid"},
    {Reason::kCompileAndLink, Outcome::kUncacheable, "compile and link"},
    {Reason::kNoLinkOutput, Outcome::kUncacheable, "no -o"},
    {Reason::kNoTracer, Outcome::kUncacheable, "no tracer"},
    {Reason::kLinkWithoutDisk, Outcome::kUncacheable, "no disk cache"},
    {Reason::kInheritedPreload, Outcome::kUncacheable, "LD_PRELOAD set"},
    {Reason::kIncompleteTrace, Outcome::kUncacheable, "incomplete trace"},
    {Reason::kUntracedLinker, Outcome::kUncacheable, "untraced linker"},
    {Reason::kNoOutDir, Outcome::kUncacheable, "no --out-dir"},
    {Reason::kExplicitOutput, Outcome::kUncacheable, "explicit -o"},
    {Reason::kNoEmit, Outcome::kUncacheable, "no --emit"},
    {Reason::kEmitWithPath, Outcome::kUncacheable, "--emit with path"},
    {Reason::kPreprocessorError, Outcome::kPreprocessFailed, "preprocessor error"},
    {Reason::kNoRustDepInfo, Outcome::kPreprocessFailed, "no rust dep-info"},
    {Reason::kNoTempDir, Outcome::kPassthrough, "no temp dir"},
    {Reason::kNoCacheKey, Outcome::kPassthrough, "no cache key"},
    {Reason::kUnreadableSource, Outcome::kPassthrough, "unreadable source"},
    {Reason::kNoDepScanOutput, Outcome::kPassthrough, "no dep scan output"},
    {Reason::kDepfileUnwritable, Outcome::kPassthrough, "depfile unwritable"},
    {Reason::kOutputUnplaceable, Outcome::kPassthrough, "output unplaceable"},
    {Reason::kCaptureFailed, Outcome::kPassthrough, "capture failed"},
    {Reason::kOutDirUnwritable, Outcome::kPassthrough, "out-dir unwritable"},
};

// A name is a stats-file field and a --show-stats label: unique, free of the
// separators, and short enough to keep the count column aligned.
constexpr size_t kMaxReasonNameLength = 19;

constexpr bool ReasonTableIsWellFormed() {
  if (std::size(kReasons) != static_cast<size_t>(Reason::kCount)) return false;
  for (size_t i = 0; i < std::size(kReasons); ++i) {
    const std::string_view name = kReasons[i].name;
    if (kReasons[i].reason != static_cast<Reason>(i)) return false;
    if (name.empty() || name.size() > kMaxReasonNameLength) return false;
    if (name.find_first_of("\t\n") != std::string_view::npos) return false;
    for (size_t j = 0; j < i; ++j) {
      if (kReasons[j].name == name) return false;
    }
  }
  return true;
}
static_assert(ReasonTableIsWellFormed(),
              "reason table out of order with the Reason enum, or a name is "
              "duplicated, too long, or contains a tab or newline");

constexpr const ReasonInfo& GetReasonInfo(Reason reason) {
  return kReasons[static_cast<size_t>(reason)];
}

// One decision: its reason, plus what made this invocation hit it (a flag, a
// path) for the log line. The detail is never counted.
struct Decision {
  Reason reason;
  std::string detail;

  // Implicit, so a parser can assign a bare Reason.
  Decision(Reason reason_in, std::string detail_in = {})
      : reason(reason_in), detail(std::move(detail_in)) {}

  std::string Describe() const {
    std::string text(GetReasonInfo(reason).name);
    if (!detail.empty()) text += ": " + detail;
    return text;
  }
};

}  // namespace vcache::core
