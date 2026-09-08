#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Unto Labs
# SPDX-License-Identifier: Apache-2.0
#
# Linux kernel integration test.
#
# Run by hand, not by `make test`: it downloads two kernel tarballs, wants
# ~10 GB of disk, and takes a few minutes even on a large machine. Everything
# it checks is a property that only a real kbuild exercises --
# `-Wp,-MMD,<file>` dependency generation, `.incbin`, out-of-tree builds, host
# tools under tools/ with absolute paths, and compiles that are meant to fail.
#
#   tests/linux_kernel_test.sh [options]
#
#   --work-dir DIR    where to download and build   (default: $TMPDIR/vcache-kernel-test)
#   --versions A,B    two kernel versions           (default: 7.0.12,7.0.13)
#   --config FILE     kernel .config to start from  (default: /boot/config-$(uname -r))
#   --targets "..."   make targets to build         (default: "kernel/ mm/")
#   --jobs N          make -j                       (default: nproc)
#   --keep            do not delete the work directory on success
#   --vcache PATH     the binary under test         (default: ../bin/vcache)
#
# The default target set is a subset of the tree, which is enough to reach every
# behaviour under test and keeps a run to a few minutes and ~10 GB. `--targets
# "all modules"` runs the same checks over the whole tree instead; it builds six
# complete kernels, so expect it to want on the order of 100 GB and the better
# part of an hour.
#
# See docs/linux-kernel.md for what the recipe is and why.

set -uo pipefail

TOP="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

WORK="${TMPDIR:-/tmp}/vcache-kernel-test"
VERSIONS="7.0.12,7.0.13"
CONFIG="/boot/config-$(uname -r)"
TARGETS="kernel/ mm/"
JOBS="$(nproc 2>/dev/null || echo 4)"
KEEP=0
VCACHE="$TOP/bin/vcache"

while (( $# )); do
  case "$1" in
    --work-dir) WORK="$2"; shift 2 ;;
    --versions) VERSIONS="$2"; shift 2 ;;
    --config)   CONFIG="$2"; shift 2 ;;
    --targets)  TARGETS="$2"; shift 2 ;;
    --jobs)     JOBS="$2"; shift 2 ;;
    --vcache)   VCACHE="$2"; shift 2 ;;
    --keep)     KEEP=1; shift ;;
    -h|--help)  awk 'NR>=5 && /^#/ { sub(/^# ?/, ""); print; next }
                     NR>=5 { exit }' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) printf 'unknown option: %s\n' "$1" >&2; exit 2 ;;
  esac
done

VER_A="${VERSIONS%%,*}"
VER_B="${VERSIONS##*,}"

PASS=0
FAIL=0
SKIP=0
ok()      { printf '  \033[32mPASS\033[0m %s\n' "$1"; PASS=$((PASS+1)); }
bad()     { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAIL=$((FAIL+1)); }
skipped() { printf '  \033[33mSKIP\033[0m %s\n' "$1"; SKIP=$((SKIP+1)); }
check()   { if [[ "$2" == "$3" ]]; then ok "$1"; else bad "$1 (expected '$3', got '$2')"; fi; }
section() { printf '\n\033[1m%s\033[0m\n' "$1"; }
note()    { printf '       %s\n' "$1"; }
die()     { printf '\033[31m%s\033[0m\n' "$1" >&2; exit 2; }

# ---------------------------------------------------------------------------
section "0. prerequisites"

[[ "$(uname -s)" == "Linux" ]] || die "this test builds a Linux kernel and needs a Linux host"
[[ -x "$VCACHE" ]] || die "no vcache binary at $VCACHE; run make first, or pass --vcache"
note "$("$VCACHE" --version | head -1)"

for t in curl tar make gcc bc flex bison objcopy; do
  command -v "$t" >/dev/null || die "missing build prerequisite: $t"
done
[[ -e /usr/include/openssl/ssl.h ]] || die "missing libssl development headers"
[[ -e /usr/include/libelf.h ]] || die "missing libelf development headers"
[[ -r "$CONFIG" ]] || die "no kernel config at $CONFIG; pass --config FILE"
note "config: $CONFIG"
note "versions: $VER_A and $VER_B, targets: $TARGETS, -j$JOBS"

# Options that need a tool this host may not have. Each is turned off with a
# note rather than failing the run: none of them changes what is under test.
DISABLE=()
HOSTFLAGS=()
if ! command -v pahole >/dev/null; then
  DISABLE+=(DEBUG_INFO_BTF DEBUG_INFO_BTF_MODULES)
  note "pahole not found: building without BTF"
fi
if [[ -e /usr/include/dwarf.h && -e /usr/include/elfutils/libdw.h ]]; then
  :
