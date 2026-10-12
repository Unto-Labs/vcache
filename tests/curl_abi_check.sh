#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Unto Labs
# SPDX-License-Identifier: Apache-2.0
#
# Checks the curl.h subset in third-party/curl against a real curl.h.
#
# The subset only takes effect on hosts without curl's headers, and every CI
# workflow installs them, so a wrong value in it would otherwise surface only as
# broken S3 requests on some other machine. Each constant, type size and
# curl_slist offset the subset declares is printed from the subset as a
# static_assert, and those asserts are then compiled against the real header.
#
# Usage: CXX=<compiler> CURL_CFLAGS=<flags locating curl.h> tests/curl_abi_check.sh
# Skips when the compiler finds no real curl.h.
set -euo pipefail

TOP=$(cd "$(dirname "$0")/.." && pwd)
CXX=${CXX:-c++}
read -r -a curl_cflags <<<"${CURL_CFLAGS:-}"
subset_include=$TOP/third-party/curl/include

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# curlver.h defines LIBCURL_VERSION and the subset does not, so this also
# refuses to compare the subset with itself.
cat >"$work/real.cc" <<'EOF'
#include <curl/curl.h>
#ifndef LIBCURL_VERSION
#error not a real curl.h
#endif
EOF
if ! "$CXX" -std=c++20 "${curl_cflags[@]}" -fsyntax-only "$work/real.cc" 2>/dev/null; then
  echo "curl ABI check: skipped (no real curl/curl.h)"
  exit 0
fi

mapfile -t names < <(grep -oE '\b(CURLE|CURLOPT|CURLOPTTYPE|CURLINFO|CURL_GLOBAL)_[A-Z0-9_]+' \
  "$subset_include/curl/curl.h" | sort -u)
if [[ ${#names[@]} -eq 0 ]]; then
  echo "curl ABI check: FAIL (no constants found in the subset)"
  exit 1
fi

{
  echo '#include <cstddef>'
  echo '#include <cstdio>'
  echo '#include <curl/curl.h>'
  echo 'static void Emit(const char* expr, long long value) {'
  echo '  std::printf("static_assert(static_cast<long long>(%s) == %lldLL, \"%s\");\n",'
  echo '              expr, value, expr);'
  echo '}'
  echo 'int main() {'
  for name in "${names[@]}"; do
    echo "  Emit(\"$name\", static_cast<long long>($name));"
  done
  for expr in 'sizeof(curl_off_t)' 'sizeof(CURLcode)' 'sizeof(CURLoption)' 'sizeof(CURLINFO)' \
              'sizeof(curl_slist)' 'offsetof(curl_slist, data)' 'offsetof(curl_slist, next)'; do
    echo "  Emit(\"$expr\", static_cast<long long>($expr));"
  done
  echo '}'
} >"$work/subset_values.cc"

"$CXX" -std=c++20 -I"$subset_include" -o "$work/subset_values" "$work/subset_values.cc"
"$work/subset_values" >"$work/subset_asserts.h"

cat >"$work/check.cc" <<'EOF'
#include <cstddef>
#include <curl/curl.h>
#include "subset_asserts.h"
EOF
if "$CXX" -std=c++20 "${curl_cflags[@]}" -I"$work" -fsyntax-only "$work/check.cc" \
    2>"$work/errors"; then
  echo "curl ABI check: ${#names[@]} constants and the type layout match the real curl.h"
else
  echo "curl ABI check: FAIL (the subset in third-party/curl differs from the real curl.h)"
  grep -E 'static assertion|static_assert' "$work/errors" || cat "$work/errors"
  exit 1
fi
