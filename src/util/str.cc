// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: GPL-3.0-only
#include "util/str.h"

#include <cctype>
#include <cstdlib>

namespace vcache::util {

std::vector<std::string> Split(std::string_view s, char delim, bool skip_empty) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    size_t pos = s.find(delim, start);
    std::string_view field =
        s.substr(start, pos == std::string_view::npos ? pos : pos - start);
    if (!skip_empty || !field.empty()) out.emplace_back(field);
    if (pos == std::string_view::npos) break;
    start = pos + 1;
  }
  return out;
}

std::string Join(const std::vector<std::string>& parts, std::string_view sep) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) out.append(sep);
    out.append(parts[i]);
  }
  return out;
}

bool StartsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string TrimWhitespace(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return std::string(s.substr(b, e - b));
}

std::string AsciiLower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string HexEncode(const unsigned char* bytes, size_t len) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.resize(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out[2 * i] = kDigits[bytes[i] >> 4];
    out[2 * i + 1] = kDigits[bytes[i] & 0x0f];
  }
  return out;
}

std::string ExpandTilde(std::string_view path) {
  if (!StartsWith(path, "~/")) return std::string(path);
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') return std::string(path);
  std::string out(home);
  out.append(path.substr(1));  // keeps the '/'
  return out;
}

bool ParseSize(std::string_view s, uint64_t* out) {
  std::string t = TrimWhitespace(s);
  if (t.empty()) return false;
  size_t idx = 0;
  uint64_t value = 0;
  while (idx < t.size() && std::isdigit(static_cast<unsigned char>(t[idx]))) {
    uint64_t digit = static_cast<uint64_t>(t[idx] - '0');
    // Guard against overflow on absurd inputs rather than wrapping silently.
    if (value > (UINT64_MAX - digit) / 10) return false;
    value = value * 10 + digit;
    ++idx;
  }
  if (idx == 0) return false;

  uint64_t multiplier = 1;
  std::string suffix = AsciiLower(t.substr(idx));
  if (suffix.empty() || suffix == "b") {
    multiplier = 1;
  } else if (suffix == "k" || suffix == "kb" || suffix == "ki") {
    multiplier = 1ull << 10;
  } else if (suffix == "m" || suffix == "mb" || suffix == "mi") {
    multiplier = 1ull << 20;
  } else if (suffix == "g" || suffix == "gb" || suffix == "gi") {
    multiplier = 1ull << 30;
  } else if (suffix == "t" || suffix == "tb" || suffix == "ti") {
    multiplier = 1ull << 40;
  } else {
    return false;
  }
  if (value > UINT64_MAX / multiplier) return false;
  *out = value * multiplier;
  return true;
}

}  // namespace vcache::util