elif [[ -n "${KERNEL_TEST_SYSROOT:-}" ]]; then
  HOSTFLAGS=(HOSTCFLAGS="-I$KERNEL_TEST_SYSROOT/usr/include"
             HOSTLDFLAGS="-L$KERNEL_TEST_SYSROOT/usr/lib/$(uname -m)-linux-gnu")
  note "using KERNEL_TEST_SYSROOT for libdw"
else
  DISABLE+=(MODVERSIONS)
  note "libdw headers not found: building without MODVERSIONS"
  note "(install libdw-dev, or set KERNEL_TEST_SYSROOT, to cover gendwarfksyms)"
fi

mkdir -p "$WORK/tarballs" "$WORK/masq" || die "cannot create $WORK"
CACHE="$WORK/cache"
export VCACHE_CACHE_SIZE=100G

# kbuild calls the compiler as `gcc`; masquerading keeps the command kbuild
# records in its .cmd files identical to an uncached build.
for c in gcc cc; do ln -sf "$VCACHE" "$WORK/masq/$c"; done

# ---------------------------------------------------------------------------
section "1. fetch and configure $VER_A and $VER_B"

fetch() {
  local v=$1 series="v${1%%.*}.x" tb="$WORK/tarballs/linux-$1.tar.xz"
  [[ -s "$tb" ]] && return 0
  curl -sSfLo "$tb" "https://cdn.kernel.org/pub/linux/kernel/$series/linux-$1.tar.xz" ||
    { rm -f "$tb"; die "could not download linux-$1.tar.xz"; }
}
fetch "$VER_A"
fetch "$VER_B"
ok "tarballs present"

# a and b are the same version in two directories; c is the other version.
extract() {
  local slot=$1 v=$2
  [[ -d "$WORK/$slot/linux-$v" ]] && return 0
  rm -rf "$WORK/$slot"; mkdir -p "$WORK/$slot"
  tar -xf "$WORK/tarballs/linux-$v.tar.xz" -C "$WORK/$slot"
}
extract a "$VER_A" & extract b "$VER_A" & extract c "$VER_B" & wait
TREE_A="$WORK/a/linux-$VER_A"
TREE_B="$WORK/b/linux-$VER_A"
TREE_C="$WORK/c/linux-$VER_B"
ok "three trees extracted"

configure() {
  local tree=$1
  cp "$CONFIG" "$tree/.config"
  # Certificate paths that only exist in a distribution's own tree.
  "$tree/scripts/config" --file "$tree/.config" \
      --set-str SYSTEM_TRUSTED_KEYS "" --set-str SYSTEM_REVOCATION_KEYS ""
  # The archive kernel/kheaders.c embeds with .incbin is what section 4 checks,
  # so make sure it is built whatever the starting config said.
  "$tree/scripts/config" --file "$tree/.config" --module IKHEADERS
  local opt
  for opt in "${DISABLE[@]}"; do
    "$tree/scripts/config" --file "$tree/.config" --disable "$opt"
  done
  ( cd "$tree" && make -j"$JOBS" "${HOSTFLAGS[@]}" olddefconfig ) >/dev/null 2>&1
}
configure "$TREE_A"; configure "$TREE_B"; configure "$TREE_C"
check "IKHEADERS is enabled, so .incbin is exercised" \
  "$(grep -c '^CONFIG_IKHEADERS=' "$TREE_A/.config")" "1"

# One build, under the recipe from docs/linux-kernel.md.
#   build <tree> <logfile>
build() {
  local tree=$1 log=$2
  ( cd "$tree" &&
    VCACHE_DIR="$CACHE" \
    VCACHE_ROOTS="$tree=/usr/src/linux" \
    VCACHE_INCOMING_PREFIX_MAPS=strip \
    VCACHE_LOG="$log" \
    PATH="$WORK/masq:$PATH" \
    make -j"$JOBS" "${HOSTFLAGS[@]}" $TARGETS ) > "$log.build" 2>&1
}
stat_of() { VCACHE_DIR="$CACHE" "$VCACHE" --show-stats | grep -F "$1" | awk '{print $NF}'; }
zero()    { VCACHE_DIR="$CACHE" "$VCACHE" --zero-stats >/dev/null; }

# ---------------------------------------------------------------------------
section "2. a cold build, then the same tree from another directory"

rm -rf "$CACHE"; mkdir -p "$CACHE"
zero
if build "$TREE_A" "$WORK/a.log"; then ok "cold build succeeded"
else bad "cold build failed; see $WORK/a.log.build"; fi
COLD_MISS=$(stat_of "cache miss")
note "cold: $COLD_MISS compiled, $(stat_of "uncacheable") uncacheable"

