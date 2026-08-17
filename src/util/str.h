// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// Small string helpers shared across vcache.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vcache::util {

// Splits `s` on every occurrence of `delim`. Empty fields are preserved unless
// `skip_empty` is set, which is what the env-var list parsers want.
std::vector<std::string> Split(std::string_view s, char delim,
                               bool skip_empty = false);

std::string Join(const std::vector<std::string>& parts, std::string_view sep);

bool StartsWith(std::string_view s, std::string_view prefix);
bool EndsWith(std::string_view s, std::string_view suffix);

std::string TrimWhitespace(std::string_view s);

// Lowercases ASCII only; used for header names and config keys, never paths.
std::string AsciiLower(std::string_view s);

// Hex-encodes `bytes` using lowercase digits.
std::string HexEncode(const unsigned char* bytes, size_t len);

// Expands a leading `~/` using $HOME. Any other input is returned unchanged.
std::string ExpandTilde(std::string_view path);

// Parses sizes such as "10G", "512M", "1024". Returns false on malformed input.
bool ParseSize(std::string_view s, uint64_t* out);

}  // namespace vcache::util
