#!/usr/bin/env bash
# Fetches and builds GCC's libcpp, which vcpp is a front end for.
#
# libcpp is *not* vendored into this repository, deliberately. It is 58,000
# lines under GPLv3, and keeping it out of the tree keeps the licence boundary
# obvious: everything committed under vcpp/ is our own GPLv3 code, everything
# above vcpp/ is Apache-2.0, and GCC's sources are downloaded at build time the
# way third-party/fetch.sh downloads gperftools.
#
# Only three directories of the tarball are extracted -- libcpp, the shared
# include/ headers it needs, and libiberty -- plus two files: the top-level
# install-sh that libcpp's configure insists on, and one x86 header lex.cc
# includes for its SSE4 path.
#
#   ./fetch-libcpp.sh            fetch and build if missing
#   ./fetch-libcpp.sh --force    start over
#
# The version is pinned and verified by SHA-256, and it is NOT interchangeable:
# it has to match the compiler vcache is caching for. libcpp 16.2.0 truncates a
# system-header level of 2 to 1 (files.cc passes an int sysp through a bool
# parameter of _cpp_post_stack_file), so every linemarker naming a system header
# comes out as "# 1 \"...\" 1 3" where gcc 13 writes "1 3 4". That is a
# one-flag difference in the preprocessed text, which is exactly the kind of
# difference vcache must not paper over. See README.md.
#
#   VCPP_GCC_VERSION=14.2.0 ./fetch-libcpp.sh    build against another release

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DL="$HERE/.dl"

# Default to the release matching the gcc on this machine, falling back to the
# one this was developed against.
default_version() {
  local v
  v="$(${CC:-gcc} -dumpfullversion 2>/dev/null || true)"
  case "$v" in
    13.3.0|16.2.0) echo "$v" ;;
    *) echo "13.3.0" ;;
  esac
}

GCC_VERSION="${VCPP_GCC_VERSION:-$(default_version)}"

case "$GCC_VERSION" in
  13.3.0) GCC_SHA256="0845e9621c9543a13f484e94584a49ffc0129970e9914624235fc1d061a0c083" ;;
  16.2.0) GCC_SHA256="e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e" ;;
  *) echo "no recorded SHA-256 for gcc ${GCC_VERSION}; add one to fetch-libcpp.sh" >&2
     exit 1 ;;
esac

GCC_URL="https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz"

SRC="$HERE/gcc-${GCC_VERSION}"
BUILD="$HERE/build"

FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

log() { printf '>> %s\n' "$*"; }

verify() {
  local file="$1" expected="$2" actual
  # sha256sum is coreutils; macOS ships shasum instead.
  if command -v sha256sum >/dev/null 2>&1; then
    actual="$(sha256sum "$file" | cut -d' ' -f1)"
  elif command -v shasum >/dev/null 2>&1; then
    actual="$(shasum -a 256 "$file" | cut -d' ' -f1)"
  else
    echo "need sha256sum or shasum to verify the download" >&2
    return 1
  fi
  if [[ "$actual" != "$expected" ]]; then
    echo "checksum mismatch for $file" >&2
    echo "  expected $expected" >&2
    echo "  actual   $actual" >&2
    return 1
  fi
}

if (( FORCE )); then
  rm -rf "$SRC" "$BUILD"
fi

# ---- download ---------------------------------------------------------------

tarball="$DL/gcc-${GCC_VERSION}.tar.xz"
if [[ ! -f "$tarball" ]]; then
  mkdir -p "$DL"
  log "downloading GCC ${GCC_VERSION} (about 107 MB, one time)"
  curl -sSL --fail -o "$tarball.part" "$GCC_URL"
  mv "$tarball.part" "$tarball"
fi
verify "$tarball" "$GCC_SHA256"

# ---- extract just what libcpp needs -----------------------------------------

if [[ ! -d "$SRC/libcpp" ]]; then
  log "extracting libcpp, include and libiberty"
  mkdir -p "$SRC"
  tar -xf "$tarball" -C "$HERE" \
      "gcc-${GCC_VERSION}/libcpp" \
      "gcc-${GCC_VERSION}/include" \
      "gcc-${GCC_VERSION}/libiberty"
  # libcpp's configure looks for install-sh at the top of the tree, and lex.cc
  # includes an x86 header from gcc/config for its vectorised scanner.
  # depcomp and friends are what libcpp's configure probes for; without them
  # it stops at "no usable dependency style found".
  for extra in install-sh config.guess config.sub move-if-change \
               depcomp compile missing mkinstalldirs ltmain.sh \
               gcc/config/i386/cpuid.h; do
    tar -xf "$tarball" -C "$HERE" "gcc-${GCC_VERSION}/$extra" 2>/dev/null || true
  done
  # The licence GCC ships, kept beside the sources it covers.
  tar -xOf "$tarball" "gcc-${GCC_VERSION}/COPYING3" > "$SRC/COPYING3" 2>/dev/null || true
fi

# ---- build ------------------------------------------------------------------

if [[ ! -f "$BUILD/libcpp/libcpp.a" ]]; then
  log "building libiberty"
  mkdir -p "$BUILD/libiberty"
  ( cd "$BUILD/libiberty" && "$SRC/libiberty/configure" >/dev/null \
      && make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >/dev/null )

  log "building libcpp"
  mkdir -p "$BUILD/libcpp"
  ( cd "$BUILD/libcpp" && "$SRC/libcpp/configure" >/dev/null \
      && make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >/dev/null )
fi

[[ -f "$BUILD/libcpp/libcpp.a" ]] || { echo "libcpp.a was not produced" >&2; exit 1; }

# One source of truth for which libcpp got built: the Makefile reads this, and
# vcpp.cc compiles against the API of whichever release it names.
cat > "$BUILD/libcpp-version.mk" <<EOF
LIBCPP_VERSION := $GCC_VERSION
LIBCPP_MAJOR   := ${GCC_VERSION%%.*}
EOF

log "libcpp ${GCC_VERSION} ready: $BUILD/libcpp/libcpp.a"