zero
if build "$TREE_B" "$WORK/b.log"; then ok "warm build in a different directory succeeded"
else bad "warm build failed; see $WORK/b.log.build"; fi
WARM_HIT=$(stat_of "cache hit (disk)")
note "warm: $WARM_HIT hits, $(stat_of "cache miss") misses, $(stat_of "uncacheable") uncacheable"

# The claim the whole recipe rests on: the build directory does not matter.
check "every cacheable compilation hit" "$(stat_of "cache miss")" "0"
check "and nothing new was stored" "$(stat_of "entries stored")" "0"

# ---------------------------------------------------------------------------
section "3. cached objects are the objects the compiler would have produced"

# Compare exactly the objects the warm build served from the cache, which is
# the claim being made. Deriving the list from the log rather than walking the
# tree keeps the check honest in both directions: it does not quietly forgive an
# object that should have matched, and it does not fail on one the build
# legitimately compiled here -- kernel/kheaders.o, whose .incbin payload is this
# tree's own by design, and which section 4 checks properly.
#
# The sub-make that builds the host tools under tools/ names its output
# absolutely, so normalise to tree-relative or those objects drop out of the
# comparison unnoticed -- and they are exactly the ones the -Wp, handling
# exists for.
CACHED_OBJS="$WORK/cached-objects.txt"
awk '$3 == "key" { obj[$2] = $8 }
     $3 == "hit" && ($2 in obj) { print obj[$2] }' "$WORK/b.log" |
  grep -E '\.o$' | sed "s|^$TREE_B/||" | sort -u > "$CACHED_OBJS"

differing=0; compared=0; skipped_=0; first=""
while read -r rel; do
  if [[ ! -f "$TREE_A/$rel" || ! -f "$TREE_B/$rel" ]]; then
    skipped_=$((skipped_+1)); continue
  fi
  compared=$((compared+1))
  if ! cmp -s "$TREE_A/$rel" "$TREE_B/$rel"; then
    differing=$((differing+1)); [[ -z "$first" ]] && first="$rel"
  fi
done < "$CACHED_OBJS"
note "compared $compared objects served from the cache"
check "every cache-served object was found in both trees" "$skipped_" "0"
check "every one is byte-identical to the compiled original" "$differing" "0"
[[ -n "$first" ]] && note "first difference: $first"

# And the objects the warm build did *not* get from the cache are only the ones
# that cannot be shared: .incbin declines, whose payload is per-tree.
uncached=$(comm -23 \
  <(cd "$TREE_B" && find . -name '*.o' | sed 's|^\./||' | sort) "$CACHED_OBJS" | wc -l)
note "$uncached objects were not served from the cache"

# kbuild feeds the .d file to fixdep and fails outright if it is missing, so a
# build that got here already proves they were replayed -- but assert it
# directly, because a silently empty dependency list would not stop the build.
missing=0
while read -r obj; do
  cmd="$(dirname "$obj")/.$(basename "$obj").cmd"
  [[ -f "$TREE_B/$cmd" ]] && grep -q '^deps_' "$TREE_B/$cmd" || missing=$((missing+1))
done < <(cd "$TREE_B" && find kernel mm -maxdepth 1 -name '*.o' | sed 's|^\./||' | head -50)
check "dependency files were replayed on the hits" "$missing" "0"

# ---------------------------------------------------------------------------
section "4. .incbin is declined, and the payload follows the tree"

zero
if build "$TREE_C" "$WORK/c.log"; then ok "building $VER_B against the $VER_A cache succeeded"
else bad "build of $VER_B failed; see $WORK/c.log.build"; fi
note "$VER_B against $VER_A: $(stat_of "cache hit (disk)") hits, $(stat_of "cache miss") misses"

check "kernel/kheaders.c was declined for .incbin" \
  "$(grep -c 'uncacheable: \.incbin' "$WORK/c.log" | awk '{print ($1>0)?"yes":"no"}')" "yes"

# The regression this guards: kernel/kheaders.c is identical between the two
# releases and its command line is too, so a cache that ignores .incbin serves
# one tree's object to the other -- and the kernel then publishes the wrong
# tree's headers through /sys/kernel/kheaders.tar.xz.
KH_A="$TREE_A/kernel/kheaders_data.tar.xz"
KH_C="$TREE_C/kernel/kheaders_data.tar.xz"
if [[ -s "$KH_A" && -s "$KH_C" ]]; then
  if cmp -s "$KH_A" "$KH_C"; then
    bad "the two trees produced the same kheaders archive; this check proves nothing"
  else
    ok "the two releases embed different header archives"
    embeds() {  # embeds <object> <payload>
      objcopy -O binary --only-section=.rodata "$1" "$WORK/rodata.bin" 2>/dev/null || return 1
      cmp -s -n "$(stat -c%s "$2")" "$WORK/rodata.bin" "$2"
    }
    if embeds "$TREE_A/kernel/kheaders.o" "$KH_A"; then ok "$VER_A embeds its own archive"
    else bad "$VER_A's kheaders.o does not embed its own archive"; fi
    if embeds "$TREE_C/kernel/kheaders.o" "$KH_C"; then ok "$VER_B embeds its own archive"
    else bad "$VER_B's kheaders.o embeds another tree's archive"; fi
  fi
