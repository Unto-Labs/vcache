#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Times vcpp against the compiler it is standing in for, on real translation
# units, and checks the output still matches while doing it. A speedup that
# changes the text is not a speedup, so this refuses to report one.
#
#   ./bench.sh --tree DIR [-n N] [--runs N]
#
# Reports best-of-N wall time and instructions retired. Best-of rather than
# mean because we are measuring the work, not the machine's mood; instructions
# because wall time on a loaded box is noise and instruction count is not.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VCPP="$HERE/../bin/vcpp"
CC="${CC:-gcc}"
TREE="" LIMIT=8 RUNS=7

while (( $# )); do
  case "$1" in
    --tree) TREE="$2"; shift 2 ;;
    -n)     LIMIT="$2"; shift 2 ;;
    --runs) RUNS="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
[[ -n "$TREE" && -d "$TREE" ]] || { echo "usage: bench.sh --tree DIR [-n N] [--runs N]" >&2; exit 2; }
[[ -x "$VCPP" ]] || { echo "no vcpp binary -- run make first" >&2; exit 2; }

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

# Everything vcpp would otherwise fork the compiler for. vcache supplies these;
# probing inside the timed region would measure gcc, not vcpp.
"$CC" -dM -E -xc /dev/null > "$tmp/predef" 2>/dev/null
SYS=()
while read -r d; do [[ -d "$d" ]] && SYS+=(-isystem "$d"); done < <(
  "$CC" -E -v -xc /dev/null -o /dev/null 2>&1 \
  | sed -n '/#include <\.\.\.> search starts here:/,/End of search list./p' \
  | sed -n 's/^ //p')
"$HERE/../tools/gen-has-table.sh" --cc "$CC" -o "$tmp/has" "$TREE" >/dev/null 2>&1 || : > "$tmp/has"

# Harvesting only finds names spelled literally; glibc reaches most of them
# through __glibc_has_attribute. Learn the rest before timing anything, so a
# decline never shows up as a difference.
learn() { "$HERE/../tools/gen-has-table.sh" --cc "$CC" -o "$tmp/has" --learn -- "$@" >/dev/null 2>&1; }

VARGS=(--has-table="$tmp/has" --predef="$tmp/predef" "${SYS[@]}" -I"$TREE")

# Biggest translation units first: they are where the time is, and small ones
# are dominated by process startup.
mapfile -t files < <(
  for f in "$TREE"/*.c; do
    [[ -f "$f" ]] || continue
    printf '%s %s\n' "$("$CC" -E -I"$TREE" "$f" 2>/dev/null | wc -c)" "$f"
  done | sort -rn | head -"$LIMIT" | cut -d' ' -f2)
(( ${#files[@]} )) || { echo "no usable .c files under $TREE" >&2; exit 2; }

best_time() {  # best-of-RUNS wall seconds
  local b=999 t
  for (( i = 0; i < RUNS; i++ )); do
    t=$( { /usr/bin/time -f "%e" "$@" >/dev/null; } 2>&1 | tail -1 )
    b=$(awk -v a="$t" -v b="$b" 'BEGIN{print (a<b)?a:b}')
  done
  echo "$b"
}
insns() { perf stat -e instructions -x, -- "$@" 2>&1 >/dev/null | awk -F, '/instructions/{print $1}'; }

printf '%-22s %10s %10s %10s %12s %12s\n' file gcc_s vcpp_s ratio gcc_insn vcpp_insn
tg=0; tv=0; ig=0; iv=0; bad=0
for f in "${files[@]}"; do
  "$CC" -E -I"$TREE" "$f" -o "$tmp/g.i" 2>/dev/null || continue
  learn "$VCPP" "${VARGS[@]}" "$f" -o /dev/null
  if ! "$VCPP" "${VARGS[@]}" "$f" -o "$tmp/v.i" 2>/dev/null || ! cmp -s "$tmp/g.i" "$tmp/v.i"; then
    printf '%-22s %s\n' "$(basename "$f")" "OUTPUT DIFFERS -- not benchmarking"
    bad=$((bad+1)); continue
  fi
  g=$(best_time "$CC" -E -I"$TREE" "$f" -o /dev/null)
  v=$(best_time "$VCPP" "${VARGS[@]}" "$f" -o /dev/null)
  gi=$(insns "$CC" -E -I"$TREE" "$f" -o /dev/null)
  vi=$(insns "$VCPP" "${VARGS[@]}" "$f" -o /dev/null)
  printf '%-22s %10s %10s %10s %12s %12s\n' "$(basename "$f")" "$g" "$v" \
    "$(awk -v a="$g" -v b="$v" 'BEGIN{printf "%.2fx", a/b}')" "$gi" "$vi"
  tg=$(awk -v a="$tg" -v b="$g" 'BEGIN{print a+b}'); tv=$(awk -v a="$tv" -v b="$v" 'BEGIN{print a+b}')
  ig=$((ig + gi)); iv=$((iv + vi))
done

printf '\ntotal %ss vs %ss  -> %s   instructions %s vs %s -> %s\n' \
  "$tg" "$tv" "$(awk -v a="$tg" -v b="$tv" 'BEGIN{printf "%.2fx", a/b}')" \
  "$ig" "$iv" "$(awk -v a="$ig" -v b="$iv" 'BEGIN{printf "%.2fx", a/b}')"
(( bad == 0 )) || { echo "$bad file(s) differed; speedups on wrong output do not count" >&2; exit 1; }
