#!/usr/bin/env bash
# Checks that every source calling a <cctype> function includes <cctype>.
#
# libstdc++ pulls <cctype> in through <string>, so a missing include builds
# here and fails only on a standard library that does not, which no toolchain
# on the build hosts has. The compiler cannot catch it, so this does.
#
# Usage: tests/cctype_include_check.sh
set -euo pipefail

TOP=$(cd "$(dirname "$0")/.." && pwd)
calls='std::(is(alnum|alpha|blank|cntrl|digit|graph|lower|print|punct|space|upper|xdigit)|to(lower|upper))\('
missing=0
while IFS= read -r file; do
  if ! grep -q '^#include <cctype>' "$file"; then
    echo "cctype include check: ${file#"$TOP"/} calls a <cctype> function without including it" >&2
    missing=1
  fi
done < <(grep -rlE "$calls" "$TOP/src" --include='*.cc' --include='*.h')
if [[ $missing -ne 0 ]]; then exit 1; fi
echo "cctype include check: ok"
