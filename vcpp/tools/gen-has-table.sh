#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the table vcpp answers __has_attribute / __has_builtin from.
#
# These operators are answered by gcc out of tables that live in the front end,
# not in libcpp, so vcpp cannot compute them and will not guess: absent an
# entry it declines and vcache preprocesses with the real compiler. This script
# asks the target compiler directly -- __has_attribute(x) expands to its value,
# so preprocessing a file of queries yields exact answers, not approximations.
#
#   ./gen-has-table.sh [--cc gcc] [-o table] [--no-system] FILE_OR_DIR...
#   ./gen-has-table.sh -o table --learn -- vcpp --has-table=table ... file.c
#
# Names are harvested from the sources given AND from the target compiler's own
# system headers, which is where most of the queries actually come from: glibc
# asks __has_attribute(__nothrow__) in nearly every translation unit, so a table
# built from project sources alone declines everything.
#
# Harvesting only finds names spelled literally, and that is not enough on its
# own: glibc routes every query through
#
#     #define __glibc_has_attribute(attr) __has_attribute (attr)
#
# so the name libcpp finally sees arrives by macro expansion and never appears
# next to "__has_attribute" in any header. --learn closes that gap the way
# vcache does in production: run the preprocessor, take the name it declined
# on, answer it, repeat. The set converges in a handful of rounds because a
# given toolchain asks about very few distinct names.

set -euo pipefail

CC="${VCPP_CC:-gcc}"
OUT="-"
SYSTEM=1
LEARN=0
srcs=()
cmd=()

while (( $# )); do
  case "$1" in
    --cc)        CC="$2"; shift 2 ;;
    -o)          OUT="$2"; shift 2 ;;
    --no-system) SYSTEM=0; shift ;;
    --learn)     LEARN=1; shift ;;
    --)          shift; cmd=("$@"); break ;;
    *)           srcs+=("$1"); shift ;;
  esac
done

# Ask the compiler what __has_KIND(NAME) expands to, for every name on stdin.
# Exact by construction: the operator evaluates to its value, so preprocessing
# a file of queries reads the answers straight out of the compiler.
answer_names() {
  local kind="$1" names="$2" out="$3"
  [[ -s "$names" ]] || return 0
  : > "$out.q"
  while read -r name; do
    printf 'V __has_%s(%s)\n' "$kind" "$name" >> "$out.q"
  done < "$names"
  "$CC" -E -P -xc "$out.q" 2>/dev/null | grep '^V ' | sed 's/^V //' > "$out.a"
  if [[ "$(wc -l < "$out.a")" != "$(wc -l < "$names")" ]]; then
    echo "gen-has-table: $CC did not answer every __has_$kind query" >&2
    return 1
  fi
  paste -d' ' "$names" "$out.a" | sed "s/^/$kind /"
}

if (( LEARN )); then
  (( ${#cmd[@]} )) || { echo "--learn needs -- followed by a command" >&2; exit 2; }
  [[ "$OUT" != "-" ]] || { echo "--learn needs -o TABLE" >&2; exit 2; }
  tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
  [[ -f "$OUT" ]] || : > "$OUT"

  # Bounded: a round that learns nothing means either success or a failure that
  # more rounds cannot fix, and the cap stops a pathological loop.
  for (( round = 0; round < 50; round++ )); do
    if "${cmd[@]}" 2>"$tmp/err" || (( $? != 3 )); then
      exit 0
    fi
    line="$(grep -m1 'declining: no answer for' "$tmp/err" || true)"
    [[ -n "$line" ]] || { cat "$tmp/err" >&2; exit 1; }
    kind="$(sed -E 's/.*__has_([a-z-]+)\(.*/\1/' <<< "$line")"
    name="$(sed -E 's/.*__has_[a-z-]+\(([^)]*)\).*/\1/' <<< "$line")"
    echo "$name" > "$tmp/one"
    answer_names "$kind" "$tmp/one" "$tmp/w" >> "$OUT" \
      || { echo "gen-has-table: cannot answer __has_$kind($name)" >&2; exit 1; }
  done
  echo "gen-has-table: still declining after 50 rounds" >&2
  exit 1
fi

# The compiler's own header directories, straight from it.
if (( SYSTEM )); then
  while read -r d; do
    [[ -d "$d" ]] && srcs+=("$d")
  done < <("$CC" -E -v -xc /dev/null -o /dev/null 2>&1 \
           | sed -n '/#include <...> search starts here:/,/End of search list./p' \
           | sed -n 's/^ //p')
fi
(( ${#srcs[@]} )) || { echo "usage: gen-has-table.sh [--cc CC] [-o OUT] FILE_OR_DIR..." >&2; exit 2; }

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

# Harvest "__has_KIND (name)" occurrences. Only literal spellings are found;
# a name reached through macro expansion will show up later as a decline,
# which is the safe direction to be wrong in.
for kind in attribute builtin; do
  grep -rhoE "__has_${kind}[[:space:]]*\([[:space:]]*[A-Za-z_][A-Za-z0-9_:]*" "${srcs[@]}" 2>/dev/null \
    | sed -E "s/.*\([[:space:]]*//" \
    | sort -u > "$tmp/$kind.names" || true
done

: > "$tmp/table"
for kind in attribute builtin; do
  answer_names "$kind" "$tmp/$kind.names" "$tmp/w" >> "$tmp/table" || exit 1
done

if [[ "$OUT" == "-" ]]; then cat "$tmp/table"; else cp "$tmp/table" "$OUT"; fi
