#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Unto Labs
# SPDX-License-Identifier: Apache-2.0
# End-to-end tests for vcache.
#
# The central claim under test is that the same source compiled from two
# unrelated directories produces one cache entry and byte-identical objects.
# Everything else here guards the paths that claim depends on.

set -uo pipefail

TOP="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VCACHE="$TOP/bin/vcache"

# $TMPDIR ends in a slash on macOS, which would make every path here carry a
# doubled separator -- ".../T//vcache-it-XXXX/..." -- and vcache deliberately
# does not match such a spelling against a root (see RootMap::Canonicalize).
# The suite would then be testing that case everywhere rather than the one it
# means to. Strip it so the paths are ordinary.
TMPBASE="${TMPDIR:-/tmp}"
while [[ "$TMPBASE" == */ && "$TMPBASE" != "/" ]]; do TMPBASE="${TMPBASE%/}"; done
WORK="$(mktemp -d "$TMPBASE/vcache-it-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0

ok()   { printf '  \033[32mPASS\033[0m %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAIL=$((FAIL+1)); }
check() { if [[ "$2" == "$3" ]]; then ok "$1"; else bad "$1 (expected '$3', got '$2')"; fi; }
skipped() { printf '  \033[33mSKIP\033[0m %s\n' "$1"; }

# BSD sed -i wants a backup suffix and GNU sed -i must not have one separated
# from it, so no single spelling works on both -- and getting it wrong makes sed
# read the *filename* as its script. Edit through a temporary instead, writing
# back with cat so the file keeps its mode; one of these files is an executable.
sed_inplace() {
  local script=$1 file=$2
  sed "$script" "$file" > "$file.sed.tmp" && cat "$file.sed.tmp" > "$file"
  rm -f "$file.sed.tmp"
}

# True when the C compiler called `gcc` is really clang, as it is on macOS.
# Some expectations differ between the two and are not about vcache being wrong.
cc_is_clang() { gcc --version 2>/dev/null | head -1 | grep -qi clang; }
section() { printf '\n\033[1m%s\033[0m\n' "$1"; }

# Releases a manifest-pause fifo. The write happens only when the compile is
# actually waiting; otherwise that process is stopped. The write is bounded
# because opening a fifo with no reader blocks.
release_pause_fifo() {  # $1 fifo, $2 pid, $3 waiting (1 or 0)
  local fifo=$1 pid=$2 waiting=$3
  if [[ "$waiting" != 1 ]]; then
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    return
  fi
  printf x > "$fifo" &
  local wpid=$!
  local n=0
  while kill -0 "$wpid" 2>/dev/null; do
    if (( n >= 50 )); then
      kill "$wpid" 2>/dev/null || true
      wait "$wpid" 2>/dev/null || true
      return
    fi
    sleep 0.05
    n=$((n + 1))
  done
  wait "$wpid" 2>/dev/null || true
}

export VCACHE_DIR="$WORK/cache"

# Dumps the paths an object records in its debug info. readelf is GNU's; macOS
# has dwarfdump. Both are asked for the compilation-unit info; `strings` is the
# last resort and is enough for "does this path appear at all", which is all
# these checks ask. Without this the greps below found nothing on macOS, which
# failed one check and silently *passed* its negated twin.
debug_info() {
  if command -v readelf >/dev/null 2>&1; then
    readelf --debug-dump=info "$1" 2>/dev/null
  elif command -v dwarfdump >/dev/null 2>&1; then
    dwarfdump --debug-info "$1" 2>/dev/null
  else
    strings "$1" 2>/dev/null
  fi
}

# The shared libraries a binary needs at load time, one per line.
linked_libraries() {
  if command -v readelf >/dev/null 2>&1; then
    readelf -d "$1" 2>/dev/null | grep NEEDED
  elif command -v otool >/dev/null 2>&1; then
    otool -L "$1" 2>/dev/null | tail -n +2
  fi
}

# When two checkouts fail to share an entry, the useful questions are what
# vcache resolved the roots to and what the two preprocessed texts -- which are
# the cache key -- disagree about. Answer both here rather than leave someone to
# reconstruct the compile by hand from a bare "expected 1, got 0".
#
#   explain_cross_checkout_miss DIR_A SPEC_A DIR_B SPEC_B  A args... -- B args...
explain_cross_checkout_miss() {
  local dir_a=$1 spec_a=$2 dir_b=$3 spec_b=$4; shift 4
  local -a args_a=() args_b=(); local side=a
  local x
  for x in "$@"; do
    if [[ "$x" == "--" ]]; then side=b; continue; fi
    if [[ "$side" == a ]]; then args_a+=("$x"); else args_b+=("$x"); fi
  done

  printf '       roots in A:\n'
  ( cd "$dir_a" && VCACHE_ROOTS="$spec_a" "$VCACHE" --show-roots ) 2>&1 | sed 's/^/         /'
  printf '       roots in B:\n'
  ( cd "$dir_b" && VCACHE_ROOTS="$spec_b" "$VCACHE" --show-roots ) 2>&1 | sed 's/^/         /'

  local la="$WORK/miss-a.log" lb="$WORK/miss-b.log"
  rm -f "$la" "$lb"
  ( cd "$dir_a" && VCACHE_ROOTS="$spec_a" VCACHE_LOG="$la" "$VCACHE" "${args_a[@]}" ) >/dev/null 2>&1
  ( cd "$dir_b" && VCACHE_ROOTS="$spec_b" VCACHE_LOG="$lb" "$VCACHE" "${args_b[@]}" ) >/dev/null 2>&1
  local ca cb
  ca=$(sed -n 's/.*\] preprocess: //p' "$la" 2>/dev/null | head -1)
  cb=$(sed -n 's/.*\] preprocess: //p' "$lb" 2>/dev/null | head -1)
  ( cd "$dir_a" && eval "$ca" ) > "$WORK/pp-a.i" 2>/dev/null
  ( cd "$dir_b" && eval "$cb" ) > "$WORK/pp-b.i" 2>/dev/null
  if cmp -s "$WORK/pp-a.i" "$WORK/pp-b.i"; then
    printf '       preprocessed text is identical; the key differs elsewhere\n'
  else
    printf '       preprocessed text differs:\n'
    diff "$WORK/pp-a.i" "$WORK/pp-b.i" 2>/dev/null | head -6 | sed 's/^/         /'
  fi
}

# Counter helpers read straight from --show-stats.
stat_of() { "$VCACHE" --show-stats | grep -F "$1" | awk '{print $NF}'; }
hits()   { stat_of "cache hit (disk)"; }
misses() { stat_of "cache miss"; }
uncacheable() { stat_of "uncacheable"; }
reset_cache() { rm -rf "$VCACHE_DIR"; }

# Counts entries in the local layer. Entries live in two-character shard
# directories, which the "??" pattern picks out from the cache directory's other
# contents: stats, compilers/ and native/.
disk_entries() {
  find "$VCACHE_DIR" -mindepth 2 -maxdepth 2 -type f -path "$VCACHE_DIR/??/*" \
    2>/dev/null | wc -l | tr -d ' '
}

# True if the compiler accepts every flag given. gcc has options clang does not
# -- -fopt-info and -aux-info among them -- and the tests that assert the file
# such a flag writes cannot mean anything on a compiler that never took it.
compiler_supports() {
  local probe="$WORK/.probe.c"
  printf 'int main(void){return 0;}\n' > "$probe"
  ( cd "$WORK" && gcc "$@" -c "$probe" -o "$WORK/.probe.o" ) >/dev/null 2>&1
}

# Builds a small project tree at $1 whose contents are identical everywhere.
make_project() {
  local root="$1"
  mkdir -p "$root/src" "$root/include"
  cat > "$root/include/lib.h" <<'EOF'
#pragma once
// Deliberately uses __FILE__: it is one of the four things that normally
// prevent cross-directory cache hits.
inline const char* lib_location() { return __FILE__; }
int add(int a, int b);
EOF
  cat > "$root/src/lib.cc" <<'EOF'
#include "lib.h"
int add(int a, int b) { return a + b; }
const char* impl_location() { return __FILE__; }
EOF
}

# --------------------------------------------------------------------------
section "1. cross-directory cache hits (the headline case)"

make_project "$WORK/checkout-a"
make_project "$WORK/checkout-b"
reset_cache

( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
    "$VCACHE" g++ -g -O2 -c -I include src/lib.cc -o "$WORK/a.o" ) 2>/dev/null
check "first compile is a miss" "$(misses)" "1"

( cd "$WORK/checkout-b" && VCACHE_ROOTS="$WORK/checkout-b=proj" \
    "$VCACHE" g++ -g -O2 -c -I include src/lib.cc -o "$WORK/b.o" ) 2>/dev/null
if [[ "$(hits)" == "1" ]]; then
  ok "second checkout hits the cache"
else
  bad "second checkout hits the cache (got $(hits))"
  explain_cross_checkout_miss \
    "$WORK/checkout-a" "$WORK/checkout-a=proj" \
    "$WORK/checkout-b" "$WORK/checkout-b=proj" \
    g++ -g -O2 -c -I include src/lib.cc -o "$WORK/diag-a.o" -- \
    g++ -g -O2 -c -I include src/lib.cc -o "$WORK/diag-b.o"
fi

if cmp -s "$WORK/a.o" "$WORK/b.o"; then
  ok "objects from both checkouts are byte-identical"
else
  bad "objects from both checkouts are byte-identical"
fi

# The canonical prefix must be what actually landed in the debug info.
if debug_info "$WORK/a.o" | grep -q "/vcache/proj"; then
  ok "debug info records the canonical path"
else
  bad "debug info records the canonical path"
fi
if debug_info "$WORK/a.o" | grep -q "checkout-a"; then
  bad "debug info leaks the local path"
else
  ok "debug info does not leak the local path"
fi

# --------------------------------------------------------------------------
section "2. out-of-tree builds (cwd differs, source tree does not)"

reset_cache
mkdir -p "$WORK/build-a" "$WORK/build-b"

( cd "$WORK/build-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
    "$VCACHE" g++ -g -c -I "$WORK/checkout-a/include" \
    "$WORK/checkout-a/src/lib.cc" -o out.o ) 2>/dev/null
( cd "$WORK/build-b" && VCACHE_ROOTS="$WORK/checkout-b=proj" \
    "$VCACHE" g++ -g -c -I "$WORK/checkout-b/include" \
    "$WORK/checkout-b/src/lib.cc" -o out.o ) 2>/dev/null

if [[ "$(hits)" == "1" ]]; then
  ok "out-of-tree build hits across checkouts"
else
  bad "out-of-tree build hits across checkouts (got $(hits))"
  explain_cross_checkout_miss \
    "$WORK/build-a" "$WORK/checkout-a=proj" \
    "$WORK/build-b" "$WORK/checkout-b=proj" \
    g++ -g -c -I "$WORK/checkout-a/include" "$WORK/checkout-a/src/lib.cc" -o diag.o -- \
    g++ -g -c -I "$WORK/checkout-b/include" "$WORK/checkout-b/src/lib.cc" -o diag.o
fi
if cmp -s "$WORK/build-a/out.o" "$WORK/build-b/out.o"; then
  ok "out-of-tree objects are byte-identical"
else
  bad "out-of-tree objects are byte-identical"
fi

# A root reached through a symlink must map the same as one reached directly.
# vcache resolves the root it is given, but the compiler is handed whatever the
# build system wrote -- and on macOS $TMPDIR lives under /var, a symlink to
# /private/var, so this is the ordinary case there rather than a corner.
reset_cache
mkdir -p "$WORK/symlinked"
ln -sfn "$WORK/checkout-a" "$WORK/symlinked/via-link"

( cd "$WORK/build-a" && VCACHE_ROOTS="$WORK/symlinked/via-link=proj" \
    "$VCACHE" g++ -g -c -I "$WORK/symlinked/via-link/include" \
    "$WORK/symlinked/via-link/src/lib.cc" -o sym.o ) 2>/dev/null
check "a root given through a symlink compiles" "$?" "0"
check "and records the canonical path, not the link" \
  "$(debug_info "$WORK/build-a/sym.o" | grep -c 'symlinked/via-link')" "0"

( cd "$WORK/build-b" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
    "$VCACHE" g++ -g -c -I "$WORK/checkout-a/include" \
    "$WORK/checkout-a/src/lib.cc" -o sym.o ) 2>/dev/null
check "the same tree by its real path hits that entry" "$(hits)" "1"
check "and the objects are byte-identical" \
  "$(cmp -s "$WORK/build-a/sym.o" "$WORK/build-b/sym.o" && echo same)" "same"

# --------------------------------------------------------------------------
section "3. dependency files are replayed with local paths"

reset_cache
( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
    "$VCACHE" g++ -MD -MF "$WORK/a.d" -c -I include src/lib.cc -o "$WORK/a.o" ) 2>/dev/null
( cd "$WORK/checkout-b" && VCACHE_ROOTS="$WORK/checkout-b=proj" \
    "$VCACHE" g++ -MD -MF "$WORK/b.d" -c -I include src/lib.cc -o "$WORK/b.o" ) 2>/dev/null

check "compile with -MD hits on the second tree" "$(hits)" "1"

if grep -q "/vcache/proj" "$WORK/b.d"; then
  bad "replayed depfile leaked canonical paths"
else
  ok "replayed depfile contains no canonical paths"
fi
# The rule target must be this build's object, not the one the entry was
# originally stored for and not vcache's internal temporary.
if head -1 "$WORK/b.d" | grep -qF "$WORK/b.o"; then
  ok "replayed depfile targets this build's object"
else
  bad "replayed depfile targets this build's object (got: $(head -1 "$WORK/b.d"))"
fi
# vcache stages the object in a mkdtemp directory named vcache-XXXXXX; that
# path must never reach the .d file. (Matched precisely, since this test's own
# work directory also begins with "vcache-".)
if grep -qE 'vcache-[A-Za-z0-9]{6}/out' "$WORK/b.d"; then
  bad "replayed depfile leaked a vcache staging path"
else
  ok "replayed depfile contains no vcache staging paths"
fi
# Every prerequisite must resolve, or make reports a missing dependency. Paths
# may legitimately be relative to the build directory.
missing=0
while read -r dep; do
  [[ -z "$dep" || "$dep" == *: || "$dep" == "\\" ]] && continue
  [[ -e "$WORK/checkout-b/$dep" || -e "$dep" ]] || { missing=$((missing+1)); echo "    missing: $dep"; }
done < <(tr -s ' \\\n' '\n' < "$WORK/b.d")
check "all replayed prerequisites resolve" "$missing" "0"

# The flag combination real build systems emit. -MP and -MF must never reach the
# preprocessing command, or every compile silently degrades to a passthrough.
reset_cache
( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
    "$VCACHE" g++ -MMD -MP -MF "$WORK/mp.d" -c -I include src/lib.cc \
    -o "$WORK/mp.o" ) 2>/dev/null
check "-MMD -MP -MF compiles and caches" "$(misses)" "1"
check "-MMD -MP -MF is not a preprocess failure" "$(stat_of 'preprocess failed')" "0"
if grep -q "^include/lib.h:$\|^ *include/lib.h:" "$WORK/mp.d" || \
   grep -qE '^[^ ]+\.h:$' "$WORK/mp.d"; then
  ok "-MP phony targets are present"
else
  bad "-MP phony targets are present"
fi
( cd "$WORK/checkout-b" && VCACHE_ROOTS="$WORK/checkout-b=proj" \
    "$VCACHE" g++ -MMD -MP -MF "$WORK/mp2.d" -c -I include src/lib.cc \
    -o "$WORK/mp2.o" ) 2>/dev/null
check "-MMD -MP hits across checkouts" "$(hits)" "1"

# --------------------------------------------------------------------------
section "4. correctness: different code must not share an entry"

reset_cache
mkdir -p "$WORK/diff/src"
cat > "$WORK/diff/src/x.cc" <<'EOF'
int f() { return 1; }
EOF
( cd "$WORK/diff" && VCACHE_ROOTS="$WORK/diff=proj" \
    "$VCACHE" g++ -c src/x.cc -o "$WORK/x1.o" ) 2>/dev/null
cat > "$WORK/diff/src/x.cc" <<'EOF'
int f() { return 2; }
EOF
( cd "$WORK/diff" && VCACHE_ROOTS="$WORK/diff=proj" \
    "$VCACHE" g++ -c src/x.cc -o "$WORK/x2.o" ) 2>/dev/null
check "changed source produces a second miss" "$(misses)" "2"
if cmp -s "$WORK/x1.o" "$WORK/x2.o"; then
  bad "different sources produced identical objects"
else
  ok "different sources produce different objects"
fi

# Optimisation level must be part of the key.
reset_cache
( cd "$WORK/diff" && VCACHE_ROOTS="$WORK/diff=proj" \
    "$VCACHE" g++ -O0 -c src/x.cc -o "$WORK/o0.o" ) 2>/dev/null
( cd "$WORK/diff" && VCACHE_ROOTS="$WORK/diff=proj" \
    "$VCACHE" g++ -O2 -c src/x.cc -o "$WORK/o2.o" ) 2>/dev/null
check "-O0 and -O2 do not share an entry" "$(misses)" "2"

# A -D that changes the code must be reflected via the preprocessed text.
reset_cache
cat > "$WORK/diff/src/d.cc" <<'EOF'
#ifdef ENABLE
int g() { return 10; }
#else
int g() { return 20; }
#endif
EOF
( cd "$WORK/diff" && VCACHE_ROOTS="$WORK/diff=proj" \
    "$VCACHE" g++ -c src/d.cc -o "$WORK/d1.o" ) 2>/dev/null
( cd "$WORK/diff" && VCACHE_ROOTS="$WORK/diff=proj" \
    "$VCACHE" g++ -DENABLE -c src/d.cc -o "$WORK/d2.o" ) 2>/dev/null
check "-D changing code produces a second miss" "$(misses)" "2"

# ...but a -D that does NOT change the preprocessed result should still hit,
# which is the payoff of excluding preprocessor flags from the key.
reset_cache
( cd "$WORK/diff" && VCACHE_ROOTS="$WORK/diff=proj" \
    "$VCACHE" g++ -c src/x.cc -o "$WORK/u1.o" ) 2>/dev/null
( cd "$WORK/diff" && VCACHE_ROOTS="$WORK/diff=proj" \
    "$VCACHE" g++ -DUNUSED_MACRO=1 -c src/x.cc -o "$WORK/u2.o" ) 2>/dev/null
check "an unused -D still hits" "$(hits)" "1"

# --------------------------------------------------------------------------
section "5. compiler diagnostics are replayed"

reset_cache
mkdir -p "$WORK/warn/src"
cat > "$WORK/warn/src/w.cc" <<'EOF'
int h() { int unused = 5; return 0; }
EOF
( cd "$WORK/warn" && VCACHE_ROOTS="$WORK/warn=proj" \
    "$VCACHE" g++ -Wall -c src/w.cc -o "$WORK/w1.o" ) 2>"$WORK/warn1.txt"
( cd "$WORK/warn" && VCACHE_ROOTS="$WORK/warn=proj" \
    "$VCACHE" g++ -Wall -c src/w.cc -o "$WORK/w2.o" ) 2>"$WORK/warn2.txt"
check "second warning compile is a hit" "$(hits)" "1"
if grep -q "unused" "$WORK/warn1.txt" && grep -q "unused" "$WORK/warn2.txt"; then
  ok "warning text is replayed from cache"
else
  bad "warning text is replayed from cache"
fi

# --------------------------------------------------------------------------
section "6. uncacheable invocations fall back cleanly"

reset_cache
cat > "$WORK/diff/src/m.cc" <<'EOF'
int main() { return 0; }
EOF
( cd "$WORK/diff" && "$VCACHE" g++ src/m.cc -o "$WORK/prog" ) 2>/dev/null
rc=$?
check "linking still succeeds" "$rc" "0"
check "linking is counted uncacheable" "$(uncacheable)" "1"
if [[ -x "$WORK/prog" ]] && "$WORK/prog"; then
  ok "linked program runs"
else
  bad "linked program runs"
fi

# A compile error must surface with the compiler's own exit code.
cat > "$WORK/diff/src/bad.cc" <<'EOF'
int broken( { }
EOF
( cd "$WORK/diff" && "$VCACHE" g++ -c src/bad.cc -o "$WORK/bad.o" ) 2>/dev/null
check "compile error propagates non-zero exit" "$?" "1"

# --------------------------------------------------------------------------
section "7. incoming prefix-map flags require an explicit decision"

reset_cache

# Default policy is error: vcache must refuse rather than silently override a
# mapping the build system asked for.
rm -f "$WORK/p1.o"
( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
    "$VCACHE" g++ -g -ffile-prefix-map="$WORK/checkout-a"=/somewhere \
    -c -I include src/lib.cc -o "$WORK/p1.o" ) >/dev/null 2>"$WORK/p1.err"
check "default policy refuses the invocation" "$?" "1"
check "refused invocation produces no object" "$([[ -e "$WORK/p1.o" ]] && echo yes || echo no)" "no"
if grep -q -- "--vcache-allow-prefix-maps" "$WORK/p1.err"; then
  ok "refusal explains how to override"
else
  bad "refusal explains how to override"
fi

# An ordinary compile with no prefix-map flag must be unaffected.
rm -f "$WORK/p0.o"
( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
    "$VCACHE" g++ -g -c -I include src/lib.cc -o "$WORK/p0.o" ) 2>/dev/null
check "compiles without prefix maps are unaffected" "$?" "0"

# CLI override: strip the caller's flag and apply vcache's own.
rm -f "$WORK/p2.o"
( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
    "$VCACHE" --vcache-allow-prefix-maps g++ -g \
    -ffile-prefix-map="$WORK/checkout-a"=/somewhere \
    -c -I include src/lib.cc -o "$WORK/p2.o" ) 2>/dev/null
check "--vcache-allow-prefix-maps compiles" "$?" "0"
if debug_info "$WORK/p2.o" | grep -q "/somewhere"; then
  bad "override drops the caller's mapping"
else
  ok "override drops the caller's mapping"
fi
if debug_info "$WORK/p2.o" | grep -q "/vcache/proj"; then
  ok "override applies vcache's mapping instead"
else
  bad "override applies vcache's mapping instead"
fi

# Environment override.
rm -f "$WORK/p3.o"
( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
    VCACHE_INCOMING_PREFIX_MAPS=strip \
    "$VCACHE" g++ -g -ffile-prefix-map=/a=/b -c -I include src/lib.cc \
    -o "$WORK/p3.o" ) 2>/dev/null
check "VCACHE_INCOMING_PREFIX_MAPS=strip compiles" "$?" "0"

# Config-file override.
rm -f "$WORK/p4.o"
mkdir -p "$WORK/cfg"
cat > "$WORK/cfg/config.toml" <<EOF
[vcache]
incoming_prefix_maps = "strip"
EOF
( cd "$WORK/checkout-a" && VCACHE_CONFIG="$WORK/cfg/config.toml" \
    VCACHE_ROOTS="$WORK/checkout-a=proj" \
    "$VCACHE" g++ -g -ffile-prefix-map=/a=/b -c -I include src/lib.cc \
    -o "$WORK/p4.o" ) 2>/dev/null
check "config-file override compiles" "$?" "0"

# CLI must beat the environment.
rm -f "$WORK/p5.o"
( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
    VCACHE_INCOMING_PREFIX_MAPS=error \
    "$VCACHE" --vcache-incoming-prefix-maps=strip g++ -g \
    -ffile-prefix-map=/a=/b -c -I include src/lib.cc -o "$WORK/p5.o" ) 2>/dev/null
check "CLI overrides the environment" "$?" "0"

# policy=keep passes the caller's flag through and does not cache.
reset_cache
rm -f "$WORK/p6.o"
( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
    "$VCACHE" --vcache-incoming-prefix-maps=keep g++ -g \
    -ffile-prefix-map="$WORK/checkout-a"=/somewhere \
    -c -I include src/lib.cc -o "$WORK/p6.o" ) 2>/dev/null
check "policy=keep compiles" "$?" "0"
check "policy=keep does not cache" "$(uncacheable)" "1"
if debug_info "$WORK/p6.o" | grep -q "/somewhere"; then
  ok "policy=keep preserves the caller's mapping"
else
  bad "policy=keep preserves the caller's mapping"
fi

# A misspelled policy must be rejected rather than silently ignored.
"$VCACHE" --vcache-incoming-prefix-maps=nonsense g++ -c "$WORK/diff/src/x.cc" \
  -o "$WORK/p7.o" >/dev/null 2>&1
check "an unknown policy name is rejected" "$?" "1"

# rustc gets the same treatment.
if command -v rustc >/dev/null 2>&1; then
  ( cd "$WORK/checkout-a" && "$VCACHE" rustc --crate-name x --crate-type lib \
      --emit=link --out-dir "$WORK/rsout" --remap-path-prefix=/a=/b \
      "$WORK/checkout-a/src/lib.cc" ) >/dev/null 2>&1
  check "rustc path also refuses by default" "$?" "1"
fi

# --------------------------------------------------------------------------
section "8. masquerade mode (symlink named after the compiler)"

reset_cache
mkdir -p "$WORK/bin"
ln -sf "$VCACHE" "$WORK/bin/g++"
( cd "$WORK/checkout-a" && PATH="$WORK/bin:$PATH" \
    VCACHE_ROOTS="$WORK/checkout-a=proj" \
    g++ -g -O2 -c -I include src/lib.cc -o "$WORK/m1.o" ) 2>/dev/null
check "masquerade compile is a miss" "$(misses)" "1"
( cd "$WORK/checkout-b" && PATH="$WORK/bin:$PATH" \
    VCACHE_ROOTS="$WORK/checkout-b=proj" \
    g++ -g -O2 -c -I include src/lib.cc -o "$WORK/m2.o" ) 2>/dev/null
check "masquerade hits across checkouts" "$(hits)" "1"

# --------------------------------------------------------------------------
section "9. Rust"

if command -v rustc >/dev/null 2>&1; then
  reset_cache
  for tree in rust-a rust-b; do
    mkdir -p "$WORK/$tree/src"
    cat > "$WORK/$tree/src/lib.rs" <<'EOF'
mod helper;
pub fn location() -> &'static str { file!() }
pub fn value() -> u32 { helper::value() }
EOF
    cat > "$WORK/$tree/src/helper.rs" <<'EOF'
pub fn value() -> u32 { 42 }
EOF
  done

  ( cd "$WORK/rust-a" && VCACHE_ROOTS="$WORK/rust-a=crate" \
      "$VCACHE" rustc --crate-name demo --crate-type lib -C debuginfo=2 \
      --emit=dep-info,link --out-dir "$WORK/rust-a/out" src/lib.rs ) 2>/dev/null
  check "first rust compile is a miss" "$(misses)" "1"

  ( cd "$WORK/rust-b" && VCACHE_ROOTS="$WORK/rust-b=crate" \
      "$VCACHE" rustc --crate-name demo --crate-type lib -C debuginfo=2 \
      --emit=dep-info,link --out-dir "$WORK/rust-b/out" src/lib.rs ) 2>/dev/null
  check "second rust checkout hits" "$(hits)" "1"

  if cmp -s "$WORK/rust-a/out/libdemo.rlib" "$WORK/rust-b/out/libdemo.rlib"; then
    ok "rlibs are byte-identical across checkouts"
  else
    bad "rlibs are byte-identical across checkouts"
  fi
  if [[ -f "$WORK/rust-b/out/demo.d" ]] && grep -q "rust-b" "$WORK/rust-b/out/demo.d"; then
    ok "rust dep-info is localised on restore"
  else
    bad "rust dep-info is localised on restore"
  fi

  # A changed crate must not reuse the entry.
  echo 'pub fn extra() -> u32 { 7 }' >> "$WORK/rust-b/src/helper.rs"
  ( cd "$WORK/rust-b" && VCACHE_ROOTS="$WORK/rust-b=crate" \
      "$VCACHE" rustc --crate-name demo --crate-type lib -C debuginfo=2 \
      --emit=dep-info,link --out-dir "$WORK/rust-b/out" src/lib.rs ) 2>/dev/null
  check "changed rust module produces a miss" "$(misses)" "2"

  # A binary crate: cargo runs build scripts and binaries straight out of the
  # output directory, so the execute bit has to survive both the miss (where
  # vcache places the artifacts itself) and the hit.
  mkdir -p "$WORK/rust-bin-a/src" "$WORK/rust-bin-b/src"
  for tree in rust-bin-a rust-bin-b; do
    cat > "$WORK/$tree/src/main.rs" <<'EOF'
fn main() { println!("built at {}", file!()); }
EOF
  done

  ( cd "$WORK/rust-bin-a" && VCACHE_ROOTS="$WORK/rust-bin-a=bin" \
      "$VCACHE" rustc --crate-name demobin --crate-type bin \
      --emit=dep-info,link --out-dir "$WORK/rust-bin-a/out" src/main.rs ) 2>/dev/null
  check "a fresh binary crate is executable" \
    "$([[ -x "$WORK/rust-bin-a/out/demobin" ]] && echo yes)" "yes"

  ( cd "$WORK/rust-bin-b" && VCACHE_ROOTS="$WORK/rust-bin-b=bin" \
      "$VCACHE" rustc --crate-name demobin --crate-type bin \
      --emit=dep-info,link --out-dir "$WORK/rust-bin-b/out" src/main.rs ) 2>/dev/null
  check "a restored binary crate is executable" \
    "$([[ -x "$WORK/rust-bin-b/out/demobin" ]] && echo yes)" "yes"
  if "$WORK/rust-bin-b/out/demobin" >/dev/null 2>&1; then
    ok "the restored binary actually runs"
  else
    bad "the restored binary actually runs"
  fi
else
  printf '  \033[33mSKIP\033[0m rustc not installed\n'
fi

# --------------------------------------------------------------------------
section "9e. Rust: a rustc rebuilt in place is a new toolchain"

# `rustc -vV` is memoised. A relink often keeps the same byte count, so a memo
# keyed only by path and size answers the new binary from the old banner.
if ! command -v rustc >/dev/null 2>&1; then
  skipped "rustc not installed"
else
  reset_cache
  real_rustc=$(command -v rustc)
  banner=$("$real_rustc" -vV)
  mkdir -p "$WORK/rustc-swap/bin"
  write_fake_rustc() {  # $1 = dest, $2 = release value
    {
      printf '%s\n' '#!/bin/sh'
      printf '%s\n' 'if [ "$1" = "-vV" ]; then'
      printf '%s\n' "cat <<'END'"
      printf '%s\n' "$banner" | sed "s/^release: .*/release: $2/"
      printf '%s\n' 'END'
      printf '%s\n' 'exit 0'
      printf '%s\n' 'fi'
      printf 'exec %q "$@"\n' "$real_rustc"
    } > "$1"
    chmod +x "$1"
  }
  write_fake_rustc "$WORK/rustc-swap/rustc-a" "9.9.1"
  write_fake_rustc "$WORK/rustc-swap/rustc-b" "9.9.2"
  size_a=$(wc -c < "$WORK/rustc-swap/rustc-a" | tr -d ' ')
  size_b=$(wc -c < "$WORK/rustc-swap/rustc-b" | tr -d ' ')
  check "the two fake rustc scripts are the same size" "$size_a" "$size_b"
  if cmp -s "$WORK/rustc-swap/rustc-a" "$WORK/rustc-swap/rustc-b"; then
    bad "the two fake rustc scripts differ"
  else
    ok "the two fake rustc scripts differ"
  fi
  cp "$WORK/rustc-swap/rustc-a" "$WORK/rustc-swap/bin/rustc"
  chmod +x "$WORK/rustc-swap/bin/rustc"

  swap_log="$WORK/rustc-swap.log"
  : > "$swap_log"
  swap_compile() {
    ( cd "$WORK/rust-a" && VCACHE_ROOTS="$WORK/rust-a=crate" VCACHE_LOG="$swap_log" \
        "$VCACHE" "$WORK/rustc-swap/bin/rustc" --crate-name demo --crate-type lib \
        -C debuginfo=2 --emit=dep-info,link --out-dir "$WORK/rust-a/out" src/lib.rs ) \
        >/dev/null
  }
  swap_compile
  check "the first swapped rustc compile misses" "$(misses)" "1"
  key_before=$(sed -n 's/.*\] rust key \([0-9a-f][0-9a-f]*\) for .*/\1/p' "$swap_log" | head -1)
  swap_compile
  check "the same rustc hits" "$(hits)" "1"

  cp "$WORK/rustc-swap/rustc-b" "$WORK/rustc-swap/bin/rustc"
  chmod +x "$WORK/rustc-swap/bin/rustc"
  touch -d '+2 seconds' "$WORK/rustc-swap/bin/rustc"
  swap_compile
  check "a rebuilt rustc misses" "$(misses)" "2"
  key_after=$(sed -n 's/.*\] rust key \([0-9a-f][0-9a-f]*\) for .*/\1/p' "$swap_log" | tail -1)
  if [[ -n "$key_before" && "$key_before" != "$key_after" ]]; then
    ok "a rebuilt rustc logs a different rust key"
  else
    bad "a rebuilt rustc logs a different rust key (before ${key_before:-missing}, after ${key_after:-missing})"
  fi

  # Same banner at two paths with different mtimes. The cache key is the
  # banner, so the second checkout hits. mtime stays in the local memo.
  reset_cache
  for tree in rustc-share-a rustc-share-b; do
    mkdir -p "$WORK/$tree/src"
    cat > "$WORK/$tree/src/lib.rs" <<'EOF'
mod helper;
pub fn location() -> &'static str { file!() }
pub fn value() -> u32 { helper::value() }
EOF
    cat > "$WORK/$tree/src/helper.rs" <<'EOF'
pub fn value() -> u32 { 42 }
EOF
    cp "$WORK/rustc-swap/rustc-a" "$WORK/$tree/rustc"
    chmod +x "$WORK/$tree/rustc"
  done
  touch -d '2020-01-01 00:00:00' "$WORK/rustc-share-a/rustc"
  touch -d '2024-06-01 00:00:00' "$WORK/rustc-share-b/rustc"
  ( cd "$WORK/rustc-share-a" && VCACHE_ROOTS="$WORK/rustc-share-a=crate" \
      "$VCACHE" "$WORK/rustc-share-a/rustc" --crate-name demo --crate-type lib \
      -C debuginfo=2 --emit=dep-info,link --out-dir "$WORK/rustc-share-a/out" src/lib.rs ) \
      >/dev/null
  check "the first copy of one rustc banner misses" "$(misses)" "1"
  ( cd "$WORK/rustc-share-b" && VCACHE_ROOTS="$WORK/rustc-share-b=crate" \
      "$VCACHE" "$WORK/rustc-share-b/rustc" --crate-name demo --crate-type lib \
      -C debuginfo=2 --emit=dep-info,link --out-dir "$WORK/rustc-share-b/out" src/lib.rs ) \
      >/dev/null
  check "two rustc copies with one banner share an entry" "$(hits)" "1"
fi

# --------------------------------------------------------------------------
section "9f. Concurrent manifest stores keep both states"

# Two worktrees load the same manifest, each adds its own state, and the
# second store must not drop the first. The fifo holds one of them still
# until the other has stored.
if ! command -v rustc >/dev/null 2>&1; then
  skipped "rustc not installed"
else
  reset_cache
  man_tree() {  # $1 dir, $2 helper return value
    mkdir -p "$1/src"
    cat > "$1/src/lib.rs" << 'EOF'
mod helper;
pub fn v() -> u32 { helper::v() }
EOF
    printf 'pub fn v() -> u32 { %s }\n' "$2" > "$1/src/helper.rs"
  }
  man_tree "$WORK/man-a" 1
  man_tree "$WORK/man-b" 2
  man_tree "$WORK/man-c" 3
  man_compile() {  # $1 dir, $2 log (may be empty)
    # An expanded VAR=value is a command word, not an assignment, so the log
    # path has to be a literal prefix when it is set.
    if [[ -n "$2" ]]; then
      ( cd "$1" && VCACHE_ROOTS="$1=crate" VCACHE_LOG="$2" \
          "$VCACHE" rustc --crate-name manmerge --crate-type lib \
          --emit=dep-info,link --out-dir "$1/out" src/lib.rs ) >/dev/null
    else
      ( cd "$1" && VCACHE_ROOTS="$1=crate" \
          "$VCACHE" rustc --crate-name manmerge --crate-type lib \
          --emit=dep-info,link --out-dir "$1/out" src/lib.rs ) >/dev/null
    fi
  }
  man_compile "$WORK/man-a" ""
  check "the first manifest version misses" "$(misses)" "1"
  fifo="$WORK/manifest-pause.fifo"
  mkfifo "$fifo"
  blog="$WORK/man-b-pause.log"
  rm -f "$blog"
  ( cd "$WORK/man-b" && VCACHE_ROOTS="$WORK/man-b=crate" VCACHE_LOG="$blog" \
      VCACHE_TEST_PAUSE_BEFORE_MANIFEST_PUT="$fifo" \
      "$VCACHE" rustc --crate-name manmerge --crate-type lib \
      --emit=dep-info,link --out-dir "$WORK/man-b/out" src/lib.rs ) >/dev/null &
  bpid=$!
  waiting=0
  for _ in $(seq 1 200); do
    if grep -q 'manifest: waiting before put' "$blog" 2>/dev/null; then waiting=1; break; fi
    if ! kill -0 "$bpid" 2>/dev/null; then break; fi
    sleep 0.05
  done
  check "the second manifest store waits" "$waiting" "1"
  if [[ "$waiting" == 1 ]]; then
    man_compile "$WORK/man-c" ""
  fi
  release_pause_fifo "$fifo" "$bpid" "$waiting"
  for _ in $(seq 1 200); do
    if ! kill -0 "$bpid" 2>/dev/null; then break; fi
    sleep 0.05
  done
  wait "$bpid" || true
  blog2="$WORK/man-b-hit.log"
  rm -f "$blog2"
  ( cd "$WORK/man-b" && VCACHE_ROOTS="$WORK/man-b=crate" VCACHE_LOG="$blog2" \
      "$VCACHE" rustc --crate-name manmerge --crate-type lib \
      --emit=dep-info,link --out-dir "$WORK/man-b/out" src/lib.rs ) >/dev/null
  check "version 2 hits through the manifest" \
    "$(grep -c 'rust manifest hit' "$blog2" || true)" "1"
  check "version 2 does not run dep-info" "$(grep -c 'rust dep-info:' "$blog2" || true)" "0"
  blog3="$WORK/man-c-hit.log"
  rm -f "$blog3"
  ( cd "$WORK/man-c" && VCACHE_ROOTS="$WORK/man-c=crate" VCACHE_LOG="$blog3" \
      "$VCACHE" rustc --crate-name manmerge --crate-type lib \
      --emit=dep-info,link --out-dir "$WORK/man-c/out" src/lib.rs ) >/dev/null
  check "version 3 hits through the manifest" \
    "$(grep -c 'rust manifest hit' "$blog3" || true)" "1"
  check "version 3 does not run dep-info" "$(grep -c 'rust dep-info:' "$blog3" || true)" "0"
fi

# --------------------------------------------------------------------------
section "9b. Rust crates that read the environment"

# rustc lists each variable read by env!/option_env! as a "# env-dep:" line in
# its dep-info. Those are key inputs, not source files to hash.
if command -v rustc >/dev/null 2>&1; then
  reset_cache
  for tree in rust-env-a rust-env-b; do
    mkdir -p "$WORK/$tree/src" "$WORK/$tree/out"
    cat > "$WORK/$tree/src/lib.rs" <<'EOF'
pub const COMMIT: Option<&str> = option_env!("DEMO_UNSET");
pub const SET: &str = env!("DEMO_SET");
EOF
  done
  envlog="$WORK/rust-env.log"
  rust_env() {  # $1 = tree; the caller's environment decides DEMO_SET/DEMO_UNSET
    ( cd "$WORK/$1" && VCACHE_ROOTS="$WORK/$1=envcrate" VCACHE_LOG="$envlog" \
        "$VCACHE" rustc --crate-name demo --edition 2021 --crate-type lib \
        --emit=dep-info,link --out-dir "$WORK/$1/out" src/lib.rs ) 2>/dev/null
  }

  : > "$envlog"
  DEMO_SET=1 rust_env rust-env-a
  check "env-reading crate is a miss, not a passthrough" "$(misses)" "1"
  check "env deps are not hashed as sources" \
    "$(grep -c 'could not read source' "$envlog")" "0"
  check "env deps are logged as env deps" \
    "$(grep -c 'rust env-dep DEMO_UNSET (unset)' "$envlog")" "1"
  check "restored dep-info keeps the env-dep lines for cargo" \
    "$(grep -cx '# env-dep:DEMO_SET=1' "$WORK/rust-env-a/out/demo.d")" "1"
  cp "$WORK/rust-env-a/out/libdemo.rlib" "$WORK/rust-env-a.rlib"

  DEMO_SET=1 rust_env rust-env-a
  check "same environment hits" "$(hits)" "1"

  DEMO_SET=2 rust_env rust-env-a
  check "a changed value misses" "$(misses)" "2"

  DEMO_SET=1 DEMO_UNSET=x rust_env rust-env-a
  check "setting an unset variable misses" "$(misses)" "3"

  DEMO_SET=1 rust_env rust-env-b
  check "a second directory hits" "$(hits)" "2"
  if cmp -s "$WORK/rust-env-a.rlib" "$WORK/rust-env-b/out/libdemo.rlib"; then
    ok "env-reading rlibs are byte-identical across directories"
  else
    bad "env-reading rlibs are byte-identical across directories"
  fi
else
  printf '  \033[33mSKIP\033[0m rustc not installed\n'
fi

# --------------------------------------------------------------------------
section "9b2. Rust: path-valued env deps named in VCACHE_RUST_PATH_ENV_VARS"

# A crate with a build script include!s generated code from OUT_DIR, which cargo
# points into each checkout's own target directory. Listed, the value is keyed
# canonically and the crate hits from another checkout; a crate that bakes the
# value into its artifact is never stored.
if command -v rustc >/dev/null 2>&1; then
  reset_cache
  for tree in rust-out-a rust-out-b; do
    mkdir -p "$WORK/$tree/src" "$WORK/$tree/target/out" "$WORK/$tree/deps"
    echo 'pub fn answer() -> u32 { 42 }' > "$WORK/$tree/target/out/gen.rs"
    echo 'include!(concat!(env!("OUT_DIR"), "/gen.rs"));' > "$WORK/$tree/src/lib.rs"
    echo 'pub const DIR: &str = env!("OUT_DIR");' > "$WORK/$tree/src/baked.rs"
  done
  outlog="$WORK/rust-out.log"
  # rust_out TREE SOURCE [env assignments...]
  rust_out() {
    local tree=$1 source=$2; shift 2
    ( cd "$WORK/$tree" && env OUT_DIR="$WORK/$tree/target/out" VCACHE_LOG="$outlog" \
        VCACHE_ROOTS="$WORK/$tree=crate:$WORK/$tree/target=target" "$@" \
        "$VCACHE" rustc --crate-name "$(basename "$source" .rs)" --edition 2021 \
        --crate-type lib --emit=dep-info,link --out-dir "$WORK/$tree/deps" "src/$source" ) 2>/dev/null
  }

  rust_out rust-out-a lib.rs
  rust_out rust-out-b lib.rs
  check "unlisted, OUT_DIR keeps another checkout from hitting" "$(misses)" "2"

  reset_cache
  rust_out rust-out-a lib.rs VCACHE_RUST_PATH_ENV_VARS=OUT_DIR
  rust_out rust-out-b lib.rs VCACHE_RUST_PATH_ENV_VARS=OUT_DIR
  check "listed, OUT_DIR hits from another checkout" "$(hits)" "1"
  if cmp -s "$WORK/rust-out-a/deps/liblib.rlib" "$WORK/rust-out-b/deps/liblib.rlib"; then
    ok "the served rlib is byte-identical to the compiled one"
  else
    bad "the served rlib is byte-identical to the compiled one"
  fi
  check "the restored dep-info names this checkout's OUT_DIR for cargo" \
    "$(grep -cx "# env-dep:OUT_DIR=$WORK/rust-out-b/target/out" "$WORK/rust-out-b/deps/lib.d")" "1"
  check "and its generated source" \
    "$(grep -c "^$WORK/rust-out-b/target/out/gen.rs:" "$WORK/rust-out-b/deps/lib.d")" "1"
  rust_out rust-out-b lib.rs VCACHE_RUST_PATH_ENV_VARS=OUT_DIR
  check "the restored checkout then hits through the manifest" "$(hits)" "2"

  reset_cache
  rust_out rust-out-a baked.rs VCACHE_RUST_PATH_ENV_VARS=OUT_DIR
  check "a crate that bakes OUT_DIR in is not stored" \
    "$("$VCACHE" --show-stats | sed -n 's/^ *env path in output *//p')" "1"
  rust_out rust-out-b baked.rs VCACHE_RUST_PATH_ENV_VARS=OUT_DIR
  check "so another checkout compiles its own" "$(misses)" "2"
  check "and its rlib holds its own path" \
    "$(grep -qa "$WORK/rust-out-b/target/out" "$WORK/rust-out-b/deps/libbaked.rlib" && echo yes)" "yes"
else
  printf '  \033[33mSKIP\033[0m rustc not installed\n'
fi

# --------------------------------------------------------------------------
section "9c. Rust: the incremental directory is not part of the key"

# cargo passes -C incremental=<target-dir>/<profile>/incremental to every
# workspace crate, so two target directories are two checkouts here. Only the
# crate is under a root; the incremental directories are not.
if command -v rustc >/dev/null 2>&1; then
  reset_cache
  mkdir -p "$WORK/rust-inc/src"
  cat > "$WORK/rust-inc/src/lib.rs" <<'EOF'
pub fn twice(x: u32) -> u32 { x * 2 }
pub fn label(x: u32) -> String { format!("value {x}") }
EOF
  # rust_inc TARGET [extra rustc args...]
  rust_inc() {
    local target=$1; shift
    ( cd "$WORK/rust-inc" && VCACHE_ROOTS="$WORK/rust-inc=crate" \
        VCACHE_LOG="$WORK/$target.log" \
        "$VCACHE" rustc --crate-name inc --crate-type lib -C debuginfo=2 \
        --emit=dep-info,metadata,link --out-dir "$WORK/$target/deps" "$@" \
        src/lib.rs ) 2>/dev/null
  }
  rust_key_of() { sed -n 's/.*rust key \([0-9a-f]*\) for .*/\1/p' "$WORK/$1.log" | head -1; }

  rust_inc inc-t1 -C "incremental=$WORK/inc-t1/incremental"
  check "first incremental compile is a miss" "$(misses)" "1"
  check "the miss leaves rustc's incremental state in its own directory" \
    "$([[ -n "$(ls -A "$WORK/inc-t1/incremental" 2>/dev/null)" ]] && echo yes)" "yes"

  rust_inc inc-t2 -Cincremental="$WORK/inc-t2/incremental"
  check "a different incremental directory still hits" "$(hits)" "1"
  check "the key ignores the incremental directory" \
    "$(rust_key_of inc-t2)" "$(rust_key_of inc-t1)"
  for artifact in libinc.rlib libinc.rmeta; do
    if cmp -s "$WORK/inc-t1/deps/$artifact" "$WORK/inc-t2/deps/$artifact"; then
      ok "restored $artifact is byte-identical to the compiled one"
    else
      bad "restored $artifact is byte-identical to the compiled one"
    fi
  done
  check "restored dep-info matches the compiled one apart from its directory" \
    "$(sed "s#$WORK/inc-t1/#$WORK/inc-t2/#g" "$WORK/inc-t1/deps/inc.d")" \
    "$(cat "$WORK/inc-t2/deps/inc.d")"
  # The entry holds the --emit artifacts only; rustc's session state is never
  # captured, so a hit has none to restore.
  check "a hit restores exactly the emitted artifacts" \
    "$(cd "$WORK/inc-t2/deps" && find . -mindepth 1 | sort | tr '\n' ' ')" \
    "./inc.d ./libinc.rlib ./libinc.rmeta "
  check "a hit does not create the incremental directory" \
    "$([[ -e "$WORK/inc-t2/incremental" ]] && echo exists || echo absent)" "absent"

  # Incremental mode raises rustc's default codegen-unit count, which changes
  # the objects, so whether it is on stays in the key: a non-incremental compile
  # of the same crate gets its own entry.
  rust_inc inc-t3
  check "a non-incremental compile does not share the incremental entry" "$(misses)" "2"
  if [[ -n "$(rust_key_of inc-t3)" && "$(rust_key_of inc-t3)" != "$(rust_key_of inc-t1)" ]]; then
    ok "the key records whether incremental is on"
  else
    bad "the key records whether incremental is on"
  fi
else
  skipped "rustc not installed"
fi

# --------------------------------------------------------------------------
section "9d. Rust: manifest-verified lookups skip the dep-info run"

# rustc's dep-info run expands every macro, so on a hit it can cost more than
# everything else vcache does. A remembered state whose files, env values and
# extern digests all still match answers the lookup without it.
if command -v rustc >/dev/null 2>&1; then
  reset_cache
  for tree in rust-man-a rust-man-b; do
    mkdir -p "$WORK/$tree/src" "$WORK/$tree/out"
    cat > "$WORK/$tree/src/lib.rs" <<'EOF'
mod helper;
pub const DATA: &str = include_str!("data.txt");
pub const COMMIT: Option<&str> = option_env!("DEMO_UNSET");
pub fn value() -> u32 { helper::value() }
EOF
    echo 'pub fn value() -> u32 { 42 }' > "$WORK/$tree/src/helper.rs"
    echo 'hello' > "$WORK/$tree/src/data.txt"
  done
  manlog="$WORK/rust-man.log"
  rust_man() {  # $1 = tree; each run gets a fresh log
    : > "$manlog"
    ( cd "$WORK/$1" && VCACHE_ROOTS="$WORK/$1=mancrate" VCACHE_LOG="$manlog" \
        "$VCACHE" rustc --crate-name man --edition 2021 --crate-type lib \
        --emit=dep-info,link --out-dir "$WORK/$1/out" src/lib.rs ) 2>/dev/null
  }
  dep_info_runs() { grep -c 'rust dep-info:' "$manlog"; }
  logged() { grep -cF "$1" "$manlog"; }

  rust_man rust-man-a
  check "first manifest-mode compile is a miss" "$(misses)" "1"
  check "a first compile logs none stored once" "$(logged 'rust manifest: none stored')" "1"
  check "a miss asks rustc for dep-info" "$(dep_info_runs)" "1"
  check "the miss records one state" "$(logged 'rust manifest: stored 1 states')" "1"
  cp "$WORK/rust-man-a/out/libman.rlib" "$WORK/rust-man-a.rlib"

  rust_man rust-man-a
  check "an unchanged crate hits" "$(hits)" "1"
  check "the hit skips the dep-info run" "$(dep_info_runs)" "0"
  check "the hit names the state that matched" "$(logged 'rust manifest hit: state 1 of 1')" "1"

  echo 'pub fn value() -> u32 { 43 }' > "$WORK/rust-man-a/src/helper.rs"
  rust_man rust-man-a
  check "an edited module misses" "$(misses)" "2"
  check "the rejection names the edited module" \
    "$(logged 'state 1 of 1 rejected: src/helper.rs changed')" "1"
  check "the manifest now holds two states" "$(logged 'rust manifest: stored 2 states')" "1"

  echo 'pub fn value() -> u32 { 42 }' > "$WORK/rust-man-a/src/helper.rs"
  rust_man rust-man-a
  check "reverting the module hits" "$(hits)" "2"
  check "the reverted hit skips the dep-info run" "$(dep_info_runs)" "0"
  check "the reverted hit uses the second state" "$(logged 'rust manifest hit: state 2 of 2')" "1"

  echo 'changed' > "$WORK/rust-man-a/src/data.txt"
  rust_man rust-man-a
  check "an edited include_str! file misses" "$(misses)" "3"
  check "the rejection names the included file" \
    "$(logged 'state 1 of 2 rejected: src/data.txt changed')" "1"
  echo 'hello' > "$WORK/rust-man-a/src/data.txt"

  DEMO_UNSET=x rust_man rust-man-a
  check "setting a variable the crate reads misses" "$(misses)" "4"
  check "the rejection names the variable" \
    "$(logged "state 1 of 3 rejected: env DEMO_UNSET is 'x', was unset")" "1"

  VCACHE_RUST_DEP_INFO=always rust_man rust-man-a
  check "rust_dep_info=always still hits" "$(hits)" "3"
  check "rust_dep_info=always runs dep-info" "$(dep_info_runs)" "1"
  check "rust_dep_info=always leaves the manifest alone" "$(logged 'rust manifest')" "0"

  rust_man rust-man-b
  check "a second directory hits" "$(hits)" "4"
  check "the second directory skips the dep-info run" "$(dep_info_runs)" "0"
  if cmp -s "$WORK/rust-man-a.rlib" "$WORK/rust-man-b/out/libman.rlib"; then
    ok "the manifest hit restores a byte-identical rlib"
  else
    bad "the manifest hit restores a byte-identical rlib"
  fi
  check "show-config reports the policy" \
    "$(VCACHE_RUST_DEP_INFO=always "$VCACHE" --show-config | grep -c 'rust dep-info: *always')" "1"
else
  skipped "rustc not installed"
fi

# --------------------------------------------------------------------------
section "9d. Cost records"

mkdir -p "$WORK/cost-src"
cat > "$WORK/cost-src/t.cc" << 'EOF'
int cost_probe() { return 1; }
EOF
reset_cache
COST_LOG="$WORK/cost.log"
rm -f "$COST_LOG"
( cd "$WORK/cost-src" && VCACHE_ROOTS="$WORK/cost-src=proj" VCACHE_LOG="$COST_LOG" \
    "$VCACHE" g++ -c t.cc -o "$WORK/cost.o" )
check "cost compile is a miss" "$(misses)" "1"

cost_key=$(sed -n 's/.*\] key \([0-9a-f][0-9a-f]*\) for .*/\1/p' "$COST_LOG" | head -1)
cost_entry="$VCACHE_DIR/${cost_key:0:2}/${cost_key:2}"
cost_rss=$(grep -a -o 'max_rss_kb: [0-9][0-9]*' "$cost_entry" 2>/dev/null | head -1 | awk '{print $2}')
if [[ -n "${cost_rss:-}" && "$cost_rss" -gt 0 ]]; then
  ok "blob meta carries max_rss_kb > 0"
else
  bad "blob meta carries max_rss_kb > 0 (key=${cost_key:-missing} rss=${cost_rss:-missing})"
fi
if grep -a -q 'wall_ms: [0-9]' "$cost_entry" 2>/dev/null; then
  ok "blob meta carries wall_ms"
else
  bad "blob meta carries wall_ms"
fi

cost_show=$("$VCACHE" --show-costs)
if printf '%s\n' "$cost_show" | grep -q '^compile records 1 '; then
  ok "--show-costs lists the compile with records 1"
else
  bad "--show-costs lists the compile with records 1"
  printf '%s\n' "$cost_show" | sed 's/^/         /'
fi
cost_lines=$(grep -c 'cost: compile' "$COST_LOG" || true)
check "exactly one cost: compile line on a miss" "$cost_lines" "1"
cost_any=$(grep -c 'cost: ' "$COST_LOG" || true)
check "the preprocess probe is not recorded as a cost" "$cost_any" "1"

( cd "$WORK/cost-src" && VCACHE_ROOTS="$WORK/cost-src=proj" VCACHE_LOG="$COST_LOG" \
    "$VCACHE" g++ -c t.cc -o "$WORK/cost.o" )
check "the second cost compile hits" "$(hits)" "1"
cost_lines=$(grep -c 'cost: compile' "$COST_LOG" || true)
check "a hit adds no cost line" "$cost_lines" "1"

cat > "$WORK/cost-src/bad.cc" << 'EOF'
int broken() { return
EOF
COST_FAIL_LOG="$WORK/cost-fail.log"
rm -f "$COST_FAIL_LOG"
( cd "$WORK/cost-src" && VCACHE_ROOTS="$WORK/cost-src=proj" VCACHE_LOG="$COST_FAIL_LOG" \
    "$VCACHE" g++ -c bad.cc -o "$WORK/cost-bad.o" ) >/dev/null 2>&1 || true
cost_fail=$(grep -c 'cost: compile .* exit=' "$COST_FAIL_LOG" || true)
check "a failed compile is still recorded, with its exit" "$cost_fail" "1"
cost_fail_any=$(grep -c 'cost: ' "$COST_FAIL_LOG" || true)
check "a failed compile records exactly one cost line" "$cost_fail_any" "1"

mkdir -p "$WORK/cost-die"
gxx=$(command -v g++)
cat > "$WORK/cost-die/cc" << EOF
#!/bin/sh
for arg in "\$@"; do
  if [ "\$arg" = "-E" ]; then
    exec $gxx "\$@"
  fi
done
kill -ABRT \$\$
EOF
chmod +x "$WORK/cost-die/cc"
cat > "$WORK/cost-die/t.cc" << 'EOF'
int live() { return 1; }
EOF
COST_SIG_LOG="$WORK/cost-signal.log"
rm -f "$COST_SIG_LOG"
( cd "$WORK/cost-die" && VCACHE_COMPILER_CHECK=mtime VCACHE_ROOTS="$WORK/cost-die=proj" \
    VCACHE_LOG="$COST_SIG_LOG" \
    "$VCACHE" "$WORK/cost-die/cc" -c t.cc -o "$WORK/cost-die.o" ) >/dev/null 2>&1 || true
cost_sig=$(grep -c 'cost: compile .* exit=' "$COST_SIG_LOG" || true)
check "a compiler killed by a signal is still recorded, with its exit" "$cost_sig" "1"

if command -v rustc >/dev/null 2>&1; then
  reset_cache
  mkdir -p "$WORK/cost-rust/src"
  cat > "$WORK/cost-rust/src/lib.rs" << 'EOF'
pub fn cost_rust() -> u32 { 1 }
EOF
  RLOG="$WORK/cost-rust.log"
  rm -f "$RLOG"
  ( cd "$WORK/cost-rust" && VCACHE_ROOTS="$WORK/cost-rust=crate" VCACHE_LOG="$RLOG" \
      "$VCACHE" rustc --crate-name costrust --crate-type lib \
      --emit=dep-info,link --out-dir "$WORK/cost-rust/out" src/lib.rs ) >/dev/null
  check "a rustc miss logs exactly one cost: rustc line" \
    "$(grep -c 'cost: rustc' "$RLOG" || true)" "1"
  check "the rust dep-info run is not a cost record" \
    "$(grep -c 'cost: ' "$RLOG" || true)" "1"
  check "the rust miss did run dep-info" \
    "$(grep -c 'rust dep-info:' "$RLOG" || true)" "1"
  rkey=$(sed -n 's/.*\] rust key \([0-9a-f][0-9a-f]*\) for .*/\1/p' "$RLOG" | head -1)
  rentry="$VCACHE_DIR/${rkey:0:2}/${rkey:2}"
  if grep -a -q 'max_rss_kb: [0-9]' "$rentry" 2>/dev/null; then
    ok "a rustc blob meta carries max_rss_kb"
  else
    bad "a rustc blob meta carries max_rss_kb"
  fi
  ( cd "$WORK/cost-rust" && VCACHE_ROOTS="$WORK/cost-rust=crate" VCACHE_LOG="$RLOG" \
      "$VCACHE" rustc --crate-name costrust --crate-type lib \
      --emit=dep-info,link --out-dir "$WORK/cost-rust/out" src/lib.rs ) >/dev/null
  check "a rust hit adds no cost line" "$(grep -c 'cost: rustc' "$RLOG" || true)" "1"
  ( cd "$WORK/cost-rust" && VCACHE_RUST_DEP_INFO=always \
      VCACHE_ROOTS="$WORK/cost-rust=crate" VCACHE_LOG="$RLOG" \
      "$VCACHE" rustc --crate-name costrust --crate-type lib \
      --emit=dep-info,link --out-dir "$WORK/cost-rust/out" src/lib.rs ) >/dev/null
  check "a rust key hit adds no cost line" "$(grep -c 'cost: rustc' "$RLOG" || true)" "1"
  check "the always policy hits on the rust key" "$(hits)" "2"
else
  skipped "rustc not installed"
fi

if [[ "$(uname -s)" == Linux ]]; then
  reset_cache
  mkdir -p "$WORK/cost-link"
  printf 'int main(void){return 0;}\n' > "$WORK/cost-link/main.c"
  gcc -c "$WORK/cost-link/main.c" -o "$WORK/cost-link/main.o"
  LLOG="$WORK/cost-link.log"
  rm -f "$LLOG"
  ( cd "$WORK/cost-link" && VCACHE_LINK_CACHE=1 VCACHE_ROOTS="$WORK/cost-link=proj" \
      VCACHE_LOG="$LLOG" "$VCACHE" gcc main.o -o main )
  check "a link logs exactly one cost: link line" \
    "$(grep -c 'cost: link' "$LLOG" || true)" "1"
else
  skipped "link cost records are Linux-only"
fi

# --------------------------------------------------------------------------
section "10. cache management commands"

reset_cache
( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
    "$VCACHE" g++ -c -I include src/lib.cc -o "$WORK/z.o" ) 2>/dev/null
"$VCACHE" --clear >/dev/null
check "counters are zero after --clear" "$(misses)" "0"
( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
    "$VCACHE" g++ -c -I include src/lib.cc -o "$WORK/z.o" ) 2>/dev/null
check "compile after --clear misses again" "$(misses)" "1"
"$VCACHE" --zero-stats >/dev/null
check "--zero-stats resets counters" "$(misses)" "0"

# VCACHE_DISABLE must bypass the cache but still compile.
reset_cache
( cd "$WORK/checkout-a" && VCACHE_DISABLE=1 \
    "$VCACHE" g++ -c -I include src/lib.cc -o "$WORK/dis.o" ) 2>/dev/null
check "disabled compile still produces an object" "$([[ -f "$WORK/dis.o" ]] && echo yes)" "yes"
check "disabled compile records no lookups" "$(misses)" "0"

# A global cache budget must not behave like 256 independent tiny budgets.
# Keep several entries concentrated in one shard and verify an explicit trim
# leaves them alone while their aggregate size remains below the configured
# global limit.
reset_cache
mkdir -p "$VCACHE_DIR/aa"
for n in 1 2 3; do
  dd if=/dev/zero of="$VCACHE_DIR/aa/entry$n" bs=1024 count=1 status=none
done
VCACHE_CACHE_SIZE=10K "$VCACHE" --trim >/dev/null
check "global trim preserves a skewed shard below the total budget" \
  "$(find "$VCACHE_DIR/aa" -type f | wc -l | tr -d " ")" "3"

# --------------------------------------------------------------------------
section "11. S3 layer (against a mock object store)"

if command -v python3 >/dev/null 2>&1; then
  S3PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
  S3DIR="$WORK/s3-objects"
  python3 "$TOP/tests/mock_s3.py" "$S3PORT" "$S3DIR" &
  S3PID=$!
  # Wait for the port to accept connections.
  for _ in $(seq 1 50); do
    python3 -c "
import socket,sys
s=socket.socket()
try: s.connect(('127.0.0.1',$S3PORT)); sys.exit(0)
except Exception: sys.exit(1)
" 2>/dev/null && break
    sleep 0.1
  done

  export AWS_ACCESS_KEY_ID=AKIDEXAMPLE
  export AWS_SECRET_ACCESS_KEY=wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY
  export VCACHE_S3_BUCKET=testbucket
  export VCACHE_S3_ENDPOINT="http://127.0.0.1:$S3PORT"
  export VCACHE_S3_PATH_STYLE=1
  export VCACHE_S3_REGION=us-east-1

  reset_cache
  ( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
      "$VCACHE" g++ -g -O2 -c -I include src/lib.cc -o "$WORK/s1.o" ) 2>/dev/null
  check "compile with s3 enabled is a miss" "$(misses)" "1"
  objects_written=$(find "$S3DIR" -type f 2>/dev/null | wc -l | tr -d " ")
  check "entry was uploaded to s3" "$objects_written" "1"
  # One store, both layers. Asserted before the local layer is wiped below,
  # since that would otherwise destroy the only evidence -- and the disk hit
  # further down proves backfill, which is a different path entirely.
  check "the same store also wrote the local layer" "$(disk_entries)" "1"

  # Drop the local layer entirely: the next lookup must be served by S3.
  rm -rf "$VCACHE_DIR"
  ( cd "$WORK/checkout-b" && VCACHE_ROOTS="$WORK/checkout-b=proj" \
      "$VCACHE" g++ -g -O2 -c -I include src/lib.cc -o "$WORK/s2.o" ) 2>/dev/null
  check "remote hit with an empty local cache" "$(stat_of 'cache hit (s3)')" "1"
  if cmp -s "$WORK/s1.o" "$WORK/s2.o"; then
    ok "object restored from s3 is byte-identical"
  else
    bad "object restored from s3 is byte-identical"
  fi

  # The remote hit should have been promoted into the local layer.
  ( cd "$WORK/checkout-b" && VCACHE_ROOTS="$WORK/checkout-b=proj" \
      "$VCACHE" g++ -g -O2 -c -I include src/lib.cc -o "$WORK/s3.o" ) 2>/dev/null
  check "backfilled entry now serves from disk" "$(hits)" "1"

  # --- ttl ------------------------------------------------------------------
  #
  # Age is taken from the object's Last-Modified, so backdating the stored file
  # is what an expired entry looks like from vcache's side.
  # GNU touch accepts -d with a relative date; BSD's does not, and neither
  # spelling of `find -newermt` is portable either. python3 is already a
  # prerequisite for this whole section, and setting an mtime through it is
  # exact and the same everywhere. Takes an age in seconds; `only_recent`
  # restricts it to objects written in the last minute, which is how the
  # eviction test gives its three entries distinct ages.
  backdate() {
    python3 - "$S3DIR" "$1" "${2:-all}" <<'BACKDATE'
import os, sys, time
root, ago, scope = sys.argv[1], float(sys.argv[2]), sys.argv[3]
now = time.time()
for dirpath, _, names in os.walk(root):
    for n in names:
        p = os.path.join(dirpath, n)
        if scope == "only_recent" and now - os.stat(p).st_mtime > 60:
            continue
        os.utime(p, (now - ago, now - ago))
BACKDATE
  }
  day=86400

  reset_cache
  backdate "$((40 * day))"
  ( cd "$WORK/checkout-b" && VCACHE_ROOTS="$WORK/checkout-b=proj" \
      "$VCACHE" g++ -g -O2 -c -I include src/lib.cc -o "$WORK/s5.o" ) 2>/dev/null
  check "an entry past its ttl is not served" "$(stat_of 'cache hit (s3)')" "0"
  check "and is counted as a miss instead" "$(misses)" "1"

  # The same object inside the window still serves, so the previous check is
  # about age and not about the object having become unreadable.
  reset_cache
  backdate "$((2 * day))"
  ( cd "$WORK/checkout-b" && VCACHE_ROOTS="$WORK/checkout-b=proj" \
      "$VCACHE" g++ -g -O2 -c -I include src/lib.cc -o "$WORK/s6.o" ) 2>/dev/null
  check "an entry inside the ttl still serves" "$(stat_of 'cache hit (s3)')" "1"

  # A ttl of zero disables the check rather than expiring everything.
  reset_cache
  backdate "$((400 * day))"
  ( cd "$WORK/checkout-b" && VCACHE_ROOTS="$WORK/checkout-b=proj" \
      VCACHE_S3_TTL_DAYS=0 \
      "$VCACHE" g++ -g -O2 -c -I include src/lib.cc -o "$WORK/s7.o" ) 2>/dev/null
  check "ttl=0 disables expiry" "$(stat_of 'cache hit (s3)')" "1"

  # --- trim -----------------------------------------------------------------

  before=$(find "$S3DIR" -type f | wc -l | tr -d " ")
  check "one object present before trimming" "$before" "1"

  # Nothing to enforce: no ttl and no cap must leave the bucket alone.
  VCACHE_S3_TTL_DAYS=0 "$VCACHE" --trim >/dev/null 2>&1
  check "trim with nothing configured deletes nothing" "$(find "$S3DIR" -type f | wc -l | tr -d " ")" "1"

  # Expired by age.
  backdate "$((40 * day))"
  "$VCACHE" --trim > "$WORK/trim.out" 2>&1
  check "trim deletes an expired object" "$(find "$S3DIR" -type f | wc -l | tr -d " ")" "0"
  if grep -q 'deleted 1 expired' "$WORK/trim.out"; then
    ok "trim reports what it deleted"
  else
    bad "trim reports what it deleted"
  fi

  # Evicted by the byte budget, oldest first, and across a paged listing:
  # MOCK_S3_PAGE_SIZE=1 forces vcache to follow continuation tokens.
  reset_cache
  for n in 1 2 3; do
    ( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
        "$VCACHE" g++ -g -O$n -c -I include src/lib.cc -o "$WORK/t$n.o" ) 2>/dev/null
    # Distinct ages so eviction order is well defined.
    backdate "$(( (4 - n) * day ))" only_recent
  done
  check "three distinct entries stored" "$(find "$S3DIR" -type f | wc -l | tr -d " ")" "3"

  kill "$S3PID" 2>/dev/null; wait "$S3PID" 2>/dev/null
  MOCK_S3_PAGE_SIZE=1 python3 "$TOP/tests/mock_s3.py" "$S3PORT" "$S3DIR" &
  S3PID=$!
  for _ in $(seq 1 50); do
    python3 -c "
import socket,sys
s=socket.socket()
try: s.connect(('127.0.0.1',$S3PORT)); sys.exit(0)
except Exception: sys.exit(1)
" 2>/dev/null && break
    sleep 0.1
  done

  # A cap just under the total forces at least one eviction; the 80% target
  # means it keeps going past the cap itself.
    # find -printf is GNU's; concatenating and counting bytes is exact everywhere.
  total_bytes=$(find "$S3DIR" -type f -exec cat {} + | wc -c | tr -d ' ')
  VCACHE_S3_TTL_DAYS=0 VCACHE_S3_CACHE_SIZE="$((total_bytes - 1))" \
    "$VCACHE" --trim > "$WORK/trim2.out" 2>&1
  after=$(find "$S3DIR" -type f | wc -l | tr -d " ")
  if [[ "$after" -lt 3 && "$after" -ge 1 ]]; then
    ok "trim evicts down to the byte budget across a paged listing"
  else
    bad "trim evicts down to the byte budget across a paged listing (left $after of 3)"
  fi
  if grep -qE 'listed 3 objects' "$WORK/trim2.out"; then
    ok "trim followed the continuation tokens to see every object"
  else
    bad "trim followed the continuation tokens to see every object"
  fi

  # An unreachable remote must degrade to a miss, never break the build.
  kill "$S3PID" 2>/dev/null; wait "$S3PID" 2>/dev/null
  rm -rf "$VCACHE_DIR"
  ( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
      "$VCACHE" g++ -O0 -c -I include src/lib.cc -o "$WORK/s4.o" ) 2>/dev/null
  rc=$?
  check "build succeeds when s3 is unreachable" "$rc" "0"

  # --- media failures are warned about, and can be made fatal ---------------
  #
  # The server is dead at this point, so every s3 request is a real transport
  # failure rather than a miss.
  reset_cache
  ( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
      "$VCACHE" g++ -O1 -c -I include src/lib.cc -o "$WORK/m1.o" ) 2>"$WORK/m1.err"
  rc=$?
  check "a broken cache layer still exits 0 by default" "$rc" "0"
  if grep -q 'warning: cache layer failed' "$WORK/m1.err"; then
    ok "and warns on stderr rather than failing silently"
  else
    bad "and warns on stderr rather than failing silently"
  fi
  # Both the lookup and the store should have been reported.
  check "media failures are counted" \
    "$([[ "$(stat_of 'cache media errors')" -ge 2 ]] && echo yes)" "yes"
  check "the object is still produced" "$([[ -s "$WORK/m1.o" ]] && echo yes)" "yes"

  reset_cache
  ( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
      "$VCACHE" --error-on-cache-media-failure \
      g++ -O1 -c -I include src/lib.cc -o "$WORK/m2.o" ) 2>/dev/null
  check "--error-on-cache-media-failure exits 90" "$?" "90"
  check "and still leaves a usable object behind" \
    "$([[ -s "$WORK/m2.o" ]] && echo yes)" "yes"

  reset_cache
  ( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
      VCACHE_ERROR_ON_CACHE_MEDIA_FAILURE=1 \
      "$VCACHE" g++ -O1 -c -I include src/lib.cc -o "$WORK/m3.o" ) 2>/dev/null
  check "the environment variable has the same effect" "$?" "90"

  # A cold cache must not be mistaken for a broken one: same flag, no s3.
  reset_cache
  ( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
      VCACHE_S3_BUCKET= AWS_ACCESS_KEY_ID= AWS_SECRET_ACCESS_KEY= \
      "$VCACHE" --error-on-cache-media-failure \
      g++ -O1 -c -I include src/lib.cc -o "$WORK/m4.o" ) 2>/dev/null
  check "a plain miss is not a media failure" "$?" "0"

  # --- the ListBucket diagnostic -------------------------------------------
  #
  # Restart the mock as a bucket that denies both the listing and any missing
  # object, which is how real S3 behaves without s3:ListBucket.
  MOCK_S3_NO_LISTBUCKET=1 python3 "$TOP/tests/mock_s3.py" "$S3PORT" "$S3DIR" &
  S3PID=$!
  for _ in $(seq 1 50); do
    python3 -c "
import socket,sys
s=socket.socket()
try: s.connect(('127.0.0.1',$S3PORT)); sys.exit(0)
except Exception: sys.exit(1)
" 2>/dev/null && break
    sleep 0.1
  done

  reset_cache
  ( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
      "$VCACHE" g++ -O2 -c -I include src/lib.cc -o "$WORK/lb1.o" ) 2>"$WORK/lb1.err"
  rc=$?
  check "a bucket without ListBucket still builds" "$rc" "0"
  if grep -q 'does not grant s3:ListBucket' "$WORK/lb1.err"; then
    ok "and says so"
  else
    bad "and says so"
  fi
  if grep -q 'assume_no_list_bucket' "$WORK/lb1.err"; then
    ok "and names the config key that silences it"
  else
    bad "and names the config key that silences it"
  fi
  # The 403 is still treated as an ordinary miss, not a media failure.
  check "a denied lookup on such a bucket is a miss, not an error" \
    "$(stat_of 'cache media errors')" "0"

  # The config-file key suppresses both the warning and the extra probe.
  cat > "$WORK/no-list.toml" <<TOML
[cache.s3]
assume_no_list_bucket = true
TOML
  reset_cache
  ( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
      VCACHE_CONFIG="$WORK/no-list.toml" \
      "$VCACHE" g++ -O2 -c -I include src/lib.cc -o "$WORK/lb2.o" ) 2>"$WORK/lb2.err"
  if grep -q 'does not grant s3:ListBucket' "$WORK/lb2.err"; then
    bad "assume_no_list_bucket silences the warning"
  else
    ok "assume_no_list_bucket silences the warning"
  fi

  # With ListBucket available, a 403 on the object means something really is
  # wrong, and must be reported rather than filed as a miss. Deleting the
  # object directory makes every GET a miss; the listing still succeeds.
  kill "$S3PID" 2>/dev/null; wait "$S3PID" 2>/dev/null
  python3 "$TOP/tests/mock_s3.py" "$S3PORT" "$S3DIR" &
  S3PID=$!
  for _ in $(seq 1 50); do
    python3 -c "
import socket,sys
s=socket.socket()
try: s.connect(('127.0.0.1',$S3PORT)); sys.exit(0)
except Exception: sys.exit(1)
" 2>/dev/null && break
    sleep 0.1
  done
  reset_cache
  ( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
      "$VCACHE" g++ -O2 -c -I include src/lib.cc -o "$WORK/lb3.o" ) 2>"$WORK/lb3.err"
  if grep -q 'does not grant s3:ListBucket' "$WORK/lb3.err"; then
    bad "a bucket that does grant ListBucket produces no warning"
  else
    ok "a bucket that does grant ListBucket produces no warning"
  fi

  kill "$S3PID" 2>/dev/null; wait "$S3PID" 2>/dev/null

  # A bucket shedding load answers 503 SlowDown. That is not a failed store,
  # it is a store to try again: without a retry the entry is lost, the compile
  # is counted as a cache media error, and the next build recompiles it.
  MOCK_S3_TRANSIENT_PUT_FAILURES=2 python3 "$TOP/tests/mock_s3.py" "$S3PORT" "$S3DIR" &
  S3PID=$!
  for _ in $(seq 1 50); do
    python3 -c "
import socket,sys
s=socket.socket()
try: s.connect(('127.0.0.1',$S3PORT)); sys.exit(0)
except Exception: sys.exit(1)
" 2>/dev/null && break
    sleep 0.1
  done
  rm -rf "$S3DIR"; mkdir -p "$S3DIR"
  reset_cache
  ( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
      "$VCACHE" g++ -O2 -c -I include src/lib.cc -o "$WORK/slow1.o" ) 2>"$WORK/slow1.err"
  check "a throttled store still produces the object" \
        "$([[ -s "$WORK/slow1.o" ]] && echo yes)" "yes"
  check "and reports no cache media error" \
        "$("$VCACHE" --show-stats | awk '/^cache media errors[[:space:]]/ { print $NF }')" "0"
  # The point of the retry: the entry actually reached the bucket, so a fresh
  # local cache can still hit it.
  reset_cache
  ( cd "$WORK/checkout-b" && VCACHE_ROOTS="$WORK/checkout-b=proj" \
      "$VCACHE" g++ -O2 -c -I include src/lib.cc -o "$WORK/slow2.o" ) 2>/dev/null
  check "and the retried store is readable from s3 afterwards" \
        "$("$VCACHE" --show-stats | awk '/^cache hit \(s3\)[[:space:]]/ { print $NF }')" "1"

  kill "$S3PID" 2>/dev/null; wait "$S3PID" 2>/dev/null

  # A compile that genuinely fails must keep reporting the compiler's status,
  # not vcache's, even with the flag on and s3 down.
  printf 'int main(){ return notdefined; }\n' > "$WORK/broken.c"
  ( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
      "$VCACHE" --error-on-cache-media-failure \
      g++ -c "$WORK/broken.c" -o "$WORK/m5.o" ) 2>/dev/null
  check "a compiler error still wins over the cache status" "$([[ $? -ne 0 && $? -ne 90 ]] && echo yes)" "yes"
  check "object still produced with s3 down" "$([[ -s "$WORK/s4.o" ]] && echo yes)" "yes"

  unset AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY VCACHE_S3_BUCKET \
        VCACHE_S3_ENDPOINT VCACHE_S3_PATH_STYLE VCACHE_S3_REGION
else
  printf '  \033[33mSKIP\033[0m python3 not installed\n'
fi

# --------------------------------------------------------------------------
section "11b. cache daemon"

# Every daemon here is stopped explicitly; the short idle timeout is a backstop
# so a failed assertion cannot leave one running long after the suite.
export VCACHE_DAEMON_IDLE_TIMEOUT=60

daemon_stat() { "$VCACHE" --daemon-status 2>/dev/null | grep -F "$1" | head -1 | awk '{print $NF}'; }
daemon_pid() { "$VCACHE" --daemon-status 2>/dev/null | awk '/^daemon pid/ {print $NF}'; }
compile_a() {
  ( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
      "$VCACHE" g++ -O2 -c -I include src/lib.cc -o "$1" ) 2>/dev/null
}
compile_b() {
  ( cd "$WORK/checkout-b" && VCACHE_ROOTS="$WORK/checkout-b=proj" \
      "$VCACHE" g++ -O2 -c -I include src/lib.cc -o "$1" ) 2>/dev/null
}

# Runs a command with a deadline, reporting "timeout" instead of hanging the
# suite. macOS has no timeout(1).
with_deadline() {
  local secs=$1; shift
  "$@" & local pid=$!
  local waited=0
  while kill -0 "$pid" 2>/dev/null; do
    if (( waited >= secs * 10 )); then kill -9 "$pid" 2>/dev/null; echo timeout; return; fi
    sleep 0.1; waited=$((waited+1))
  done
  wait "$pid"; echo "exit=$?"
}

reset_cache
check "--start-daemon starts one" \
  "$("$VCACHE" --start-daemon | grep -c 'daemon started')" "1"
check "a second --start-daemon finds it running" \
  "$("$VCACHE" --start-daemon | grep -c 'already running')" "1"
DPID=$(daemon_pid)
check "the daemon reports its pid" "$([[ -n "$DPID" ]] && kill -0 "$DPID" && echo yes)" "yes"
check "its state directory is private" \
  "$(ls -ld "$VCACHE_DIR/daemon" | cut -c1-10)" "drwx------"

export VCACHE_DAEMON=on
compile_a "$WORK/d1.o"
DLOG="$WORK/daemon-disk-hit.log"
rm -f "$DLOG"
( cd "$WORK/checkout-b" && VCACHE_ROOTS="$WORK/checkout-b=proj" VCACHE_LOG="$DLOG" \
    "$VCACHE" g++ -O2 -c -I include src/lib.cc -o "$WORK/d2.o" ) 2>/dev/null
check "a disk hit logs hit on disk" "$(grep -c 'hit on disk' "$DLOG" || true)" "1"
check "a compile through the daemon misses, then" "$(misses)" "1"
check "the other checkout hits through the daemon" "$(hits)" "1"
check "the daemon served both lookups" "$(daemon_stat 'lookups')" "2"
check "and took the store" "$(daemon_stat 'stores')" "1"
if cmp -s "$WORK/d1.o" "$WORK/d2.o"; then
  ok "objects through the daemon are byte-identical"
else
  bad "objects through the daemon are byte-identical"
fi
check "the entry is in the ordinary disk layer" "$(disk_entries)" "1"
check "--show-stats mentions the running daemon" \
  "$("$VCACHE" --show-stats | grep -c '^daemon .*running')" "1"

# A client whose cache differs is refused and runs in-process, so it still
# builds and still caches -- just not through the daemon.
VCACHE_CACHE_SIZE=5G compile_a "$WORK/d3.o"
check "a client with a different cache config still compiles" \
  "$([[ -s "$WORK/d3.o" ]] && echo yes)" "yes"
check "and the daemon recorded the refusal" \
  "$("$VCACHE" --daemon-status | grep -c '1 refused')" "1"
check "the refused client hit the disk layer in-process" "$(hits)" "2"

# A daemon killed outright: compiles fall back, and a new one can take over
# the lock and the leftover socket.
kill -9 "$DPID" 2>/dev/null; sleep 0.2
compile_b "$WORK/d4.o"
check "a compile after the daemon is killed still hits, in-process" "$(hits)" "3"
check "--start-daemon replaces a killed daemon" \
  "$("$VCACHE" --start-daemon | grep -c 'daemon started')" "1"
check "--stop-daemon stops it" "$("$VCACHE" --stop-daemon | grep -c 'stopped')" "1"
check "and nothing answers afterwards" \
  "$("$VCACHE" --daemon-status >/dev/null 2>&1; echo $?)" "1"

# auto: the first compile starts the daemon. It must not keep the build's
# output pipe open, or `$(...)` -- and make, and a CI step -- never finishes.
export VCACHE_DAEMON=auto
reset_cache
result=$(with_deadline 20 bash -c "out=\$(cd '$WORK/checkout-a' && VCACHE_ROOTS='$WORK/checkout-a=proj' '$VCACHE' g++ -O2 -c -I include src/lib.cc -o '$WORK/d5.o' 2>&1); echo \"\$out\"")
check "auto mode does not hold the caller's output open" \
  "$([[ "$result" != *timeout* ]] && echo yes)" "yes"
check "auto mode started a daemon" "$([[ -n "$(daemon_pid)" ]] && echo yes)" "yes"
check "and the compile went through it" "$(daemon_stat 'stores')" "1"
"$VCACHE" --stop-daemon >/dev/null

# A relative VCACHE_DIR names the same cache for the daemon, which runs from
# "/", as for the compile that started it. Otherwise the daemon would refuse
# its own client.
mkdir -p "$WORK/rel"
( cd "$WORK/rel" && VCACHE_DIR=relcache VCACHE_ROOTS="$WORK/checkout-a=proj" \
    "$VCACHE" g++ -O2 -c -I "$WORK/checkout-a/include" "$WORK/checkout-a/src/lib.cc" \
    -o "$WORK/rel/r.o" ) 2>/dev/null
check "auto mode with a relative VCACHE_DIR serves its own client" \
  "$(cd "$WORK/rel" && VCACHE_DIR=relcache "$VCACHE" --daemon-status | awk '/^stores/ {print $NF}')" "1"
( cd "$WORK/rel" && VCACHE_DIR=relcache "$VCACHE" --stop-daemon >/dev/null )

# A daemon whose cache directory is deleted from under it exits rather than
# lingering unreachable until its idle timeout.
reset_cache
"$VCACHE" --start-daemon >/dev/null
DPID=$(daemon_pid)
reset_cache
for _ in $(seq 1 30); do kill -0 "$DPID" 2>/dev/null || break; sleep 0.1; done
check "a daemon whose cache directory is deleted exits" \
  "$(kill -0 "$DPID" 2>/dev/null && echo running || echo exited)" "exited"

# Idle exit.
VCACHE_DAEMON_IDLE_TIMEOUT=1 "$VCACHE" --start-daemon >/dev/null
sleep 2.5
check "an idle daemon exits on its own" \
  "$("$VCACHE" --daemon-status >/dev/null 2>&1; echo $?)" "1"
export VCACHE_DAEMON=on

if command -v python3 >/dev/null 2>&1; then
  S3PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
  S3DIR="$WORK/daemon-s3"
  python3 "$TOP/tests/mock_s3.py" "$S3PORT" "$S3DIR" &
  S3PID=$!
  for _ in $(seq 1 50); do
    python3 -c "
import socket,sys
s=socket.socket()
try: s.connect(('127.0.0.1',$S3PORT)); sys.exit(0)
except Exception: sys.exit(1)
" 2>/dev/null && break
    sleep 0.1
  done
  s3_objects() { find "$S3DIR" -type f 2>/dev/null | wc -l | tr -d " "; }

  export AWS_ACCESS_KEY_ID=AKIDEXAMPLE
  export AWS_SECRET_ACCESS_KEY=wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY
  export VCACHE_S3_BUCKET=testbucket
  export VCACHE_S3_ENDPOINT="http://127.0.0.1:$S3PORT"
  export VCACHE_S3_PATH_STYLE=1
  export VCACHE_S3_REGION=us-east-1

  reset_cache
  "$VCACHE" --start-daemon >/dev/null
  compile_a "$WORK/ds1.o"
  check "a store through the daemon reaches disk" "$(disk_entries)" "1"
  stop_out=$("$VCACHE" --stop-daemon)
  check "--stop-daemon drains the upload" "$(grep -c 'uploaded 1, failed 0' <<<"$stop_out")" "1"
  check "the entry reached s3" "$(s3_objects)" "1"

  # A fresh local cache: the daemon fetches from s3 and backfills disk.
  reset_cache
  "$VCACHE" --start-daemon >/dev/null
  compile_b "$WORK/ds2.o"
  check "the daemon serves an s3 hit" "$(stat_of 'cache hit (s3)')" "1"
  check "and backfills the disk layer" "$(disk_entries)" "1"
  if cmp -s "$WORK/ds1.o" "$WORK/ds2.o"; then
    ok "the object from s3 via the daemon is byte-identical"
  else
    bad "the object from s3 via the daemon is byte-identical"
  fi
  "$VCACHE" --stop-daemon >/dev/null

  # A journalled upload left by a daemon that died is sent by the next one.
  rm -rf "$S3DIR"; mkdir -p "$S3DIR"
  entry=$(find "$VCACHE_DIR" -mindepth 2 -maxdepth 2 -type f -path "$VCACHE_DIR/??/*" | head -1)
  key="$(basename "$(dirname "$entry")")$(basename "$entry")"
  mkdir -p "$VCACHE_DIR/daemon/pending"
  : > "$VCACHE_DIR/daemon/pending/$key"
  "$VCACHE" --start-daemon >/dev/null
  stop_out=$("$VCACHE" --stop-daemon)
  check "a journalled upload is recovered and sent" "$(s3_objects)" "1"
  check "and its journal entry is cleared" \
    "$(find "$VCACHE_DIR/daemon/pending" -type f | wc -l | tr -d ' ')" "0"

  # Uploads are asynchronous, so a failed one cannot fail the compile that
  # stored it. --stop-daemon is where it surfaces, and where the strict flag
  # turns it into an exit status.
  kill "$S3PID" 2>/dev/null; wait "$S3PID" 2>/dev/null
  reset_cache
  "$VCACHE" --start-daemon >/dev/null
  compile_a "$WORK/ds3.o"
  check "a compile with s3 down still produces its object" \
    "$([[ -s "$WORK/ds3.o" ]] && echo yes)" "yes"
  VCACHE_ERROR_ON_CACHE_MEDIA_FAILURE=1 "$VCACHE" --stop-daemon > "$WORK/stop.out"
  check "--stop-daemon reports the failed upload" "$?" "90"
  check "and says so" "$(grep -c 'failed 1' "$WORK/stop.out")" "1"

  unset AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY VCACHE_S3_BUCKET \
        VCACHE_S3_ENDPOINT VCACHE_S3_PATH_STYLE VCACHE_S3_REGION
else
  skipped "daemon s3 tests: python3 not installed"
fi
"$VCACHE" --stop-daemon >/dev/null 2>&1
unset VCACHE_DAEMON VCACHE_DAEMON_IDLE_TIMEOUT

# --------------------------------------------------------------------------
section "11c. A held blob is a memory hit"

# With the disk layer off, a store waits in the upload queue. The next lookup
# is served from that queue. The client logs the layer and still counts a
# disk hit: the stats file's positional lines are a format other tools read.
if ! command -v python3 >/dev/null 2>&1; then
  skipped "held-hit test: python3 not installed"
else
  reset_cache
  S3PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
  S3DIR="$WORK/held-s3"
  MOCK_S3_LATENCY_MS=3000 python3 "$TOP/tests/mock_s3.py" "$S3PORT" "$S3DIR" &
  S3PID=$!
  for _ in $(seq 1 50); do
    python3 -c "
import socket,sys
s=socket.socket()
try: s.connect(('127.0.0.1',$S3PORT)); sys.exit(0)
except Exception: sys.exit(1)
" 2>/dev/null && break
    sleep 0.1
  done
  export AWS_ACCESS_KEY_ID=AKIDEXAMPLE
  export AWS_SECRET_ACCESS_KEY=wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY
  export VCACHE_S3_BUCKET=testbucket
  export VCACHE_S3_ENDPOINT="http://127.0.0.1:$S3PORT"
  export VCACHE_S3_PATH_STYLE=1
  export VCACHE_S3_REGION=us-east-1
  export VCACHE_DISK=0
  export VCACHE_DAEMON=on
  "$VCACHE" --start-daemon >/dev/null
  ( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
      "$VCACHE" g++ -O2 -c -I include src/lib.cc -o "$WORK/held1.o" ) 2>/dev/null
  check "the held-blob compile misses" "$(misses)" "1"
  HLOG="$WORK/held-hit.log"
  rm -f "$HLOG"
  ( cd "$WORK/checkout-b" && VCACHE_ROOTS="$WORK/checkout-b=proj" VCACHE_LOG="$HLOG" \
      "$VCACHE" g++ -O2 -c -I include src/lib.cc -o "$WORK/held2.o" ) 2>/dev/null
  check "a held hit logs hit on memory" "$(grep -c 'hit on memory' "$HLOG" || true)" "1"
  check "a held hit counts as a disk hit" "$(hits)" "1"
  check "the daemon counts a memory hit" "$(daemon_stat 'hit (memory)')" "1"
  "$VCACHE" --stop-daemon >/dev/null 2>&1 || true
  kill "$S3PID" 2>/dev/null || true
  wait "$S3PID" 2>/dev/null || true
  unset AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY VCACHE_S3_BUCKET \
        VCACHE_S3_ENDPOINT VCACHE_S3_PATH_STYLE VCACHE_S3_REGION \
        VCACHE_DISK VCACHE_DAEMON
fi

# --------------------------------------------------------------------------
section "11b2. A second store during an upload is the one S3 keeps"

# Two stores of one key, the second while the first upload is still in the
# mock's latency. The bucket must end on the second value.
if ! command -v python3 >/dev/null 2>&1; then
  skipped "rewrite upload test: python3 not installed"
else
  reset_cache
  "$VCACHE" --stop-daemon >/dev/null 2>&1 || true
  S3PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
  S3DIR="$WORK/rewrite-s3"
  MOCK_S3_LATENCY_MS=500 python3 "$TOP/tests/mock_s3.py" "$S3PORT" "$S3DIR" &
  S3PID=$!
  for _ in $(seq 1 50); do
    python3 -c "
import socket,sys
s=socket.socket()
try: s.connect(('127.0.0.1',$S3PORT)); sys.exit(0)
except Exception: sys.exit(1)
" 2>/dev/null && break
    sleep 0.1
  done
  export AWS_ACCESS_KEY_ID=AKIDEXAMPLE
  export AWS_SECRET_ACCESS_KEY=wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY
  export VCACHE_S3_BUCKET=testbucket
  export VCACHE_S3_ENDPOINT="http://127.0.0.1:$S3PORT"
  export VCACHE_S3_PATH_STYLE=1
  export VCACHE_S3_REGION=us-east-1
  export VCACHE_DAEMON=on
  "$VCACHE" --start-daemon >/dev/null
  rewrite_key=22222222222222222222222222222222
  printf 'manifest-v1' | "$VCACHE" --test-put "$rewrite_key"
  printf 'manifest-v2' | "$VCACHE" --test-put "$rewrite_key"
  check "a merged re-put is not a second queued upload" \
    "$(daemon_stat 'uploads queued')" "1"
  "$VCACHE" --stop-daemon >/dev/null
  rewrite_obj="$S3DIR/${rewrite_key:0:2}__${rewrite_key:2}"
  check "the bucket object is the second store" "$(cat "$rewrite_obj" 2>/dev/null)" "manifest-v2"
  kill "$S3PID" 2>/dev/null || true
  wait "$S3PID" 2>/dev/null || true

  # One upload thread, so a slow put stays in flight or queued on purpose.
  # A small body sleeps longer and is written when that sleep ends, so the
  # last completion is the value left in the bucket.
  kill "$S3PID" 2>/dev/null || true
  wait "$S3PID" 2>/dev/null || true
  export VCACHE_DISK=0
  export VCACHE_TEST_MAX_HELD_BYTES=10
  export VCACHE_DAEMON_UPLOAD_THREADS=1
  export MOCK_S3_LATENCY_MS=200
  export MOCK_S3_APPLY_AFTER_LATENCY=1
  export MOCK_S3_SLOW_UNDER_BYTES=10
  export MOCK_S3_SLOW_UNDER_EXTRA_MS=1500

  start_order_s3() {  # $1 storage dir
    S3PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
    S3DIR="$1"
    python3 "$TOP/tests/mock_s3.py" "$S3PORT" "$S3DIR" &
    S3PID=$!
    for _ in $(seq 1 50); do
      python3 -c "
import socket,sys
s=socket.socket()
try: s.connect(('127.0.0.1',$S3PORT)); sys.exit(0)
except Exception: sys.exit(1)
" 2>/dev/null && break
      sleep 0.05
    done
    export VCACHE_S3_ENDPOINT="http://127.0.0.1:$S3PORT"
    reset_cache
    "$VCACHE" --start-daemon >/dev/null
  }
  wait_started() {  # $1 path
    local saw=0
    for _ in $(seq 1 100); do
      if [[ -f "$1" ]]; then saw=1; break; fi
      sleep 0.05
    done
    echo "$saw"
  }

  # The worker is busy with the blocker, so the first victim value is queued.
  start_order_s3 "$WORK/refuse-queued-s3"
  blocker_key=11111111111111111111111111111111
  queued_key=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
  printf 'bbbb' | "$VCACHE" --test-put "$blocker_key"
  blocker_obj="$S3DIR/${blocker_key:0:2}__${blocker_key:2}.started"
  check "the blocker upload is in flight" "$(wait_started "$blocker_obj")" "1"
  printf 'v1v1' | "$VCACHE" --test-put "$queued_key"
  printf 'V2VALUE-0123456789' | "$VCACHE" --test-put "$queued_key"
  "$VCACHE" --stop-daemon >/dev/null
  queued_obj="$S3DIR/${queued_key:0:2}__${queued_key:2}"
  check "a refused re-put of a queued value leaves the newer value in s3" \
    "$(cat "$queued_obj" 2>/dev/null)" "V2VALUE-0123456789"
  kill "$S3PID" 2>/dev/null || true
  wait "$S3PID" 2>/dev/null || true

  # Nothing else is queued, so the victim's own upload is the one in flight.
  start_order_s3 "$WORK/refuse-flight-s3"
  flight_key=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
  printf 'v1v1' | "$VCACHE" --test-put "$flight_key"
  flight_obj="$S3DIR/${flight_key:0:2}__${flight_key:2}"
  check "the victim upload is in flight" "$(wait_started "$flight_obj.started")" "1"
  printf 'V2VALUE-0123456789' | "$VCACHE" --test-put "$flight_key" &
  flight_put=$!
  flight_marked=0
  for _ in $(seq 1 40); do
    if [[ "$(daemon_stat 'uploads superseded')" == "1" ]]; then flight_marked=1; break; fi
    if ! kill -0 "$flight_put" 2>/dev/null; then break; fi
    sleep 0.05
  done
  check "a refused in-flight re-put is marked superseded" "$flight_marked" "1"
  wait "$flight_put"
  upload_identity=$("$VCACHE" --daemon-status 2>/dev/null | awk '
    $1=="uploads" && $2=="queued" {q=$NF}
    $1=="completed" {c=$NF}
    $1=="bytes" {seen_bytes=1}
    $1=="failed" && seen_bytes {f=$NF}
    $1=="skipped" {s=$NF}
    $1=="pending" {p=$NF}
    $1=="uploads" && $2=="superseded" {u=$NF}
    END {
      sum = c+0 + f+0 + s+0 + p+0 + u+0
      if (q+0 == sum) print "yes"
      else print q "!=" sum
    }')
  check "a drained in-flight refusal keeps the upload identity" "$upload_identity" "yes"
  "$VCACHE" --stop-daemon >/dev/null
  check "a refused re-put of an in-flight value leaves the newer value in s3" \
    "$(cat "$flight_obj" 2>/dev/null)" "V2VALUE-0123456789"

  # V2 is refused while V1 is in flight, then V3 is accepted. V2 must not
  # upload after V3.
  start_order_s3 "$WORK/refuse-overtake-s3"
  overtake_key=cccccccccccccccccccccccccccccccc
  printf 'v1v1' | "$VCACHE" --test-put "$overtake_key"
  overtake_obj="$S3DIR/${overtake_key:0:2}__${overtake_key:2}"
  check "the overtaken upload is in flight" "$(wait_started "$overtake_obj.started")" "1"
  printf 'V2VALUE-0123456789' | "$VCACHE" --test-put "$overtake_key" &
  overtake_put=$!
  overtake_marked=0
  for _ in $(seq 1 40); do
    if [[ "$(daemon_stat 'uploads superseded')" == "1" ]]; then overtake_marked=1; break; fi
    if ! kill -0 "$overtake_put" 2>/dev/null; then break; fi
    sleep 0.05
  done
  check "the overtaken refusal is waiting" "$overtake_marked" "1"
  printf 'v3v3' | "$VCACHE" --test-put "$overtake_key"
  wait "$overtake_put"
  "$VCACHE" --stop-daemon >/dev/null
  check "a newer accepted store is what s3 keeps" \
    "$(cat "$overtake_obj" 2>/dev/null)" "v3v3"
  kill "$S3PID" 2>/dev/null || true
  wait "$S3PID" 2>/dev/null || true
  unset AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY VCACHE_S3_BUCKET \
        VCACHE_S3_ENDPOINT VCACHE_S3_PATH_STYLE VCACHE_S3_REGION \
        VCACHE_DAEMON VCACHE_DISK VCACHE_TEST_MAX_HELD_BYTES \
        VCACHE_DAEMON_UPLOAD_THREADS MOCK_S3_LATENCY_MS \
        MOCK_S3_APPLY_AFTER_LATENCY MOCK_S3_SLOW_UNDER_BYTES \
        MOCK_S3_SLOW_UNDER_EXTRA_MS
fi

# --------------------------------------------------------------------------
section "11. Daemon uploads survive eviction"

# A later compile must not evict an entry whose upload is still journalled.
# The mock answers slowly so the journal is still there when the cache trims.
if ! command -v python3 >/dev/null 2>&1; then
  skipped "upload eviction test: python3 not installed"
else
  reset_cache
  S3PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
  S3DIR="$WORK/evict-s3"
  MOCK_S3_LATENCY_MS=300 python3 "$TOP/tests/mock_s3.py" "$S3PORT" "$S3DIR" &
  S3PID=$!
  for _ in $(seq 1 50); do
    python3 -c "
import socket,sys
s=socket.socket()
try: s.connect(('127.0.0.1',$S3PORT)); sys.exit(0)
except Exception: sys.exit(1)
" 2>/dev/null && break
    sleep 0.1
  done
  export AWS_ACCESS_KEY_ID=AKIDEXAMPLE
  export AWS_SECRET_ACCESS_KEY=wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY
  export VCACHE_S3_BUCKET=testbucket
  export VCACHE_S3_ENDPOINT="http://127.0.0.1:$S3PORT"
  export VCACHE_S3_PATH_STYLE=1
  export VCACHE_S3_REGION=us-east-1
  export VCACHE_CACHE_SIZE=16K
  export VCACHE_DAEMON=on
  "$VCACHE" --start-daemon >/dev/null
  mkdir -p "$WORK/evict-src"
  for n in 1 2 3 4 5; do
    printf 'int evict_%d(void){return %d;}\n' "$n" "$n" > "$WORK/evict-src/f$n.c"
    ( cd "$WORK/evict-src" && VCACHE_ROOTS="$WORK/evict-src=proj" \
        "$VCACHE" gcc -c "f$n.c" -o "$WORK/evict$n.o" ) >/dev/null
  done
  for _ in $(seq 1 80); do
    if [[ "$(daemon_stat 'pending')" == "0" ]]; then break; fi
    sleep 0.1
  done
  check "eviction does not skip an upload" "$(daemon_stat 'skipped')" "0"
  "$VCACHE" --stop-daemon >/dev/null 2>&1 || true
  check "the bucket received all five compiles" \
    "$(find "$S3DIR" -type f | wc -l | tr -d ' ')" "5"
  kill "$S3PID" 2>/dev/null || true
  wait "$S3PID" 2>/dev/null || true
  unset AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY VCACHE_S3_BUCKET \
        VCACHE_S3_ENDPOINT VCACHE_S3_PATH_STYLE VCACHE_S3_REGION \
        VCACHE_CACHE_SIZE VCACHE_DAEMON
fi

# --------------------------------------------------------------------------
section "10d. Jobserver"

"$VCACHE" --stop-daemon >/dev/null 2>&1 || true
reset_cache
check "jobserver is off by default" \
  "$("$VCACHE" --start-daemon >/dev/null && [[ ! -p "$VCACHE_DIR/daemon/jobserver.fifo" ]] && echo yes)" "yes"
check "--jobserver-env without a pool exits 1" \
  "$("$VCACHE" --jobserver-env >/dev/null 2>&1; echo $?)" "1"
"$VCACHE" --stop-daemon >/dev/null 2>&1 || true
reset_cache

export VCACHE_DAEMON_JOBSERVER=1
export VCACHE_DAEMON_JOBSERVER_JOBS=2
export VCACHE_DAEMON_IDLE_TIMEOUT=60
check "a jobserver daemon starts" \
  "$("$VCACHE" --start-daemon | grep -c 'daemon started')" "1"
JS_FIFO="$VCACHE_DIR/daemon/jobserver.fifo"
check "the jobserver fifo exists" "$([[ -p "$JS_FIFO" ]] && echo yes)" "yes"
check "jobserver tokens total" "$(daemon_stat 'jobserver tokens total')" "2"
check "jobserver tokens free" "$(daemon_stat 'jobserver tokens free')" "2"
check "jobserver tokens withdrawn" "$(daemon_stat 'jobserver tokens withdrawn')" "0"
check "--jobserver-env prints the fifo MAKEFLAGS line" \
  "$("$VCACHE" --jobserver-env)" "MAKEFLAGS=-j --jobserver-auth=fifo:$JS_FIFO"

mkdir -p "$WORK/js"
cat > "$WORK/js/tick.sh" << 'EOF'
#!/bin/sh
file=$1
exec 9>>"$file.lock"
flock 9
cur=0; max=0
if read -r cur max < "$file"; then :; fi
cur=$((cur + 1))
if [ "$cur" -gt "$max" ]; then max=$cur; fi
printf '%s %s\n' "$cur" "$max" > "$file"
flock -u 9
sleep 1
flock 9
cur=0; max=0
read -r cur max < "$file"
cur=$((cur - 1))
printf '%s %s\n' "$cur" "$max" > "$file"
flock -u 9
EOF
chmod +x "$WORK/js/tick.sh"
cat > "$WORK/js/Makefile" << 'EOF'
.PHONY: all t1 t2 t3 t4 t5 t6
all: t1 t2 t3 t4 t5 t6
t1 t2 t3 t4 t5 t6:
	sh tick.sh count
EOF
cat > "$WORK/js/build.ninja" << 'EOF'
rule tick
  command = sh tick.sh count
build t1: tick
build t2: tick
build t3: tick
build t4: tick
build t5: tick
build t6: tick
build all: phony t1 t2 t3 t4 t5 t6
default all
EOF

js_max() { awk '{ print $2 }' "$WORK/js/count"; }
# The printed line contains a space. An unquoted $(...) splits it, and env
# then keeps only MAKEFLAGS=-j. Quoting passes the whole assignment.
js_flags=$("$VCACHE" --jobserver-env)
printf '0 0\n' > "$WORK/js/count"
mk_out=$(with_deadline 20 bash -c 'cd "$1" && env "$2" make >/dev/null' _ "$WORK/js" "$js_flags")
check "make under the jobserver finishes" "$([[ "$mk_out" != *timeout* ]] && echo yes)" "yes"
check "make under the jobserver never exceeds 2" "$(js_max)" "2"

# PATH ninja on this machine is a wrapper that appends -j, which makes ninja
# ignore the fifo. The real client is /usr/bin/ninja.
ninja_bin=/usr/bin/ninja
if [[ -x "$ninja_bin" ]] && \
   [[ "$(printf '%s\n' 1.13 "$("$ninja_bin" --version)" | sort -V | head -1)" == "1.13" ]]; then
  printf '0 0\n' > "$WORK/js/count"
  nj_out=$(with_deadline 20 bash -c 'cd "$1" && env "$2" "$3" >/dev/null' _ \
    "$WORK/js" "$js_flags" "$ninja_bin")
  check "ninja under the jobserver finishes" "$([[ "$nj_out" != *timeout* ]] && echo yes)" "yes"
  check "ninja under the jobserver never exceeds 2" "$(js_max)" "2"
else
  skipped "ninja >= 1.13 not installed"
fi

printf '0 0\n' > "$WORK/js/count"
( cd "$WORK/js" && env -u MAKEFLAGS make -j6 ) >/dev/null
check "make -j6 without the jobserver runs 6 wide" "$(js_max)" "6"

"$VCACHE" --stop-daemon >/dev/null
check "stopping the daemon removes the fifo" "$([[ ! -p "$JS_FIFO" ]] && echo yes)" "yes"

# A token held past one idle timeout must not retire the daemon. The second
# timeout is what retires a pool whose client has gone away with a token.
reset_cache
VCACHE_DAEMON_IDLE_TIMEOUT=3 "$VCACHE" --start-daemon >/dev/null
JS_READY=$(date +%s)
JS_FIFO="$VCACHE_DIR/daemon/jobserver.fifo"
JS_PID=$(daemon_pid)
(
  exec 3<>"$JS_FIFO"
  IFS= read -r -n 1 -u 3
  sleep 8
  printf '+' >&3
) &
JS_HOLDER=$!
held=0
for _ in $(seq 1 30); do
  if [[ "$(daemon_stat 'jobserver tokens free')" == "1" ]]; then held=1; break; fi
  sleep 0.1
done
check "a client holds one jobserver token" "$held" "1"
while (( $(date +%s) < JS_READY + 4 )); do sleep 0.2; done
check "the daemon stays up while a token is out past one idle timeout" \
  "$(kill -0 "$JS_PID" 2>/dev/null && echo yes)" "yes"
"$VCACHE" --stop-daemon >/dev/null 2>&1 || true
kill "$JS_HOLDER" 2>/dev/null || true
wait "$JS_HOLDER" 2>/dev/null || true

reset_cache
"$VCACHE" --start-daemon >/dev/null
JS_FIFO="$VCACHE_DIR/daemon/jobserver.fifo"
rm -f "$JS_FIFO"
check "--jobserver-env exits 1 when the fifo is missing" \
  "$("$VCACHE" --jobserver-env >/dev/null 2>&1; echo $?)" "1"
"$VCACHE" --stop-daemon >/dev/null 2>&1 || true
unset VCACHE_DAEMON_JOBSERVER VCACHE_DAEMON_JOBSERVER_JOBS VCACHE_DAEMON_IDLE_TIMEOUT

# --------------------------------------------------------------------------
section "12. runtime dependencies stay minimal"

# vcache runs once per compilation, so every DT_NEEDED entry is mapped and
# relocated on every invocation. libcurl alone drags in ~30 shared objects.
needed="$(linked_libraries "$VCACHE")"
if grep -q "libcurl" <<<"$needed"; then
  bad "libcurl is not a link-time dependency"
else
  ok "libcurl is not a link-time dependency"
fi
if grep -qE "libcrypto|libssl" <<<"$needed"; then
  bad "OpenSSL is not a link-time dependency"
else
  ok "OpenSSL is not a link-time dependency"
fi
# Counted through the same helper so the number means something on macOS too,
# where there is no ldd and this silently reported an empty closure.
lib_count="$(linked_libraries "$VCACHE" | wc -l | tr -d ' ')"
if [[ "$lib_count" -ge 1 && "$lib_count" -le 6 ]]; then
  ok "link-time closure is small ($lib_count entries)"
else
  bad "link-time closure is small (got $lib_count entries)"
fi

# The dlopen must actually be conditional, not merely deferred to startup.
if command -v ldd >/dev/null 2>&1; then
  echo 'int probe(){return 1;}' > "$WORK/probe.cc"
  reset_cache
  disk_only="$(LD_DEBUG=libs "$VCACHE" g++ -c "$WORK/probe.cc" -o "$WORK/probe.o" 2>&1 \
                 | grep -ci libcurl || true)"
  check "disk-only compile never loads libcurl" "$disk_only" "0"

  # Point at a dead port: the load must still happen, then fail as a miss.
  reset_cache
  with_s3="$(AWS_ACCESS_KEY_ID=x AWS_SECRET_ACCESS_KEY=y \
             VCACHE_S3_BUCKET=b VCACHE_S3_PATH_STYLE=1 \
             VCACHE_S3_ENDPOINT=http://127.0.0.1:1 \
             LD_DEBUG=libs "$VCACHE" g++ -c "$WORK/probe.cc" -o "$WORK/probe2.o" 2>&1 \
               | grep -ci libcurl || true)"
  if [[ "$with_s3" -gt 0 ]]; then
    ok "configuring s3 does load libcurl on demand"
  else
    bad "configuring s3 does load libcurl on demand"
  fi
  check "unreachable s3 endpoint still compiles" "$([[ -s "$WORK/probe2.o" ]] && echo yes)" "yes"
fi

# --------------------------------------------------------------------------
section "13. -march=native resolves to a concrete target"

if ! g++ -march=native -E -dM -x c++ /dev/null >/dev/null 2>&1; then
  printf '  (skipped: this compiler does not accept -march=native)\n'
else
  make_project "$WORK/native-a"
  make_project "$WORK/native-b"
  reset_cache

  ( cd "$WORK/native-a" && VCACHE_ROOTS="$WORK/native-a=proj" \
      "$VCACHE" g++ -g -O2 -march=native -mtune=native -c -I include src/lib.cc \
      -o "$WORK/na.o" ) 2>/dev/null
  check "native compile is cached, not skipped" "$(misses)" "1"
  check "native compile is not counted uncacheable" "$(uncacheable)" "0"

  ( cd "$WORK/native-b" && VCACHE_ROOTS="$WORK/native-b=proj" \
      "$VCACHE" g++ -g -O2 -march=native -mtune=native -c -I include src/lib.cc \
      -o "$WORK/nb.o" ) 2>/dev/null
  check "a second checkout hits with -march=native" "$(hits)" "1"

  if cmp -s "$WORK/na.o" "$WORK/nb.o"; then
    ok "native objects from both checkouts are byte-identical"
  else
    bad "native objects from both checkouts are byte-identical"
  fi

  # Dropping the flag must land on a different key: the resolved target is part
  # of what was hashed, so this is a miss rather than a (wrong) hit.
  ( cd "$WORK/native-a" && VCACHE_ROOTS="$WORK/native-a=proj" \
      "$VCACHE" g++ -g -O2 -c -I include src/lib.cc -o "$WORK/nc.o" ) 2>/dev/null
  check "the same source without -march=native is a separate entry" "$(misses)" "2"

  # The escape hatch for a cache directory shared between unlike machines.
  reset_cache
  ( cd "$WORK/native-a" && VCACHE_NATIVE_TARGET=uncacheable \
      VCACHE_ROOTS="$WORK/native-a=proj" \
      "$VCACHE" g++ -g -O2 -march=native -c -I include src/lib.cc \
      -o "$WORK/nd.o" ) 2>/dev/null
  check "VCACHE_NATIVE_TARGET=uncacheable declines to cache" "$(uncacheable)" "1"
  check "and still produces an object" "$([[ -s "$WORK/nd.o" ]] && echo yes)" "yes"
fi

# --------------------------------------------------------------------------
section "14. dependency scans (-M/-MM) are cached against a manifest"

# A tree whose header graph has some depth, so the scan has something to find.
for tree in dep-a dep-b; do
  mkdir -p "$WORK/$tree/src" "$WORK/$tree/inc"
  cat > "$WORK/$tree/inc/base.h" <<'EOF'
#pragma once
enum { kBase = 1 };
EOF
  cat > "$WORK/$tree/inc/mid.h" <<'EOF'
#pragma once
#include "base.h"
int mid(void);
EOF
  cat > "$WORK/$tree/src/top.c" <<'EOF'
#include "mid.h"
int mid(void) { return kBase; }
EOF
done
reset_cache

scan_a() { ( cd "$WORK/dep-a" && VCACHE_ROOTS="$WORK/dep-a=proj" \
  "$VCACHE" gcc -M -MP -I inc "$@" src/top.c -o "$WORK/a.d" ) 2>/dev/null; }

scan_a
check "first dependency scan is a miss" "$(misses)" "1"
check "and it is not counted uncacheable" "$(uncacheable)" "0"
if grep -q 'base\.h' "$WORK/a.d" && grep -q 'mid\.h' "$WORK/a.d"; then
  ok "the scan output names the whole include graph"
else
  bad "the scan output names the whole include graph"
fi
cp "$WORK/a.d" "$WORK/a-first.d"

scan_a
check "repeating the scan hits" "$(hits)" "1"
if cmp -s "$WORK/a.d" "$WORK/a-first.d"; then
  ok "the replayed output is byte-identical to the scanned one"
else
  bad "the replayed output is byte-identical to the scanned one"
fi

# The manifest is the whole guarantee: touching a header two levels down has to
# invalidate the entry even though the command line and the source are unchanged.
echo 'enum { kExtra = 2 };' >> "$WORK/dep-a/inc/base.h"
scan_a
check "editing an indirect header invalidates the entry" "$(misses)" "2"

# Restoring it exactly must hit again -- the manifest compares content, not mtime.
sed_inplace '$ d' "$WORK/dep-a/inc/base.h"
touch "$WORK/dep-a/inc/base.h"
scan_a
check "restoring the header hits again despite a new mtime" "$(hits)" "2"

# A deleted header cannot be verified, so it must not serve.
mv "$WORK/dep-a/inc/base.h" "$WORK/base.h.bak"
scan_a >/dev/null 2>&1
mv "$WORK/base.h.bak" "$WORK/dep-a/inc/base.h"
check "a missing header does not serve a hit" "$(hits)" "2"

# Changing the include path is a different question, so a different entry.
reset_cache
scan_a
mkdir -p "$WORK/dep-a/other"
scan_a -I other
check "a changed -I is a separate entry" "$(misses)" "2"

# The cross-directory claim applies here too: the manifest holds canonical
# paths, so a second checkout verifies its own copies of the same files.
reset_cache
scan_a
( cd "$WORK/dep-b" && VCACHE_ROOTS="$WORK/dep-b=proj" \
    "$VCACHE" gcc -M -MP -I inc src/top.c -o "$WORK/b.d" ) 2>/dev/null
check "a second checkout hits the same entry" "$(hits)" "1"
if [[ -s "$WORK/b.d" ]] && cmp -s "$WORK/a.d" "$WORK/b.d" &&
   ! grep -q "dep-a" "$WORK/b.d"; then
  ok "and gets an answer with no trace of the other checkout"
else
  bad "and gets an answer with no trace of the other checkout"
fi

# The same, with the source named absolutely, so the paths that come back are
# absolute and have to be localised rather than merely left alone.
reset_cache
( cd "$WORK" && VCACHE_ROOTS="$WORK/dep-a=proj" \
    "$VCACHE" gcc -M -I "$WORK/dep-a/inc" "$WORK/dep-a/src/top.c" \
    -o "$WORK/abs-a.d" ) 2>/dev/null
( cd "$WORK" && VCACHE_ROOTS="$WORK/dep-b=proj" \
    "$VCACHE" gcc -M -I "$WORK/dep-b/inc" "$WORK/dep-b/src/top.c" \
    -o "$WORK/abs-b.d" ) 2>/dev/null
check "absolute paths hit across checkouts too" "$(hits)" "1"
if grep -q "dep-b/inc/base.h" "$WORK/abs-b.d" && ! grep -q "dep-a" "$WORK/abs-b.d"; then
  ok "and the replayed paths point at this checkout"
else
  bad "and the replayed paths point at this checkout"
fi

# With no -o the answer goes to stdout, on the hit path as well as the miss.
reset_cache
( cd "$WORK/dep-a" && VCACHE_ROOTS="$WORK/dep-a=proj" \
    "$VCACHE" gcc -M -I inc src/top.c > "$WORK/stdout1.d" ) 2>/dev/null
( cd "$WORK/dep-a" && VCACHE_ROOTS="$WORK/dep-a=proj" \
    "$VCACHE" gcc -M -I inc src/top.c > "$WORK/stdout2.d" ) 2>/dev/null
check "a scan to stdout hits on the second run" "$(hits)" "1"
if [[ -s "$WORK/stdout1.d" ]] && cmp -s "$WORK/stdout1.d" "$WORK/stdout2.d"; then
  ok "and replays byte-identically to stdout"
else
  bad "and replays byte-identically to stdout"
fi

# The escape hatch.
reset_cache
( cd "$WORK/dep-a" && VCACHE_DEP_SCAN=uncacheable VCACHE_ROOTS="$WORK/dep-a=proj" \
    "$VCACHE" gcc -M -I inc src/top.c -o "$WORK/off.d" ) 2>/dev/null
check "VCACHE_DEP_SCAN=uncacheable declines to cache" "$(uncacheable)" "1"
check "and still writes the dependency file" "$([[ -s "$WORK/off.d" ]] && echo yes)" "yes"

# A failing scan must propagate the compiler's exit code, not a cached success.
( cd "$WORK/dep-a" && VCACHE_ROOTS="$WORK/dep-a=proj" \
    "$VCACHE" gcc -M -I inc src/missing.c -o "$WORK/bad.d" ) 2>/dev/null
check "a scan of a missing source fails" "$?" "1"

# The source's path is part of the question, not only its content. Identical
# sources reached by different canonical paths must not share a state: the
# recorded headers still name the first directory and still verify, so the
# second would be served the first one's dependency list.
mkdir -p "$WORK/depsrc/a" "$WORK/depsrc/b" "$WORK/depsrc/c" "$WORK/depsrc/d"
for tree in a b c d; do
  printf '#include "x.h"\nint v = V;\n' > "$WORK/depsrc/$tree/main.c"
done
printf '#define V 1\n' > "$WORK/depsrc/a/x.h"
printf '#define V 2\n/* different size */\n' > "$WORK/depsrc/b/x.h"
cp "$WORK/depsrc/a/x.h" "$WORK/depsrc/c/x.h"
cp "$WORK/depsrc/a/x.h" "$WORK/depsrc/d/x.h"

# One word per line, so vcache's unwrapped rendering compares equal to gcc's.
dep_words() { tr -d '\\' < "$1" | tr -s ' \t\n' '\n' | sed '/^$/d'; }

# Runs the compiler itself in the same place, and checks vcache's answer
# against it word for word.
#   check_depsrc_answer LABEL DIR OUTFILE args...
check_depsrc_answer() {
  local label=$1 dir=$2 out=$3; shift 3
  ( cd "$dir" && gcc "$@" ) > "$WORK/depsrc-expected.d" 2>/dev/null
  if [[ -s "$out" ]] &&
     [[ "$(dep_words "$out")" == "$(dep_words "$WORK/depsrc-expected.d")" ]]; then
    ok "$label"
  else
    bad "$label (got: $(tr '\n' ' ' < "$out"))"
  fi
}

for mode in "-MM" "-M -MP"; do
  read -ra mflags <<< "$mode"

  reset_cache
  ( cd "$WORK/depsrc" && "$VCACHE" gcc "${mflags[@]}" a/main.c ) > "$WORK/ps-a.d" 2>/dev/null
  ( cd "$WORK/depsrc" && "$VCACHE" gcc "${mflags[@]}" b/main.c ) > "$WORK/ps-b.d" 2>/dev/null
  check "$mode: from a parent directory, b/main.c misses after a/main.c" "$(misses)" "2"
  check_depsrc_answer "$mode: and gets b's own dependency list" \
    "$WORK/depsrc" "$WORK/ps-b.d" "${mflags[@]}" b/main.c
  ( cd "$WORK/depsrc" && "$VCACHE" gcc "${mflags[@]}" b/main.c ) > "$WORK/ps-b2.d" 2>/dev/null
  check "$mode: repeating b hits its own state" "$(hits)" "1"
  check_depsrc_answer "$mode: and replays b's dependency list" \
    "$WORK/depsrc" "$WORK/ps-b2.d" "${mflags[@]}" b/main.c
  printf '#define V 3\n' > "$WORK/depsrc/b/x.h"
  ( cd "$WORK/depsrc" && "$VCACHE" gcc "${mflags[@]}" b/main.c ) > "$WORK/ps-b3.d" 2>/dev/null
  check "$mode: editing b/x.h invalidates b's state" "$(misses)" "3"
  printf '#define V 2\n/* different size */\n' > "$WORK/depsrc/b/x.h"

  reset_cache
  ( cd "$WORK/depsrc/a" && VCACHE_ROOTS="$WORK/depsrc=proj" \
      "$VCACHE" gcc "${mflags[@]}" "$WORK/depsrc/a/main.c" ) > "$WORK/or-a.d" 2>/dev/null
  ( cd "$WORK/depsrc/b" && VCACHE_ROOTS="$WORK/depsrc=proj" \
      "$VCACHE" gcc "${mflags[@]}" "$WORK/depsrc/b/main.c" ) > "$WORK/or-b.d" 2>/dev/null
  check "$mode: one root over both, b misses after a" "$(misses)" "2"
  check_depsrc_answer "$mode: and gets b's own dependency list" \
    "$WORK/depsrc/b" "$WORK/or-b.d" "${mflags[@]}" "$WORK/depsrc/b/main.c"

  reset_cache
  ( cd "$WORK/depsrc/a" && VCACHE_MAP_CWD=0 \
      "$VCACHE" gcc "${mflags[@]}" "$WORK/depsrc/a/main.c" ) > "$WORK/nr-a.d" 2>/dev/null
  ( cd "$WORK/depsrc/b" && VCACHE_MAP_CWD=0 \
      "$VCACHE" gcc "${mflags[@]}" "$WORK/depsrc/b/main.c" ) > "$WORK/nr-b.d" 2>/dev/null
  check "$mode: with no roots at all, b misses after a" "$(misses)" "2"
  check_depsrc_answer "$mode: and gets b's own dependency list" \
    "$WORK/depsrc/b" "$WORK/nr-b.d" "${mflags[@]}" "$WORK/depsrc/b/main.c"

  # The sharing that is intended: one relative path under the mapped cwd, and
  # per-checkout roots naming both copies by one canonical path.
  reset_cache
  ( cd "$WORK/depsrc/c" && "$VCACHE" gcc "${mflags[@]}" main.c ) > "$WORK/rel-c.d" 2>/dev/null
  ( cd "$WORK/depsrc/d" && "$VCACHE" gcc "${mflags[@]}" main.c ) > "$WORK/rel-d.d" 2>/dev/null
  check "$mode: the same relative path in a copied tree still hits" "$(hits)" "1"
  check_depsrc_answer "$mode: and replays the local dependency list" \
    "$WORK/depsrc/d" "$WORK/rel-d.d" "${mflags[@]}" main.c

  reset_cache
  ( cd "$WORK/depsrc/c" && VCACHE_ROOTS="$WORK/depsrc/c=proj" \
      "$VCACHE" gcc "${mflags[@]}" "$WORK/depsrc/c/main.c" ) > "$WORK/pc-c.d" 2>/dev/null
  ( cd "$WORK/depsrc/d" && VCACHE_ROOTS="$WORK/depsrc/d=proj" \
      "$VCACHE" gcc "${mflags[@]}" "$WORK/depsrc/d/main.c" ) > "$WORK/pc-d.d" 2>/dev/null
  check "$mode: per-checkout roots still hit" "$(hits)" "1"
  check_depsrc_answer "$mode: and the restored paths name this checkout" \
    "$WORK/depsrc/d" "$WORK/pc-d.d" "${mflags[@]}" "$WORK/depsrc/d/main.c"
done

# --------------------------------------------------------------------------
section "14b. Concurrent dep-scan stores keep both states"

# Same source, different headers, one canonical root. The paused store must
# keep the state the other checkout wrote while it waited.
reset_cache
ds_tree() {  # $1 dir, $2 header value
  mkdir -p "$1"
  printf '#include "x.h"\nint v = V;\n' > "$1/main.c"
  printf '#define V %s\n' "$2" > "$1/x.h"
}
ds_tree "$WORK/ds-a" 1
ds_tree "$WORK/ds-b" 2
ds_tree "$WORK/ds-c" 3
ds_scan() {  # $1 dir, $2 log (may be empty)
  if [[ -n "${2:-}" ]]; then
    ( cd "$1" && VCACHE_ROOTS="$1=proj" VCACHE_LOG="$2" \
        "$VCACHE" gcc -M -MP main.c -o "$1/out.d" ) >/dev/null
  else
    ( cd "$1" && VCACHE_ROOTS="$1=proj" \
        "$VCACHE" gcc -M -MP main.c -o "$1/out.d" ) >/dev/null
  fi
}
ds_scan "$WORK/ds-a" ""
check "the first dep-scan version misses" "$(misses)" "1"
fifo="$WORK/depscan-pause.fifo"
mkfifo "$fifo"
dblog="$WORK/ds-b-pause.log"
rm -f "$dblog"
( cd "$WORK/ds-b" && VCACHE_ROOTS="$WORK/ds-b=proj" VCACHE_LOG="$dblog" \
    VCACHE_TEST_PAUSE_BEFORE_MANIFEST_PUT="$fifo" \
    "$VCACHE" gcc -M -MP main.c -o "$WORK/ds-b/out.d" ) >/dev/null &
dbpid=$!
dwaiting=0
for _ in $(seq 1 200); do
  if grep -q 'manifest: waiting before put' "$dblog" 2>/dev/null; then dwaiting=1; break; fi
  if ! kill -0 "$dbpid" 2>/dev/null; then break; fi
  sleep 0.05
done
check "the second dep-scan store waits" "$dwaiting" "1"
if [[ "$dwaiting" == 1 ]]; then
  ds_scan "$WORK/ds-c" ""
fi
release_pause_fifo "$fifo" "$dbpid" "$dwaiting"
for _ in $(seq 1 200); do
  if ! kill -0 "$dbpid" 2>/dev/null; then break; fi
  sleep 0.05
done
wait "$dbpid" || true
dblog2="$WORK/ds-b-hit.log"
rm -f "$dblog2"
ds_scan "$WORK/ds-b" "$dblog2"
check "header 2 hits through the dep-scan manifest" \
  "$(grep -c 'dep scan hit' "$dblog2" || true)" "1"
check "header 2 does not re-run the scan" "$(grep -c 'dep scan:' "$dblog2" || true)" "0"
dblog3="$WORK/ds-c-hit.log"
rm -f "$dblog3"
ds_scan "$WORK/ds-c" "$dblog3"
check "header 3 hits through the dep-scan manifest" \
  "$(grep -c 'dep scan hit' "$dblog3" || true)" "1"
check "header 3 does not re-run the scan" "$(grep -c 'dep scan:' "$dblog3" || true)" "0"

# --------------------------------------------------------------------------
section "15. flags that write a second output file are declined"

# The dangerous shape: the object comes back correct from the cache and the
# companion file silently never appears, so the failure surfaces later and
# somewhere else. Driven with gcc because it is always present; the parser
# tests cover the clang-only spellings.

reset_cache
mkdir -p "$WORK/side"
printf 'int f(void){return 1;}\n' > "$WORK/side/s.c"

( cd "$WORK/side" && VCACHE_ROOTS="$WORK/side=proj" \
    "$VCACHE" gcc --coverage -c s.c -o cov.o ) 2>/dev/null
check "--coverage is not cached" "$(uncacheable)" "1"
check "and the .gcno is produced" "$([[ -f "$WORK/side/cov.gcno" ]] && echo yes)" "yes"

# The real regression guard: compile again, and the .gcno must come back even
# though an entry for this source now exists. A cached object with no .gcno is
# exactly the silent breakage this section exists to prevent.
rm -f "$WORK/side/cov.gcno"
( cd "$WORK/side" && VCACHE_ROOTS="$WORK/side=proj" \
    "$VCACHE" gcc --coverage -c s.c -o cov.o ) 2>/dev/null
check "a second --coverage compile still produces the .gcno" \
  "$([[ -f "$WORK/side/cov.gcno" ]] && echo yes)" "yes"
check "and is still counted uncacheable" "$(uncacheable)" "2"

reset_cache
( cd "$WORK/side" && VCACHE_ROOTS="$WORK/side=proj" \
    "$VCACHE" gcc -fstack-usage -c s.c -o su.o ) 2>/dev/null
check "-fstack-usage is not cached" "$(uncacheable)" "1"
check "and the .su is produced" "$([[ -f "$WORK/side/su.su" ]] && echo yes)" "yes"

# -fopt-info writes its report to a file only when spelled with an `=`; without
# one it goes to stderr, which vcache captures and replays, so that form stays
# cacheable and only the file-writing spelling is declined.
reset_cache
( cd "$WORK/side" && VCACHE_ROOTS="$WORK/side=proj" \
    "$VCACHE" gcc -O2 -fopt-info-optimized=oi.txt -c s.c -o oi.o ) 2>/dev/null
check "-fopt-info-optimized=FILE is not cached" "$(uncacheable)" "1"
if compiler_supports -O2 -fopt-info-optimized=/dev/null; then
  check "and the report is produced" "$([[ -f "$WORK/side/oi.txt" ]] && echo yes)" "yes"
else
  skipped "-fopt-info is a gcc option; this compiler has none"
fi

reset_cache
( cd "$WORK/side" && VCACHE_ROOTS="$WORK/side=proj" \
    "$VCACHE" gcc -O2 -fopt-info-optimized -c s.c -o oi2.o ) 2>/dev/null
check "-fopt-info without a file is still cached" "$(uncacheable)" "0"

reset_cache
( cd "$WORK/side" && VCACHE_ROOTS="$WORK/side=proj" \
    "$VCACHE" gcc -aux-info aux.txt -c s.c -o ai.o ) 2>/dev/null
check "-aux-info is not cached" "$(uncacheable)" "1"
if compiler_supports -aux-info /dev/null; then
  check "and the listing is produced" "$([[ -f "$WORK/side/aux.txt" ]] && echo yes)" "yes"
else
  skipped "-aux-info is a gcc option; this compiler has none"
fi

# Without such a flag the same source caches normally, so the check above is
# about the flag and not about this file being unusual.
reset_cache
( cd "$WORK/side" && VCACHE_ROOTS="$WORK/side=proj" \
    "$VCACHE" gcc -c s.c -o plain.o ) 2>/dev/null
check "the same source without those flags is cached" "$(misses)" "1"
check "and is not counted uncacheable" "$(uncacheable)" "0"

# --------------------------------------------------------------------------
section "15b. linker-only flags do not fork the key"

# Under -c the driver never hands these to the compiler proper, and gcc ignores
# them without a word -- even under -Werror. Keying them costs hits for nothing,
# and worst of all across machines: a -L or -Wl,-rpath holding a local path is
# exactly the sort of difference the S3 layer exists to see through, and unlike
# -I these are not canonicalised by any root mapping.

reset_cache
mkdir -p "$WORK/link"
printf 'extern int ext(int);\nint fn(int x){ return ext(x) + 7; }\n' > "$WORK/link/l.c"

( cd "$WORK/link" && VCACHE_ROOTS="$WORK/link=proj" \
    "$VCACHE" gcc -O2 -L/usr/lib -lm -c l.c -o "$WORK/l1.o" ) 2>/dev/null
check "a compile with linker flags is a miss" "$(misses)" "1"
( cd "$WORK/link" && VCACHE_ROOTS="$WORK/link=proj" \
    "$VCACHE" gcc -O2 -L/somewhere/else -Wl,-rpath,/opt -rdynamic -s \
    -c l.c -o "$WORK/l2.o" ) 2>/dev/null
if cc_is_clang; then
  # Deliberate, and the opposite answer for a good reason: clang names an unused
  # linker flag in a warning, vcache stores that warning and replays it on a
  # hit, so the flags have to be in the key or the replayed text would report a
  # flag the caller never passed. gcc ignores them in silence, so they do not.
  check "different linker flags are a separate entry under clang" "$(misses)" "2"
else
  check "different linker flags hit the same entry" "$(hits)" "1"
fi

# The served object has to be what the compiler would have produced, not merely
# something: a key that drops too much is a wrong answer, not a slow one.
( cd "$WORK/link" && gcc -O2 -L/somewhere/else -Wl,-rpath,/opt -rdynamic -s \
    -c l.c -o "$WORK/lref.o" ) 2>/dev/null
if cmp -s "$WORK/l2.o" "$WORK/lref.o"; then
  ok "the served object matches an uncached compile"
else
  bad "the served object matches an uncached compile"
fi

# The other half of the bargain: flags that merely look like link flags must
# still key. -shared implies PIC on some targets and -pthread defines
# _REENTRANT, so neither is safe to drop however inert it looks here.
reset_cache
( cd "$WORK/link" && VCACHE_ROOTS="$WORK/link=proj" \
    "$VCACHE" gcc -O2 -c l.c -o "$WORK/p1.o" ) 2>/dev/null
( cd "$WORK/link" && VCACHE_ROOTS="$WORK/link=proj" \
    "$VCACHE" gcc -O2 -pthread -c l.c -o "$WORK/p2.o" ) 2>/dev/null
check "-pthread is still a separate entry" "$(misses)" "2"
( cd "$WORK/link" && VCACHE_ROOTS="$WORK/link=proj" \
    "$VCACHE" gcc -O2 -static -c l.c -o "$WORK/p3.o" ) 2>/dev/null
check "-static is still a separate entry" "$(misses)" "3"

# --------------------------------------------------------------------------
section "16. clang"

if command -v clang >/dev/null 2>&1; then
  make_project "$WORK/clang-a"
  make_project "$WORK/clang-b"

  reset_cache
  ( cd "$WORK/clang-a" && VCACHE_ROOTS="$WORK/clang-a=proj" \
      "$VCACHE" clang -g -O2 -c -I include src/lib.cc -o "$WORK/ca.o" ) 2>/dev/null
  check "clang compile is a miss" "$(misses)" "1"
  ( cd "$WORK/clang-b" && VCACHE_ROOTS="$WORK/clang-b=proj" \
      "$VCACHE" clang -g -O2 -c -I include src/lib.cc -o "$WORK/cb.o" ) 2>/dev/null
  check "clang hits across checkouts" "$(hits)" "1"
  if cmp -s "$WORK/ca.o" "$WORK/cb.o"; then
    ok "clang objects are byte-identical across checkouts"
  else
    bad "clang objects are byte-identical across checkouts"
  fi

  # clang and gcc must not share an entry: same source, same flags, different
  # code generator.
  reset_cache
  ( cd "$WORK/clang-a" && VCACHE_ROOTS="$WORK/clang-a=proj" \
      "$VCACHE" clang -O2 -c -I include src/lib.cc -o "$WORK/cc1.o" ) 2>/dev/null
  ( cd "$WORK/clang-a" && VCACHE_ROOTS="$WORK/clang-a=proj" \
      "$VCACHE" g++ -O2 -c -I include src/lib.cc -o "$WORK/gg1.o" ) 2>/dev/null
  if [[ "$(clang --version 2>/dev/null | head -1)" == "$(gcc --version 2>/dev/null | head -1)" ]]; then
    # macOS ships one compiler under both names, so sharing the entry is right.
    skipped "clang and gcc are the same compiler here"
  else
    check "clang and gcc do not share an entry" "$(misses)" "2"
  fi

  # The Firedancer sequence sccache could not cache. It must cache here, and
  # still hit from a different directory.
  reset_cache
  ( cd "$WORK/clang-a" && VCACHE_ROOTS="$WORK/clang-a=proj" \
      "$VCACHE" clang -O2 -c -I include src/lib.cc -o "$WORK/fq1.o" \
      -Xclang -target-feature -Xclang +fast-vector-fsqrt ) 2>/dev/null
  check "an -Xclang target-feature compile is cacheable" "$(uncacheable)" "0"
  check "and is a miss the first time" "$(misses)" "1"
  ( cd "$WORK/clang-b" && VCACHE_ROOTS="$WORK/clang-b=proj" \
      "$VCACHE" clang -O2 -c -I include src/lib.cc -o "$WORK/fq2.o" \
      -Xclang -target-feature -Xclang +fast-vector-fsqrt ) 2>/dev/null
  check "and hits from another checkout" "$(hits)" "1"
  if cmp -s "$WORK/fq1.o" "$WORK/fq2.o"; then
    ok "-Xclang objects are byte-identical across checkouts"
  else
    bad "-Xclang objects are byte-identical across checkouts"
  fi

  # A different -Xclang value must be a different entry, or the flag would be
  # silently dropped from the key.
  ( cd "$WORK/clang-a" && VCACHE_ROOTS="$WORK/clang-a=proj" \
      "$VCACHE" clang -O2 -c -I include src/lib.cc -o "$WORK/fq3.o" \
      -Xclang -target-feature -Xclang +avx2 ) 2>/dev/null
  check "a different -Xclang value is a separate entry" "$(misses)" "2"

  # clang diagnostics must replay on a hit, with local paths.
  reset_cache
  printf 'int unused_thing(void){int x; return 0;}\n' > "$WORK/clang-a/src/warn.c"
  cp "$WORK/clang-a/src/warn.c" "$WORK/clang-b/src/warn.c"
  ( cd "$WORK/clang-a" && VCACHE_ROOTS="$WORK/clang-a=proj" \
      "$VCACHE" clang -Wall -c src/warn.c -o "$WORK/w1.o" ) 2>"$WORK/w1.err"
  ( cd "$WORK/clang-b" && VCACHE_ROOTS="$WORK/clang-b=proj" \
      "$VCACHE" clang -Wall -c src/warn.c -o "$WORK/w2.o" ) 2>"$WORK/w2.err"
  check "the warning compile hits on the second checkout" "$(hits)" "1"
  if grep -q 'unused' "$WORK/w2.err"; then
    ok "clang diagnostics are replayed on a hit"
  else
    bad "clang diagnostics are replayed on a hit"
  fi
  if grep -q 'clang-a' "$WORK/w2.err"; then
    bad "replayed clang diagnostics leak the other checkout's path"
  else
    ok "replayed clang diagnostics carry no foreign path"
  fi

  # Dependency files, which clang formats slightly differently from gcc.
  reset_cache
  ( cd "$WORK/clang-a" && VCACHE_ROOTS="$WORK/clang-a=proj" \
      "$VCACHE" clang -MMD -MF "$WORK/cd1.d" -c -I include src/lib.cc \
      -o "$WORK/cd1.o" ) 2>/dev/null
  ( cd "$WORK/clang-b" && VCACHE_ROOTS="$WORK/clang-b=proj" \
      "$VCACHE" clang -MMD -MF "$WORK/cd2.d" -c -I include src/lib.cc \
      -o "$WORK/cd2.o" ) 2>/dev/null
  check "clang -MMD hits across checkouts" "$(hits)" "1"
  if grep -q '/vcache/proj' "$WORK/cd2.d"; then
    bad "replayed clang depfile leaked canonical paths"
  else
    ok "replayed clang depfile contains no canonical paths"
  fi

  # Split DWARF writes the debug info to a companion .dwo. Caching the object
  # alone leaves it referencing debug info that was never written -- and the
  # object still links, so nothing complains until someone opens a debugger.
  reset_cache
  ( cd "$WORK/clang-a" && VCACHE_ROOTS="$WORK/clang-a=proj" \
      "$VCACHE" clang -g -gsplit-dwarf -c src/warn.c -o "$WORK/sd1.o" ) 2>/dev/null
  check "clang -gsplit-dwarf is not cached" "$(uncacheable)" "1"
  rm -f "$WORK/sd1.dwo" "$WORK/clang-a/warn.dwo"
  ( cd "$WORK/clang-a" && VCACHE_ROOTS="$WORK/clang-a=proj" \
      "$VCACHE" clang -g -gsplit-dwarf -c src/warn.c -o "$WORK/sd1.o" ) 2>/dev/null
  if [[ "$(uname -s)" == Darwin ]]; then
    # Mach-O has no .dwo: clang accepts -gsplit-dwarf and emits nothing beside
    # the object, so there is no companion file to assert. Declining to cache
    # it is still the right answer and is checked above.
    skipped "split DWARF writes no companion file on Mach-O"
  else
    check "and a second compile still writes the .dwo" \
      "$([[ -f "$WORK/sd1.dwo" || -f "$WORK/clang-a/warn.dwo" ]] && echo yes)" "yes"
  fi

  # The embedding variant has no companion file, so it must stay cacheable.
  reset_cache
  ( cd "$WORK/clang-a" && VCACHE_ROOTS="$WORK/clang-a=proj" \
      "$VCACHE" clang -g -gsplit-dwarf=single -c src/warn.c -o "$WORK/sd2.o" ) 2>/dev/null
  check "clang -gsplit-dwarf=single is still cached" "$(uncacheable)" "0"
  check "and is a normal miss" "$(misses)" "1"

  # Side-output flags, in clang's own spellings.
  reset_cache
  ( cd "$WORK/clang-a" && VCACHE_ROOTS="$WORK/clang-a=proj" \
      "$VCACHE" clang -c src/warn.c -o "$WORK/tt.o" -MJ "$WORK/frag.json" ) 2>/dev/null
  check "clang -MJ is not cached" "$(uncacheable)" "1"
  check "and the fragment is produced" \
    "$([[ -f "$WORK/frag.json" ]] && echo yes)" "yes"

  # -Werror must not disable caching. vcache adds -fno-working-directory to the
  # preprocessing run for gcc's benefit; clang has no use for it and says so,
  # which under -Werror is an error, and every compile would quietly fall back
  # to running the compiler.
  reset_cache
  ( cd "$WORK/clang-a" && VCACHE_ROOTS="$WORK/clang-a=proj" \
      "$VCACHE" clang -Werror -O1 -c -I include src/lib.cc -o "$WORK/we1.o" ) 2>/dev/null
  check "clang -Werror still preprocesses" "$(stat_of 'preprocess failed')" "0"
  check "and is cached" "$(misses)" "1"
  ( cd "$WORK/clang-b" && VCACHE_ROOTS="$WORK/clang-b=proj" \
      "$VCACHE" clang -Werror -O1 -c -I include src/lib.cc -o "$WORK/we2.o" ) 2>/dev/null
  check "clang -Werror hits across checkouts" "$(hits)" "1"

  # A file named by a flag whose contents pick what code comes out. The
  # preprocessed text cannot stand in for it: clang reads the list in the middle
  # end, so both compiles below preprocess to exactly the same bytes.
  reset_cache
  # A memory access is what asan instruments, so this source is what makes the
  # two ignore lists produce visibly different objects.
  printf 'int probe(int *p){ p[0] = p[1] + 1; return p[0]; }\n' \
    > "$WORK/clang-a/src/ign.c"
  printf 'fun:*\n' > "$WORK/clang-a/ign.txt"
  ( cd "$WORK/clang-a" && VCACHE_ROOTS="$WORK/clang-a=proj" \
      "$VCACHE" clang -O1 -fsanitize=address -fsanitize-ignorelist=ign.txt \
      -c src/ign.c -o "$WORK/ig1.o" ) 2>/dev/null
  check "an -fsanitize-ignorelist compile is cacheable" "$(uncacheable)" "0"
  ( cd "$WORK/clang-a" && VCACHE_ROOTS="$WORK/clang-a=proj" \
      "$VCACHE" clang -O1 -fsanitize=address -fsanitize-ignorelist=ign.txt \
      -c src/ign.c -o "$WORK/ig2.o" ) 2>/dev/null
  check "an unchanged ignore list hits" "$(hits)" "1"

  printf 'fun:nothing_at_all\n' > "$WORK/clang-a/ign.txt"
  ( cd "$WORK/clang-a" && VCACHE_ROOTS="$WORK/clang-a=proj" \
      "$VCACHE" clang -O1 -fsanitize=address -fsanitize-ignorelist=ign.txt \
      -c src/ign.c -o "$WORK/ig3.o" ) 2>/dev/null
  check "editing the ignore list is a separate entry" "$(hits)" "1"
  if cmp -s "$WORK/ig1.o" "$WORK/ig3.o"; then
    bad "an edited ignore list served the old object"
  else
    ok "an edited ignore list produced a different object"
  fi

  # clang is not silent about linker flags under -c: it names the offending flag
  # in an "unused" warning, which vcache stores and replays. Sharing an entry
  # between -lm and -lz would report a flag the caller never passed, so unlike
  # gcc these have to stay in the key.
  reset_cache
  ( cd "$WORK/clang-a" && VCACHE_ROOTS="$WORK/clang-a=proj" \
      "$VCACHE" clang -O1 -lm -c src/warn.c -o "$WORK/ln1.o" ) 2>/dev/null
  ( cd "$WORK/clang-a" && VCACHE_ROOTS="$WORK/clang-a=proj" \
      "$VCACHE" clang -O1 -lz -c src/warn.c -o "$WORK/ln2.o" ) 2>/dev/null
  check "clang keys linker flags" "$(misses)" "2"
  ( cd "$WORK/clang-a" && VCACHE_ROOTS="$WORK/clang-a=proj" \
      "$VCACHE" clang -O1 -lm -c src/warn.c -o "$WORK/ln3.o" ) 2>"$WORK/ln3.err"
  check "and the repeat hits" "$(hits)" "1"
  if grep -q -- '-lm' "$WORK/ln3.err" && ! grep -q -- '-lz' "$WORK/ln3.err"; then
    ok "the replayed warning names the flag that was passed"
  else
    bad "the replayed warning names the flag that was passed"
  fi

  # Modules are declined rather than keyed: a .pcm names the modules it imports
  # in turn, so hashing the one file on the command line would not cover the
  # transitive set, and `import` is not expanded by the preprocessor the way
  # `#include` is.
  reset_cache
  printf 'export module M;\nexport constexpr int val() { return 111; }\n' \
    > "$WORK/clang-a/m.cppm"
  printf 'import M;\nint caller() { return val(); }\n' > "$WORK/clang-a/use.cc"
  if ( cd "$WORK/clang-a" && clang++ -std=c++20 --precompile m.cppm -o M.pcm ) 2>/dev/null; then
    ( cd "$WORK/clang-a" && VCACHE_ROOTS="$WORK/clang-a=proj" \
        "$VCACHE" clang++ -std=c++20 -O2 -fmodule-file=M=M.pcm -c use.cc \
        -o "$WORK/mod.o" ) 2>/dev/null
    check "-fmodule-file is not cached" "$(uncacheable)" "1"
    check "and the object is still produced" \
      "$([[ -s "$WORK/mod.o" ]] && echo yes)" "yes"
  else
    printf '  \033[33mSKIP\033[0m clang++ cannot precompile a C++20 module\n'
  fi
else
  printf '  \033[33mSKIP\033[0m clang not installed\n'
fi

# --------------------------------------------------------------------------
section "a compiler that lives inside the mapped tree"
# A project that builds part of itself with a compiler it just built -- LLVM
# compiling its own runtime libraries with the clang from this build -- puts the
# driver inside the source root. clang then reports that location in `-v`
# output ("InstalledDir:", and the repository it was configured from), which is
# what VCACHE_COMPILER_CHECK=version hashes. Hashing it verbatim gives two
# checkouts of one revision different compiler identities, so nothing shares.
reset_cache
for d in "$WORK/cc-one" "$WORK/a-considerably-longer-cc-two"; do
  make_project "$d"
  mkdir -p "$d/bin"
  cat > "$d/bin/mycc" <<EOF
#!/bin/sh
if [ "\$1" = "-v" ]; then
  echo "mycc version 1.0 (git://example/repo.git abcdef)" >&2
  echo "Target: x86_64-pc-linux-gnu" >&2
  echo "InstalledDir: $d/bin" >&2
  exit 0
fi
exec g++ "\$@"
EOF
  chmod +x "$d/bin/mycc"
done
( cd "$WORK/cc-one" && \
  "$VCACHE" --vcache-root="$PWD=proj" ./bin/mycc -g -O2 -I include -c src/lib.cc -o lib.o ) 2>/dev/null
( cd "$WORK/a-considerably-longer-cc-two" && \
  "$VCACHE" --vcache-root="$PWD=proj" ./bin/mycc -g -O2 -I include -c src/lib.cc -o lib.o ) 2>/dev/null
check "an in-tree compiler hits across checkouts" "$(hits)" "1"
check "and only one entry was stored" "$(disk_entries)" "1"
check "and the objects are byte-identical" \
  "$(cmp -s "$WORK/cc-one/lib.o" "$WORK/a-considerably-longer-cc-two/lib.o" \
     && echo same)" "same"

# The mapping must not paper over a genuinely different compiler: only the part
# of the banner that names a mapped path is normalised away.
reset_cache
sed_inplace 's/mycc version 1.0/mycc version 2.0/' "$WORK/a-considerably-longer-cc-two/bin/mycc"
( cd "$WORK/cc-one" && \
  "$VCACHE" --vcache-root="$PWD=proj" ./bin/mycc -g -O2 -I include -c src/lib.cc -o lib.o ) 2>/dev/null
( cd "$WORK/a-considerably-longer-cc-two" && \
  "$VCACHE" --vcache-root="$PWD=proj" ./bin/mycc -g -O2 -I include -c src/lib.cc -o lib.o ) 2>/dev/null
check "a different compiler version still misses" "$(misses)" "2"

# --------------------------------------------------------------------------
section "a compiler rebuilt in place is not answered from the old banner"
# `<compiler> -v` is memoised, because spawning it per compilation would be
# wasteful. If that memo is keyed only by the driver's path and size, a compiler
# rebuilt in place keeps the previous banner -- and relinking after a source
# change lands on the same byte count often enough that this is not a corner
# case. The result is a false HIT: objects from the old compiler served to the
# new one. Rebuilding must invalidate the memo.
reset_cache
mkdir -p "$WORK/rebuilt-cc"
make_project "$WORK/rebuilt-cc"
write_stub_cc() {  # $1 = version string, same length every time
  cat > "$WORK/rebuilt-cc/mycc" <<EOF
#!/bin/sh
if [ "\$1" = "-v" ]; then
  echo "mycc version $1 (git://example/repo.git abcdef)" >&2
  echo "Target: x86_64-pc-linux-gnu" >&2
  exit 0
fi
exec g++ "\$@"
EOF
  chmod +x "$WORK/rebuilt-cc/mycc"
}
write_stub_cc "1.0"
size_before=$(wc -c < "$WORK/rebuilt-cc/mycc" | tr -d " ")
( cd "$WORK/rebuilt-cc" && "$VCACHE" --vcache-root="$PWD=proj" \
    ./mycc -g -O2 -I include -c src/lib.cc -o lib.o ) 2>/dev/null
check "the first compile with the stub misses" "$(misses)" "1"

write_stub_cc "2.0"
check "the rebuilt stub is the same size" \
  "$(wc -c < "$WORK/rebuilt-cc/mycc" | tr -d " ")" "$size_before"
( cd "$WORK/rebuilt-cc" && "$VCACHE" --vcache-root="$PWD=proj" \
    ./mycc -g -O2 -I include -c src/lib.cc -o lib.o ) 2>/dev/null
check "a same-size rebuilt compiler is not served from the old memo" \
  "$(misses)" "2"
check "and it did not report a hit" "$(hits)" "0"

# --------------------------------------------------------------------------
section "link caching"

# Linux only. The tracer that discovers a link's input set needs LD_PRELOAD
# interposition, /proc/self/exe, /proc/self/fd and dl_iterate_phdr; vcache
# declines link caching elsewhere, so there is nothing here to assert.
if [[ "$(uname -s)" != Linux ]]; then
  printf '  \033[33mSKIP\033[0m link caching is Linux-only\n'
else

# A link has no preprocessed text to key on, so its input set is discovered by
# tracing. These cases are mostly about the discovered set being right in both
# directions: everything that matters is in it, and nothing that does not.

export VCACHE_LINK_CACHE=1

# The tracer is part of the correctness boundary, so exercise it directly:
# instrumentation must not change the errno seen by the tool, and a relative
# openat path must be resolved against its directory fd rather than the cwd.
mkdir -p "$WORK/tracer-openat/dir"
printf 'input\n' > "$WORK/tracer-openat/dir/value"
cat > "$WORK/tracer-openat/check.c" <<'EOF'
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
int main(void) {
  int missing = open("definitely-missing", O_RDONLY);
  if (missing >= 0 || errno != ENOENT) return 1;
  int dir = open("dir", O_RDONLY | O_DIRECTORY);
  if (dir < 0) return 2;
  int input = openat(dir, "value", O_RDONLY);
  if (input < 0) return 3;
  close(input);
  close(dir);
  return 0;
}
EOF
gcc "$WORK/tracer-openat/check.c" -o "$WORK/tracer-openat/check"
( cd "$WORK/tracer-openat" && \
  VCACHE_TRACE_LOG="$WORK/tracer-openat/trace" \
  LD_PRELOAD="$TOP/bin/vcache-fstrace.so" ./check )
tracer_rc=$?
check "the tracer preserves a failed call's errno" "$tracer_rc" "0"
check "openat records the directory-fd-resolved input" \
  "$(grep -F $'R\t'"$WORK/tracer-openat/dir/value" \
      "$WORK/tracer-openat/trace" >/dev/null && echo yes)" "yes"

make_link_project() {  # $1 = root, $2 = value returned by helper()
  mkdir -p "$1"
  cat > "$1/helper.c" <<EOF
int helper(void) { return $2; }
EOF
  cat > "$1/main.c" <<'EOF'
#include <stdio.h>
int helper(void);
int main(void) { printf("%d\n", helper()); return 0; }
EOF
  ( cd "$1" && gcc -c helper.c -o helper.o && gcc -c main.c -o main.o )
}

reset_cache
make_link_project "$WORK/link-one" 7
make_link_project "$WORK/a-considerably-longer-link-two" 7
for d in "$WORK/link-one" "$WORK/a-considerably-longer-link-two"; do
  ( cd "$d" && "$VCACHE" --vcache-root="$PWD=proj" \
      gcc -g -O2 helper.o main.o -o app ) 2>/dev/null
done
check "a link hits across checkouts" "$(hits)" "1"
check "and the binaries are byte-identical" \
  "$(cmp -s "$WORK/link-one/app" "$WORK/a-considerably-longer-link-two/app" \
     && echo same)" "same"
check "and the replayed binary runs" \
  "$("$WORK/a-considerably-longer-link-two/app")" "7"
check "and it is executable" \
  "$([[ -x "$WORK/a-considerably-longer-link-two/app" ]] && echo yes)" "yes"

# The property a positive-only cache cannot have. -lval resolves in late/ only
# because early/ does not contain it; installing a different library there must
# not be served the old entry.
reset_cache
mkdir -p "$WORK/ldsearch/early" "$WORK/ldsearch/late" "$WORK/ldsearch/proj"
( cd "$WORK/ldsearch/proj"
  cat > m.c <<'EOF'
#include <stdio.h>
int value(void);
int main(void) { printf("%d\n", value()); return 0; }
EOF
  printf 'int value(void){ return 111; }\n' > late.c
  printf 'int value(void){ return 999; }\n' > early.c
  gcc -c m.c -o m.o && gcc -c late.c -o late.o && gcc -c early.c -o early.o
  ar rcs "$WORK/ldsearch/late/libval.a" late.o
  ar rcs "$WORK/ldsearch/early-libval.a" early.o ) 2>/dev/null

link_val() {
  ( cd "$WORK/ldsearch/proj" && rm -f app && "$VCACHE" --vcache-root="$PWD=proj" \
      gcc m.o -L"$WORK/ldsearch/early" -L"$WORK/ldsearch/late" -lval -o app \
      && ./app ) 2>/dev/null
}
check "the first link resolves in the later -L directory" "$(link_val)" "111"
check "an unchanged repeat hits" "$(link_val >/dev/null; hits)" "1"
cp "$WORK/ldsearch/early-libval.a" "$WORK/ldsearch/early/libval.a"
check "a library appearing in an earlier -L directory is not served the old entry" \
  "$(link_val)" "999"
check "and that was a miss, not a hit" "$(misses)" "2"

# A changed input must not be served either, which the pre-key handles because
# the objects are named on the command line.
reset_cache
make_link_project "$WORK/link-changed" 1
( cd "$WORK/link-changed" && "$VCACHE" --vcache-root="$PWD=proj" \
    gcc helper.o main.o -o app ) 2>/dev/null
make_link_project "$WORK/link-changed" 2
( cd "$WORK/link-changed" && rm -f app && "$VCACHE" --vcache-root="$PWD=proj" \
    gcc helper.o main.o -o app && ./app ) > "$WORK/changed.out" 2>/dev/null
check "a changed object is not served the old binary" \
  "$(cat "$WORK/changed.out")" "2"

# Flags whose output is not a function of the inputs, and shapes that belong to
# the compile path, must be declined rather than cached.
reset_cache
( cd "$WORK/link-one" && "$VCACHE" --vcache-root="$PWD=proj" \
    gcc helper.o main.o -Wl,--build-id=uuid -o app-uuid ) 2>/dev/null
check "--build-id=uuid is not cached" "$(uncacheable)" "1"
check "and the binary is still produced" \
  "$([[ -x "$WORK/link-one/app-uuid" ]] && echo yes)" "yes"

reset_cache
( cd "$WORK/link-one" && "$VCACHE" --vcache-root="$PWD=proj" \
    gcc helper.c main.c -o app-src ) 2>/dev/null
check "compiling and linking in one step is not cached as a link" \
  "$(uncacheable)" "1"
check "and that binary is produced too" \
  "$([[ -x "$WORK/link-one/app-src" ]] && echo yes)" "yes"

# Without the tracer there is no absent set, so a hit cannot be shown to be
# sound. That must decline, not cache on trust.
reset_cache
( cd "$WORK/link-one" && VCACHE_TRACER=/nonexistent/tracer.so \
    "$VCACHE" --vcache-root="$PWD=proj" gcc helper.o main.o -o app-notrace ) 2>/dev/null
check "a missing tracer makes the link uncacheable" "$(uncacheable)" "1"
check "and the link still happens" \
  "$([[ -x "$WORK/link-one/app-notrace" ]] && echo yes)" "yes"

# A flag that makes the link write a second file: replaying the binary without
# it would look successful and leave the map file missing.
reset_cache
( cd "$WORK/link-one" && "$VCACHE" --vcache-root="$PWD=proj" \
    gcc helper.o main.o -Wl,-Map=link.map -o app-map ) 2>/dev/null
( cd "$WORK/link-one" && rm -f app-map link.map && "$VCACHE" --vcache-root="$PWD=proj" \
    gcc helper.o main.o -Wl,-Map=link.map -o app-map ) 2>/dev/null
check "a link with a map file hits" "$(hits)" "1"
check "and the map file is replayed, not silently skipped" \
  "$([[ -s "$WORK/link-one/link.map" ]] && echo yes)" "yes"

# The comma and -Xlinker spellings are equally real driver interfaces. Missing
# either output would let a hit return success while leaving a stale map behind.
reset_cache
( cd "$WORK/link-one" && "$VCACHE" --vcache-root="$PWD=proj" \
    gcc helper.o main.o -Wl,-Map,comma.map -o app-comma ) 2>/dev/null
( cd "$WORK/link-one" && rm -f app-comma comma.map && \
    "$VCACHE" --vcache-root="$PWD=proj" \
    gcc helper.o main.o -Wl,-Map,comma.map -o app-comma ) 2>/dev/null
check "the comma-form map link hits" "$(hits)" "1"
check "and its map file is replayed" \
  "$([[ -s "$WORK/link-one/comma.map" ]] && echo yes)" "yes"

reset_cache
( cd "$WORK/link-one" && "$VCACHE" --vcache-root="$PWD=proj" \
    gcc helper.o main.o -Xlinker -Map -Xlinker xlinker.map -o app-xlinker ) 2>/dev/null
( cd "$WORK/link-one" && rm -f app-xlinker xlinker.map && \
    "$VCACHE" --vcache-root="$PWD=proj" \
    gcc helper.o main.o -Xlinker -Map -Xlinker xlinker.map -o app-xlinker ) 2>/dev/null
check "the -Xlinker map link hits" "$(hits)" "1"
check "and its map file is replayed" \
  "$([[ -s "$WORK/link-one/xlinker.map" ]] && echo yes)" "yes"

# Out-of-band outputs still carry their digest in the checked blob. Corrupting
# the sidecar must be a miss, never a successful hit returning corrupt bytes.
reset_cache
( cd "$WORK/link-one" && "$VCACHE" --vcache-root="$PWD=proj" \
    gcc helper.o main.o -o app-verified ) 2>/dev/null
sidecar=$(find "$VCACHE_DIR" -type f -name '*.linkout' -print -quit)
printf 'corrupt\n' > "$sidecar"
( cd "$WORK/link-one" && rm -f app-verified && \
    "$VCACHE" --vcache-root="$PWD=proj" gcc helper.o main.o -o app-verified ) 2>/dev/null
check "a corrupt link sidecar is not served as a hit" "$(hits)" "0"
check "and the real linker repairs it on a miss" \
  "$("$WORK/link-one/app-verified")" "7"

# Read-only applies to the sidecars written outside Storage::Put too.
reset_cache
( cd "$WORK/link-one" && VCACHE_READONLY=1 \
    "$VCACHE" --vcache-root="$PWD=proj" gcc helper.o main.o -o app-readonly ) 2>/dev/null
check "read-only link caching writes no sidecar" \
  "$(find "$VCACHE_DIR" -type f -name '*.linkout' 2>/dev/null | wc -l | tr -d " ")" "0"

# The two guards that decide whether an entry is sound enough to store are
# worth breaking on purpose. A guard that has never been seen to fire is
# indistinguishable from one that cannot.
#
# Both are driven by a stub tracer, so what is under test is vcache's reaction
# to a degraded trace rather than the real tracer's behaviour.
if cc_stub=$(command -v gcc || command -v cc) && [[ -n "$cc_stub" ]]; then
  cat > "$WORK/stub-tracer.c" <<'STUBEOF'
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
__attribute__((constructor)) static void stub(void) {
  const char *log = getenv("VCACHE_TRACE_LOG");
  const char *mode = getenv("STUB_MODE");
  if (log == NULL || mode == NULL) return;
  if (strcmp(mode, "incomplete") == 0) {
    /* Write a trace that is otherwise complete and usable -- including a
       process record, so the empty-tools guard is not what rejects it -- and
       then raise the marker. The marker must be the only reason this is not
       stored, or the test proves nothing. */
    int fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) {
      (void)!write(fd, "P\t/bin/sh\nR\t/etc/hostname\n", 27);
      close(fd);
    }
    char marker[4096];
    snprintf(marker, sizeof marker, "%s.incomplete", log);
    int mfd = open(marker, O_WRONLY | O_CREAT, 0600);
    if (mfd >= 0) close(mfd);
    return;
  }
  if (strcmp(mode, "noprocs") == 0) {
    /* Reads, but never announces a process: the input set may be partial. */
    int fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) { (void)!write(fd, "R\t/etc/hostname\n", 16); close(fd); }
  }
}
STUBEOF
  if "$cc_stub" -fPIC -shared -o "$WORK/stub-tracer.so" "$WORK/stub-tracer.c" 2>/dev/null; then
    reset_cache
    ( cd "$WORK/link-one" && STUB_MODE=incomplete VCACHE_TRACER="$WORK/stub-tracer.so" \
        "$VCACHE" --vcache-root="$PWD=proj" gcc helper.o main.o -o app-inc ) 2>/dev/null
    check "an incomplete trace is not stored" "$(disk_entries)" "0"
    check "and it is counted uncacheable, not silently dropped" "$(uncacheable)" "1"
    check "and the binary is still produced" \
      "$([[ -x "$WORK/link-one/app-inc" ]] && echo yes)" "yes"

    reset_cache
    ( cd "$WORK/link-one" && STUB_MODE=noprocs VCACHE_TRACER="$WORK/stub-tracer.so" \
        "$VCACHE" --vcache-root="$PWD=proj" gcc helper.o main.o -o app-np ) 2>/dev/null
    check "a trace naming no processes is not stored" "$(disk_entries)" "0"
    check "and that is counted uncacheable too" "$(uncacheable)" "1"
  else
    printf '  \033[33mSKIP\033[0m stub tracer would not compile\n'
  fi
fi

# The tracer observes a link; it is not part of it. Recording itself would make
# every rebuild of vcache invalidate every link entry, for a file that cannot
# affect the output -- and it would bite hardest while developing vcache, which
# is exactly when the cache is wanted.
tracer_so="$(dirname "$VCACHE")/vcache-fstrace.so"
if [[ -f "$tracer_so" ]]; then
  rm -f "$WORK/self-trace.log"
  ( cd "$WORK/link-one" && VCACHE_TRACE_LOG="$WORK/self-trace.log" \
      LD_PRELOAD="$tracer_so" gcc helper.o main.o -o app-self ) 2>/dev/null
  check "the tracer does not record itself as an input" \
    "$(grep -c 'vcache-fstrace' "$WORK/self-trace.log" || true)" "0"
  # ...but it must still record the libraries that are part of the linker: for
  # ld.bfd the implementation lives in libbfd, and libz/libzstd emit output
  # bytes for compressed debug sections.
  check "and it still records the other loaded libraries" \
    "$(awk -F'\t' '$1=="R"{print $2}' "$WORK/self-trace.log" |
       grep -cE '\.so($|\.)' | awk '{print ($1 > 0) ? "yes" : "no"}')" "yes"
fi

# Link caching is off unless asked for.
reset_cache
( cd "$WORK/link-one" && env -u VCACHE_LINK_CACHE "$VCACHE" \
    --vcache-root="$PWD=proj" gcc helper.o main.o -o app-off ) 2>/dev/null
check "link caching is off by default" "$(disk_entries)" "0"

unset VCACHE_LINK_CACHE
fi

# --------------------------------------------------------------------------
section "17. kbuild-shaped command lines"

# The Linux kernel never spells the dependency flags the way the driver does.
# Every compile it runs carries -Wp,-MMD,<file>, and the host tools it builds
# under tools/ add -Wp,-MT,<target> with both paths absolute. Treating -Wp, as
# one opaque flag put those paths in the cache key, which is enough on its own
# to stop two checkouts of the same tree ever sharing an entry.

reset_cache
for dir in "$WORK/kb-one" "$WORK/kb-two"; do
  mkdir -p "$dir/sub"
  printf '#include <h.h>\nint f(void){return X;}\n' > "$dir/sub/a.c"
  printf '#define X 1\n' > "$dir/sub/h.h"
done

( cd "$WORK/kb-one" && VCACHE_ROOTS="$PWD=proj" "$VCACHE" gcc \
    "-Wp,-MMD,$PWD/sub/.a.o.d" "-Wp,-MT,$PWD/sub/a.o" -I "$PWD/sub" \
    -c sub/a.c -o "$PWD/sub/a.o" )
check "a kbuild-shaped compile is cached" "$(misses)" "1"
check "and the dependency file is written" \
  "$([[ -s "$WORK/kb-one/sub/.a.o.d" ]] && echo yes)" "yes"

( cd "$WORK/kb-two" && VCACHE_ROOTS="$PWD=proj" "$VCACHE" gcc \
    "-Wp,-MMD,$PWD/sub/.a.o.d" "-Wp,-MT,$PWD/sub/a.o" -I "$PWD/sub" \
    -c sub/a.c -o "$PWD/sub/a.o" )
check "and the same compile in another directory hits" "$(hits)" "1"
check "with a byte-identical object" \
  "$(cmp -s "$WORK/kb-one/sub/a.o" "$WORK/kb-two/sub/a.o" && echo same)" "same"

# The depfile has to come back on the hit as well -- kbuild feeds it straight
# to fixdep, which fails outright if it is missing -- and it has to name paths
# in *this* tree, not the one the entry was stored from.
check "the dependency file is replayed on the hit" \
  "$([[ -s "$WORK/kb-two/sub/.a.o.d" ]] && echo yes)" "yes"
check "naming this tree's target" \
  "$(grep -cF "$WORK/kb-two/sub/a.o" "$WORK/kb-two/sub/.a.o.d")" "1"
check "and this tree's prerequisites" \
  "$(grep -cF "$WORK/kb-two/sub/h.h" "$WORK/kb-two/sub/.a.o.d")" "1"
check "with nothing left pointing at the other tree" \
  "$(grep -cF "$WORK/kb-one" "$WORK/kb-two/sub/.a.o.d")" "0"

# A compile that fails still owes a dependency file: gcc writes it during
# preprocessing, before the error, and kbuild feeds it to fixdep either way.
# The kernel's lib/test_fortify targets compile code that is meant not to
# build, so a missing .d there stops the build outright.
reset_cache
mkdir -p "$WORK/kb-fail"
printf '#include <stdlib.h>\nthis is not c;\n' > "$WORK/kb-fail/bad.c"
( cd "$WORK/kb-fail" && VCACHE_ROOTS="$PWD=proj" "$VCACHE" gcc \
    "-Wp,-MMD,$PWD/.bad.o.d" -c bad.c -o bad.o ) 2>/dev/null
check "a failed compile still reports the compiler's status" "$?" "1"
check "and still writes its dependency file" \
  "$([[ -s "$WORK/kb-fail/.bad.o.d" ]] && echo yes)" "yes"
check "but leaves no object behind" \
  "$([[ -e "$WORK/kb-fail/bad.o" ]] && echo yes || echo no)" "no"

# --------------------------------------------------------------------------
section "18. .incbin is not cached"

# .incbin tells the *assembler* to splice a file in verbatim. Neither its name
# nor its contents reach the preprocessed text, so two compilations that
# preprocess identically can legitimately owe different objects. The kernel
# relies on this -- kernel/kheaders.c embeds a tar of the tree's headers -- and
# caching it serves an object holding some other build's payload.

# The directive itself is portable; `.pushsection .rodata, "a"` is not -- that
# spelling is ELF's, and a Mach-O assembler rejects it. The decline is decided
# from the preprocessed text and has nothing platform-specific in it, and
# ContainsIncbin is covered by the unit tests everywhere, so skipping the
# end-to-end case off ELF loses nothing that is not checked elsewhere.
if [[ "$(uname -s)" != Linux ]]; then
  printf '  \033[33mSKIP\033[0m .incbin end-to-end needs ELF section syntax\n'
else

reset_cache
mkdir -p "$WORK/incbin"
printf 'payload-one' > "$WORK/incbin/payload.bin"
cat > "$WORK/incbin/e.c" <<'INCBIN_EOF'
asm("  .pushsection .rodata, \"a\"\n"
    "  .globl blob\n"
    "blob:\n"
    "  .incbin \"payload.bin\"\n"
    "  .popsection\n");
extern char blob[];
char* get(void) { return blob; }
INCBIN_EOF

( cd "$WORK/incbin" && VCACHE_ROOTS="$PWD=proj" "$VCACHE" gcc -c e.c -o e.o )
check ".incbin is declined" "$(uncacheable)" "1"
check "and nothing is stored for it" "$(disk_entries)" "0"

# The point of declining it: change only the payload, which the preprocessed
# text cannot see, and the object must still follow.
printf 'payload-two' > "$WORK/incbin/payload.bin"
( cd "$WORK/incbin" && VCACHE_ROOTS="$PWD=proj" "$VCACHE" gcc -c e.c -o e.o )
check "a changed payload is picked up" \
  "$(strings "$WORK/incbin/e.o" | grep -c 'payload-two')" "1"
check "and the stale one is gone" \
  "$(strings "$WORK/incbin/e.o" | grep -c 'payload-one')" "0"

# The same source without the directive caches normally, so the check above is
# about .incbin and not about this file being unusual.
reset_cache
printf 'int g(void){return 2;}\n' > "$WORK/incbin/p.c"
( cd "$WORK/incbin" && VCACHE_ROOTS="$PWD=proj" "$VCACHE" gcc -c p.c -o p.o )
check "a file without .incbin is still cached" "$(misses)" "1"
check "and is not counted uncacheable" "$(uncacheable)" "0"

fi

# --------------------------------------------------------------------------
section "precompiled headers"

# A precompiled header records the absolute paths and mtimes of its inputs, so
# generating one is declined; a stored copy would be wrong in another directory.
# Using one is cacheable only while the preprocessed text still expands the
# header, which is what each compiler block below pins down.
make_pch_tree() {
  mkdir -p "$1"
  printf '#define FOO 1\nstruct S { int x; };\n' > "$1/h.h"
  printf 'int f() { S s{FOO}; return s.x; }\n' > "$1/main.cc"
}

if command -v clang++ >/dev/null 2>&1; then
  reset_cache
  make_pch_tree "$WORK/pch-clang-a"
  make_pch_tree "$WORK/pch-clang-b"
  # clang rejects a PCH built at another -O level, so both steps pass -O2.
  for d in "$WORK/pch-clang-a" "$WORK/pch-clang-b"; do
    ( cd "$d" && VCACHE_ROOTS="$d=proj" \
        "$VCACHE" clang++ -O2 -x c++-header -c h.h -Xclang -emit-pch -o h.pch ) 2>/dev/null
  done
  check "clang PCH generation is declined" "$(uncacheable)" "2"
  check "and the PCH is still written" \
    "$([[ -s "$WORK/pch-clang-a/h.pch" && -s "$WORK/pch-clang-b/h.pch" ]] && echo yes)" "yes"

  # clang -E re-emits the PCH's header text, so the key covers the header even
  # though the .pch bytes never reach it.
  for d in "$WORK/pch-clang-a" "$WORK/pch-clang-b"; do
    ( cd "$d" && VCACHE_ROOTS="$d=proj" "$VCACHE" clang++ -O2 -c \
        -Xclang -include-pch -Xclang "$d/h.pch" main.cc -o main.o ) 2>/dev/null
  done
  check "a compile using the PCH is cached" "$(misses)" "1"
  check "and hits from another directory" "$(hits)" "1"
  if cmp -s "$WORK/pch-clang-a/main.o" "$WORK/pch-clang-b/main.o"; then
    ok "PCH objects are byte-identical across directories"
  else
    bad "PCH objects are byte-identical across directories"
  fi

  # The driver spelling takes its value as a separate argument, which must not
  # be read as a second input file.
  for d in "$WORK/pch-clang-a" "$WORK/pch-clang-b"; do
    ( cd "$d" && VCACHE_ROOTS="$d=proj" "$VCACHE" clang++ -O2 -c \
        -include-pch "$d/h.pch" main.cc -o driver.o ) 2>/dev/null
  done
  check "the driver's -include-pch spelling is cached too" "$(uncacheable)" "2"
  check "and hits from another directory" "$(hits)" "2"

  # An edited header leaves the PCH stale: clang refuses it, so vcache must not
  # answer from an entry either.
  pa="$WORK/pch-clang-a"
  hits_before=$(hits)
  printf '#define FOO 1\nstruct S { int x; int y; };\n' > "$pa/h.h"
  if ( cd "$pa" && VCACHE_ROOTS="$pa=proj" "$VCACHE" clang++ -O2 -c \
         -Xclang -include-pch -Xclang "$pa/h.pch" main.cc -o stale.o ) 2>/dev/null; then
    bad "a stale PCH still fails the compile"
  else
    ok "a stale PCH still fails the compile"
  fi
  check "and is not answered from the cache" "$(hits)" "$hits_before"
  ( cd "$pa" && VCACHE_ROOTS="$pa=proj" \
      "$VCACHE" clang++ -O2 -x c++-header -c h.h -Xclang -emit-pch -o h.pch ) 2>/dev/null
  misses_before=$(misses)
  ( cd "$pa" && VCACHE_ROOTS="$pa=proj" "$VCACHE" clang++ -O2 -c \
      -Xclang -include-pch -Xclang "$pa/h.pch" main.cc -o main.o ) 2>/dev/null
  check "a rebuilt PCH for the edited header is a new key" \
    "$(misses)" "$((misses_before + 1))"
else
  skipped "clang++ is not installed"
fi

if command -v g++ >/dev/null 2>&1 && ! g++ --version 2>/dev/null | head -1 | grep -qi clang; then
  reset_cache
  make_pch_tree "$WORK/pch-gcc-a"
  make_pch_tree "$WORK/pch-gcc-b"
  for d in "$WORK/pch-gcc-a" "$WORK/pch-gcc-b"; do
    ( cd "$d" && VCACHE_ROOTS="$d=proj" \
        "$VCACHE" g++ -O2 -x c++-header -c h.h -o h.h.gch ) 2>/dev/null
  done
  check "gcc .gch generation is declined" "$(uncacheable)" "2"
  # Without this the checks below would pass on a textual include alone.
  check "and gcc picks the .gch up" \
    "$(cd "$WORK/pch-gcc-a" && g++ -O2 -H -c -include h.h main.cc -o /dev/null 2>&1 \
         | grep -c '^! .*h\.h\.gch')" "1"

  # gcc -E expands the header textually even when a valid .gch sits beside it.
  for d in "$WORK/pch-gcc-a" "$WORK/pch-gcc-b"; do
    ( cd "$d" && VCACHE_ROOTS="$d=proj" \
        "$VCACHE" g++ -O2 -c -include h.h main.cc -o main.o ) 2>/dev/null
  done
  check "a compile using the .gch is cached" "$(misses)" "1"
  check "and hits from another directory" "$(hits)" "1"
  if cmp -s "$WORK/pch-gcc-a/main.o" "$WORK/pch-gcc-b/main.o"; then
    ok ".gch objects are byte-identical across directories"
  else
    bad ".gch objects are byte-identical across directories"
  fi

  # -fpch-preprocess leaves only a pragma naming the .gch, so two different
  # .gch files would preprocess identically.
  pg="$WORK/pch-gcc-a"
  if ( cd "$pg" && VCACHE_ROOTS="$pg=proj" "$VCACHE" g++ -O2 -c -fpch-preprocess \
         -include h.h main.cc -o pp.o ) 2>/dev/null && [[ -s "$pg/pp.o" ]]; then
    ok "-fpch-preprocess still compiles"
  else
    bad "-fpch-preprocess still compiles"
  fi
  check "and is declined" "$(uncacheable)" "3"
else
  skipped "g++ is not installed, or is clang"
fi

# --------------------------------------------------------------------------
section "--show-stats breaks decisions down by reason"

# The positional counters say how many invocations were declined; the reason
# rows say which rule declined them, under the same names the log uses.
reason_row() { printf '  %-20s%s' "$1" "$2"; }
uncacheable_block() {
  "$VCACHE" --show-stats | awk '/^uncacheable /{ shown = 1; print; next }
                                shown && /^  /{ print; next }
                                { shown = 0 }'
}

reset_cache
mkdir -p "$WORK/reasons"
printf 'int r(void){return 3;}\nint main(void){return r() - 3;}\n' > "$WORK/reasons/r.c"
"$VCACHE" --zero-stats >/dev/null
( cd "$WORK/reasons" && VCACHE_ROOTS="$WORK/reasons=proj" "$VCACHE" gcc -c r.c -o r.o ) 2>/dev/null
( cd "$WORK/reasons" && VCACHE_ROOTS="$WORK/reasons=proj" "$VCACHE" gcc -c r.c -o r.o ) 2>/dev/null
( cd "$WORK/reasons" && "$VCACHE" cc r.o -o r ) 2>/dev/null
( cd "$WORK/reasons" && "$VCACHE" gcc -E r.c -o r.i ) 2>/dev/null
expected_uncacheable=2
expected_block="$(printf 'uncacheable         2\n%s\n%s' \
  "$(reason_row link 1)" "$(reason_row 'preprocess only' 1)")"
if command -v rustc >/dev/null 2>&1; then
  printf 'pub fn f() -> i32 { 3 }\n' > "$WORK/reasons/lib.rs"
  ( cd "$WORK/reasons" && "$VCACHE" rustc --crate-name reasons --crate-type lib \
      --emit=link lib.rs ) 2>/dev/null
  expected_uncacheable=3
  expected_block="$(printf 'uncacheable         3\n%s\n%s\n%s' "$(reason_row link 1)" \
    "$(reason_row 'no --out-dir' 1)" "$(reason_row 'preprocess only' 1)")"
else
  skipped "rustc without --out-dir (no rustc on PATH)"
fi

check "the compile missed once" "$(misses)" "1"
check "and hit once" "$(hits)" "1"
check "and stored one entry" "$(stat_of 'entries stored')" "1"
check "the link, -E and rustc runs are uncacheable" "$(uncacheable)" "$expected_uncacheable"
check "nothing failed to preprocess" "$(stat_of 'preprocess failed')" "0"
check "each uncacheable run is listed under its reason" "$(uncacheable_block)" "$expected_block"
check "no passthrough row when nothing fell back" \
  "$("$VCACHE" --show-stats | grep -c '^passthrough' || true)" "0"
check "the stats file keeps the nine positional lines first" \
  "$(head -9 "$VCACHE_DIR/stats" | grep -cE '^[0-9]+$')" "9"
check "followed by one reason line per reason" \
  "$(tail -n +10 "$VCACHE_DIR/stats" | grep -cE $'^reason\t[^\t]+\t[0-9]+$')" \
  "$((expected_uncacheable))"
"$VCACHE" --zero-stats >/dev/null
check "--zero-stats clears the reasons" "$("$VCACHE" --show-stats | grep -c '^  ' || true)" "0"

section "10a. Compile sessions"
if command -v python3 >/dev/null 2>&1; then
  python3 "$TOP/tests/daemon_session_test.py" "$VCACHE" "$WORK/compile-sessions" \
    > "$WORK/compile-sessions.report" 2> "$WORK/compile-sessions.errors"
  session_test_exit=$?
  while IFS='|' read -r outcome message; do
    if [[ "$outcome" == PASS ]]; then ok "$message"; else bad "$message"; fi
  done < "$WORK/compile-sessions.report"
  check "compile session integration driver completes" "$session_test_exit" "0"
  if [[ "$session_test_exit" != 0 ]]; then cat "$WORK/compile-sessions.errors"; fi
else
  bad "compile session integration requires python3"
fi

section "10b. Single-flight"
if command -v python3 >/dev/null 2>&1; then
  timeout 120 python3 "$TOP/tests/daemon_lease_test.py" "$VCACHE" "$WORK/single-flight" \
    > "$WORK/single-flight.report" 2> "$WORK/single-flight.errors"
  lease_test_exit=$?
  while IFS='|' read -r outcome message; do
    if [[ "$outcome" == PASS ]]; then ok "$message"; else bad "$message"; fi
  done < "$WORK/single-flight.report"
  check "single-flight integration driver completes" "$lease_test_exit" "0"
  if [[ "$lease_test_exit" != 0 ]]; then cat "$WORK/single-flight.errors"; fi
else
  bad "single-flight integration requires python3"
fi

section "10d. Elastic jobserver"
if command -v python3 >/dev/null 2>&1; then
  timeout 120 python3 "$TOP/tests/daemon_elastic_test.py" "$VCACHE" "$WORK/elastic" \
    "$TOP/bin/vcache_test" > "$WORK/elastic.report" 2> "$WORK/elastic.errors"
  elastic_test_exit=$?
  while IFS='|' read -r outcome message; do
    if [[ "$outcome" == PASS ]]; then ok "$message"; else bad "$message"; fi
  done < "$WORK/elastic.report"
  check "elastic jobserver integration driver completes" "$elastic_test_exit" "0"
  if [[ "$elastic_test_exit" != 0 ]]; then cat "$WORK/elastic.errors"; fi
else
  bad "elastic jobserver integration requires python3"
fi

section "10c. Memory admission"
if command -v python3 >/dev/null 2>&1; then
  timeout 120 python3 "$TOP/tests/daemon_admission_test.py" "$VCACHE" "$WORK/admission" \
    "$TOP/bin/vcache_test" > "$WORK/admission.report" 2> "$WORK/admission.errors"
  admission_test_exit=$?
  while IFS='|' read -r outcome message; do
    if [[ "$outcome" == PASS ]]; then ok "$message"; else bad "$message"; fi
  done < "$WORK/admission.report"
  check "memory admission integration driver completes" "$admission_test_exit" "0"
  if [[ "$admission_test_exit" != 0 ]]; then cat "$WORK/admission.errors"; fi
else
  bad "memory admission integration requires python3"
fi

# --------------------------------------------------------------------------
printf '\n\033[1mintegration: %d passed, %d failed\033[0m\n' "$PASS" "$FAIL"
[[ "$FAIL" -eq 0 ]]
