# BLAKE3 1.5.4 (C implementation)

Upstream: https://github.com/BLAKE3-team/BLAKE3, tag `1.5.4`, from the `c/`
subdirectory. Used for cache keys (`src/hash/hasher.cc`).

## What is vendored

| File | Purpose |
| --- | --- |
| `blake3.h`, `blake3_impl.h` | public and internal headers |
| `blake3.c` | the hasher |
| `blake3_dispatch.c` | runtime CPU feature detection |
| `blake3_portable.c` | plain C compression function |
| `blake3_{sse2,sse41,avx2,avx512}_x86-64_unix.S` | SIMD kernels, x86-64 ELF |
| `LICENSE_A2` | Apache 2.0 with LLVM exception |

## What upstream ships that is not here

- **`*_x86-64_windows_gnu.S`** — MinGW assembly. vcache builds ELF only.
- **`blake3_{sse2,sse41,avx2,avx512}.c`** — C intrinsics implementations of the
  same kernels. Upstream builds *either* these or the assembly, not both; vcache
  builds the assembly, so these were never compiled.
- **`blake3_neon.c`** — ARM NEON kernel. On a non-x86-64 host the Makefile builds
  the portable C path instead (see `BLAKE3_ARCH` in the top-level Makefile),
  which is correct but slower. Restore this file and wire up `BLAKE3_USE_NEON`
  if ARM throughput ever matters.

  Because this file is absent, the Makefile **must** pass `-DBLAKE3_USE_NEON=0`
  on aarch64. `blake3_impl.h` autodetects that macro to `1` there, and
  `blake3_dispatch.c` then calls `blake3_hash_many_neon` unconditionally, so
  omitting the define does not select the portable path — it fails the link on
  undefined references.

Everything kept here is compiled on at least one supported target. Nothing here
is dead.

## Updating

Fetch the upstream tarball, copy `c/blake3.h`, `c/blake3_impl.h`, `c/blake3.c`,
`c/blake3_dispatch.c`, `c/blake3_portable.c`, the four `*_x86-64_unix.S` files
and the licence, then run `make test` — the hashing tests in `tests/unit_test.cc`
cover both the SIMD and portable paths.
