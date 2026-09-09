#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The acceptance test: does vcpp produce exactly what `gcc -E` produces?
#
# Byte-identity is the bar, not "equivalent output". vcache hashes preprocessed
# text to decide whether two compilations are the same one; if vcpp's text
# differs from what the compiler will actually consume, that reasoning no
# longer holds, and the failure mode is a wrong object rather than a slow
# build. Anything short of byte-identity is a red test.
#
#   ./compare.sh                     run against the bundled corpus
#   ./compare.sh --tree DIR [-n N]   run against a real build tree, taking the
#                                    compile commands kbuild recorded in its
#                                    .*.o.cmd files
#
# The weaker "tokens match, formatting differs" line is progress reporting
# while the output driver is being brought up. It is not a pass.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VCPP="$HERE/../bin/vcpp"
CC="${CC:-gcc}"
TREE="" LIMIT=50

while (( $# )); do
  case "$1" in
    --tree) TREE="$2"; shift 2 ;;
    -n)     LIMIT="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

[[ -x "$VCPP" ]] || { echo "no vcpp binary -- run make first" >&2; exit 2; }

PASS=0 TOKENS=0 FAIL=0 DECLINED=0
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

# vcpp answers __has_attribute / __has_builtin from a table and declines
# without one. Build it from whatever we are about to preprocess.
HAS_TABLE="$tmp/has.tbl"
"$HERE/../tools/gen-has-table.sh" --cc "$CC" -o "$HAS_TABLE" \
  "${TREE:-$HERE/corpus}" 2>/dev/null || : > "$HAS_TABLE"

# Compares one preprocessing run. Extra arguments are passed to both.
compare_one() {
  local src="$1"; shift
  "$CC" "$@" -E "$src" -o "$tmp/gcc.i" 2>/dev/null || return 0   # gcc itself failed; not our business
  # Learn any __has_* name the harvest missed before judging the output; a
  # decline that a table entry would have answered is not a real decline.
  "$HERE/../tools/gen-has-table.sh" --cc "$CC" -o "$HAS_TABLE" --learn -- \
    "$VCPP" --has-table="$HAS_TABLE" "$@" "$src" -o /dev/null >/dev/null 2>&1
  "$VCPP" --has-table="$HAS_TABLE" "$@" "$src" -o "$tmp/vcpp.i" 2>"$tmp/err"
  local rc=$?
  # 3 is vcpp declining: it met something it cannot answer and said so rather
  # than guessing. vcache falls back to the real compiler, so the build stays
  # correct -- but a decline is a lost hit, so it is reported, not hidden.
  if (( rc == 3 )); then
    printf '  \033[36mDECLINE\033[0m %s (%s)\n' "$src" "$(grep -m1 declining "$tmp/err" | sed 's/^vcpp: declining: //')"
    DECLINED=$((DECLINED+1)); return 0
  elif (( rc != 0 )); then
    printf '  \033[31mFAIL\033[0m %s (vcpp exited %d: %s)\n' "$src" "$rc" "$(head -1 "$tmp/err")"
    FAIL=$((FAIL+1)); return 0
  fi
  if cmp -s "$tmp/gcc.i" "$tmp/vcpp.i"; then
    PASS=$((PASS+1)); return 0
  fi
  # Same tokens, different layout: worth distinguishing while bringing the
  # output driver up, but still not a pass.
  local a b
  a="$(grep -v '^#' "$tmp/gcc.i"  | tr -s ' \t\n' ' ')"
  b="$(grep -v '^#' "$tmp/vcpp.i" | tr -s ' \t\n' ' ')"
  if [[ "$a" == "$b" ]]; then
    printf '  \033[33mTOKENS\033[0m %s (token stream matches, formatting differs)\n' "$src"
    TOKENS=$((TOKENS+1))
  else
    printf '  \033[31mFAIL\033[0m %s\n' "$src"
    FAIL=$((FAIL+1))
  fi
}

if [[ -n "$TREE" ]]; then
  # A real tree: replay the compile kbuild recorded, minus the flags that name
  # its dependency file and its object.
  mapfile -t cmds < <(cd "$TREE" && find . -name '.*.o.cmd' | sort | head -"$((LIMIT * 4))")
  n=0
  for c in "${cmds[@]}"; do
    (( n >= LIMIT )) && break
    line="$(sed -n '1s/^savedcmd_[^ ]* := //p' "$TREE/$c")"
    # Only the compile: kbuild appends objtool after a ';'.
    line="${line%% ; *}"; line="${line%%; objtool*}"
    read -r -a argv <<< "$line"
    [[ "${argv[0]##*/}" == gcc || "${argv[0]##*/}" == cc ]] || continue
    src="${argv[-1]}"
    [[ "$src" == *.c && -f "$TREE/$src" ]] || continue
    flags=(); skip=0
    for a in "${argv[@]:1}"; do
      (( skip )) && { skip=0; continue; }
      case "$a" in
        -Wp,-MMD,*|-Wp,-MD,*) continue ;;
        -o) skip=1; continue ;;
        -c|"$src") continue ;;
      esac
      flags+=("$a")
    done
    ( cd "$TREE" && compare_one "$src" "${flags[@]}" )
    n=$((n+1))
  done
else
  for src in "$HERE"/corpus/*.c; do
    [[ -f "$src" ]] || continue
    compare_one "$src" -I"$HERE/corpus"
  done
fi

printf '\nbyte-identical to %s: %d   tokens-only: %d   declined: %d   failed: %d\n' \
  "$CC" "$PASS" "$TOKENS" "$DECLINED" "$FAIL"
[[ "$TOKENS" -eq 0 && "$FAIL" -eq 0 && "$DECLINED" -eq 0 ]]