else
  skipped "kheaders archive not built; cannot check the .incbin payload"
fi

# ---------------------------------------------------------------------------
section "5. out-of-tree builds"

# A pristine tree is required for O=, so use the one version c came from only
# after cleaning it would be wrong; extract a fourth.
extract d "$VER_B"
TREE_D="$WORK/d/linux-$VER_B"
# Three object directories, not two: the refusal check below leaves whatever it
# managed to build behind, and reusing that directory for the cold measurement
# would hide those targets from it -- make would find them already up to date
# and never call the compiler, so nothing would be stored for the warm run to
# hit. That cost an afternoon; keep them separate.
OBJ0="$WORK/obj0"; OBJ1="$WORK/obj1"; OBJ2="$WORK/obj2"
rm -rf "$OBJ0" "$OBJ1" "$OBJ2"; mkdir -p "$OBJ0" "$OBJ1" "$OBJ2"

prep_obj() {
  local obj=$1
  cp "$CONFIG" "$obj/.config"
  "$TREE_D/scripts/config" --file "$obj/.config" \
      --set-str SYSTEM_TRUSTED_KEYS "" --set-str SYSTEM_REVOCATION_KEYS "" --module IKHEADERS
  local opt
  for opt in "${DISABLE[@]}"; do
    "$TREE_D/scripts/config" --file "$obj/.config" --disable "$opt"
  done
  ( cd "$TREE_D" && make O="$obj" -j"$JOBS" "${HOSTFLAGS[@]}" olddefconfig ) >/dev/null 2>&1
}
prep_obj "$OBJ0"; prep_obj "$OBJ1"; prep_obj "$OBJ2"

# kbuild adds -fmacro-prefix-map of its own here. vcache owns path rewriting, so
# it must stop rather than silently let one mapping win.
refusal=$( ( cd "$TREE_D" &&
  VCACHE_DIR="$CACHE" VCACHE_ROOTS="$TREE_D=/usr/src/linux" \
  VCACHE_INCOMING_PREFIX_MAPS=error PATH="$WORK/masq:$PATH" \
  make O="$OBJ0" -j"$JOBS" "${HOSTFLAGS[@]}" $TARGETS ) 2>&1 |
  grep -c 'refusing to run: the command line contains -fmacro-prefix-map' )
check "an O= build without incoming_prefix_maps=strip is refused, loudly" \
  "$([[ "$refusal" -gt 0 ]] && echo yes || echo no)" "yes"

# With strip, and with the object tree named as a root of its own: the host
# tools under tools/ are built by a sub-make whose working directory is a
# subdirectory of the object tree, and they name $(objtree)/include absolutely.
build_oot() {
  local obj=$1 log=$2
  ( cd "$TREE_D" &&
    VCACHE_DIR="$CACHE" \
    VCACHE_ROOTS="$TREE_D=/usr/src/linux:$obj=/usr/obj/linux" \
    VCACHE_INCOMING_PREFIX_MAPS=strip \
    VCACHE_LOG="$log" \
    PATH="$WORK/masq:$PATH" \
    make O="$obj" -j"$JOBS" "${HOSTFLAGS[@]}" $TARGETS ) > "$log.build" 2>&1
}
zero
if build_oot "$OBJ1" "$WORK/o1.log"; then ok "cold O= build succeeded"
else bad "cold O= build failed; see $WORK/o1.log.build"; fi
zero
if build_oot "$OBJ2" "$WORK/o2.log"; then ok "warm O= build in another object directory succeeded"
else bad "warm O= build failed; see $WORK/o2.log.build"; fi
note "warm O=: $(stat_of "cache hit (disk)") hits, $(stat_of "cache miss") misses"
check "the object directory does not affect the key" "$(stat_of "cache miss")" "0"

# ---------------------------------------------------------------------------
printf '\n\033[1mlinux kernel: %d passed, %d failed, %d skipped\033[0m\n' "$PASS" "$FAIL" "$SKIP"
if [[ "$FAIL" -eq 0 && "$KEEP" -eq 0 ]]; then
  rm -rf "$WORK/a" "$WORK/b" "$WORK/c" "$WORK/d" "$OBJ0" "$OBJ1" "$OBJ2" "$CACHE"
  printf 'removed the trees and the cache; tarballs kept in %s\n' "$WORK/tarballs"
else
  printf 'work directory kept at %s\n' "$WORK"
fi
[[ "$FAIL" -eq 0 ]]
