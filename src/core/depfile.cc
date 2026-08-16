// SPDX-License-Identifier: GPL-3.0-only
#include "core/depfile.h"

#include <boost/fusion/include/adapt_struct.hpp>
#include <boost/spirit/home/x3.hpp>

#include <string>
#include <vector>

#include "core/roots.h"

// Spirit fills DepRule directly, which requires the struct to be fusion-adapted
// before any rule declares it as an attribute type.
BOOST_FUSION_ADAPT_STRUCT(vcache::core::DepRule, targets, prerequisites)

namespace vcache::core {
namespace {

namespace x3 = boost::spirit::x3;

// A backslash immediately before a newline is a line continuation and counts as
// whitespace; blanks separate tokens. Newlines are *not* skipped, because they
// terminate a rule.
const auto skipper = x3::blank | (x3::lit('\\') >> x3::eol);

// One character of a path token. gcc escapes spaces, tabs, hashes and colons
// with a backslash, and writes a literal '$' as "$$".
const auto path_char =
    (x3::lit('\\') >> x3::char_(" \t\\#:")) |
    (x3::lit("$$") >> x3::attr('$')) |
    (x3::char_ - x3::space - x3::char_(":\\"));

// lexeme: no skipping inside a token, otherwise an escaped space would split.
const auto path_token = x3::rule<struct path_tag, std::string>{"path"} =
    x3::lexeme[+path_char];

const auto rule_targets = x3::rule<struct targets_tag, std::vector<std::string>>{
    "targets"} = +path_token;
const auto rule_prereqs = x3::rule<struct prereqs_tag, std::vector<std::string>>{
    "prerequisites"} = *path_token;

const auto dep_rule = x3::rule<struct rule_tag, DepRule>{"rule"} =
    rule_targets >> ':' >> rule_prereqs;

// Rules are separated by one or more newlines; leading and trailing blank lines
// are tolerated.
const auto dep_file = x3::rule<struct file_tag, std::vector<DepRule>>{"depfile"} =
    x3::omit[*x3::eol] >> -(dep_rule % +x3::eol) >> x3::omit[*x3::eol];

// Applies gcc's escaping when writing a token back out.
std::string Escape(const std::string& path) {
  std::string out;
  out.reserve(path.size() + 8);
  for (char c : path) {
    switch (c) {
      case ' ':
      case '\t':
      case '#':
      case '\\':
        out.push_back('\\');
        out.push_back(c);
        break;
      case '$':
        out.append("$$");
        break;
      default:
        out.push_back(c);
    }
  }
  return out;
}

}  // namespace

std::optional<DepFile> ParseDepFile(const std::string& text) {
  DepFile out;
  auto begin = text.begin();
  const auto end = text.end();
  const bool ok =
      x3::phrase_parse(begin, end, dep_file, skipper, out.rules);
  if (!ok || begin != end) return std::nullopt;
  return out;
}

std::string RenderDepFile(const DepFile& dep) {
  std::string out;
  for (const DepRule& rule : dep.rules) {
    if (rule.targets.empty()) continue;
    for (size_t i = 0; i < rule.targets.size(); ++i) {
      if (i != 0) out.push_back(' ');
      out.append(Escape(rule.targets[i]));
    }
    out.push_back(':');
    // One line per rule, with no backslash-newline continuations.
    //
    // gcc's own layout wraps after every prerequisite, and make accepts either.
    // cargo does not: its dep-info reader takes the whole line, splits on
    // whitespace, and treats a token-final backslash as an escaped space inside
    // a filename rather than as a line continuation. A wrapped file therefore
    // fails with "malformed dep-info format, trailing \" and the build stops.
    // Since make is happy with long lines, the unwrapped form is the one that
    // works for both readers.
    for (const std::string& prereq : rule.prerequisites) {
      out.push_back(' ');
      out.append(Escape(prereq));
    }
    out.push_back('\n');
  }
  return out;
}

void RemapDepFile(DepFile* dep, const RootMap& roots, MapDirection direction) {
  auto map_one = [&](std::string& path) {
    path = (direction == MapDirection::kCanonicalize) ? roots.Canonicalize(path)
                                                      : roots.Localize(path);
  };
  for (DepRule& rule : dep->rules) {
    for (std::string& t : rule.targets) map_one(t);
    for (std::string& p : rule.prerequisites) map_one(p);
  }
}

}  // namespace vcache::core
