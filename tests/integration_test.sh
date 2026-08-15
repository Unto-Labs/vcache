#!/usr/bin/env bash
# End-to-end tests for vcache.
#
# The central claim under test is that the same source compiled from two
# unrelated directories produces one cache entry and byte-identical objects.
# Everything else here guards the paths that claim depends on.

set -uo pipefail

TOP="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VCACHE="$TOP/bin/vcache"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/vcache-it-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0

ok()   { printf '  \033[32mPASS\033[0m %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAIL=$((FAIL+1)); }
check() { if [[ "$2" == "$3" ]]; then ok "$1"; else bad "$1 (expected '$3', got '$2')"; fi; }
section() { printf '\n\033[1m%s\033[0m\n' "$1"; }

export VCACHE_DIR="$WORK/cache"

# Counter helpers read straight from --show-stats.
stat_of() { "$VCACHE" --show-stats | grep -F "$1" | awk '{print $NF}'; }
hits()   { stat_of "cache hit (disk)"; }
misses() { stat_of "cache miss"; }
uncacheable() { stat_of "uncacheable"; }
reset_cache() { rm -rf "$VCACHE_DIR"; }

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
check "second checkout hits the cache" "$(hits)" "1"

if cmp -s "$WORK/a.o" "$WORK/b.o"; then
  ok "objects from both checkouts are byte-identical"
else
  bad "objects from both checkouts are byte-identical"
fi

# The canonical prefix must be what actually landed in the debug info.
if readelf --debug-dump=info "$WORK/a.o" 2>/dev/null | grep -q "/vcache/proj"; then
  ok "debug info records the canonical path"
else
  bad "debug info records the canonical path"
fi
if readelf --debug-dump=info "$WORK/a.o" 2>/dev/null | grep -q "checkout-a"; then
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

check "out-of-tree build hits across checkouts" "$(hits)" "1"
if cmp -s "$WORK/build-a/out.o" "$WORK/build-b/out.o"; then
  ok "out-of-tree objects are byte-identical"
else
  bad "out-of-tree objects are byte-identical"
fi

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
if readelf --debug-dump=info "$WORK/p2.o" 2>/dev/null | grep -q "/somewhere"; then
  bad "override drops the caller's mapping"
else
  ok "override drops the caller's mapping"
fi
if readelf --debug-dump=info "$WORK/p2.o" 2>/dev/null | grep -q "/vcache/proj"; then
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
if readelf --debug-dump=info "$WORK/p6.o" 2>/dev/null | grep -q "/somewhere"; then
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
  objects_written=$(find "$S3DIR" -type f 2>/dev/null | wc -l)
  check "entry was uploaded to s3" "$objects_written" "1"

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

  # An unreachable remote must degrade to a miss, never break the build.
  kill "$S3PID" 2>/dev/null; wait "$S3PID" 2>/dev/null
  rm -rf "$VCACHE_DIR"
  ( cd "$WORK/checkout-a" && VCACHE_ROOTS="$WORK/checkout-a=proj" \
      "$VCACHE" g++ -O0 -c -I include src/lib.cc -o "$WORK/s4.o" ) 2>/dev/null
  rc=$?
  check "build succeeds when s3 is unreachable" "$rc" "0"
  check "object still produced with s3 down" "$([[ -s "$WORK/s4.o" ]] && echo yes)" "yes"

  unset AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY VCACHE_S3_BUCKET \
        VCACHE_S3_ENDPOINT VCACHE_S3_PATH_STYLE VCACHE_S3_REGION
else
  printf '  \033[33mSKIP\033[0m python3 not installed\n'
fi

# --------------------------------------------------------------------------
section "12. runtime dependencies stay minimal"

# vcache runs once per compilation, so every DT_NEEDED entry is mapped and
# relocated on every invocation. libcurl alone drags in ~30 shared objects.
needed="$(readelf -d "$VCACHE" 2>/dev/null | grep NEEDED)"
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
ldd_count="$(ldd "$VCACHE" 2>/dev/null | wc -l)"
if [[ "$ldd_count" -le 6 ]]; then
  ok "ldd closure is small ($ldd_count entries)"
else
  bad "ldd closure is small (got $ldd_count entries)"
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
sed -i '$ d' "$WORK/dep-a/inc/base.h"
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

# --------------------------------------------------------------------------
printf '\n\033[1mintegration: %d passed, %d failed\033[0m\n' "$PASS" "$FAIL"
[[ "$FAIL" -eq 0 ]]
