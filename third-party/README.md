# Third-party dependencies

Four libraries, three of them committed here in full or in part. Nothing in this
directory is unused: every file is compiled on at least one supported target.

| Directory | Version | Licence | Committed? | Size | Used for |
| --- | --- | --- | --- | --- | --- |
| `blake3/` | 1.5.4 | Apache 2.0 w/ LLVM exception | yes | 418 KB | cache-key hashing |
| `tomlplusplus/` | 3.4.0 | MIT | yes | 477 KB | config-file parsing |
| `boost/` | 1.86.0 subset | BSL-1.0 | yes | 4.2 MB | Spirit X3, for dependency-file parsing |
| `gperftools/` | 2.16 | BSD 3-clause | no — fetched | 13 MB | tcmalloc |

**None of this is under vcache's GPL.** Each component stays under the licence
above, and the licence text in each directory travels with it. vcache itself is
GPL-3.0-or-later; these terms permit inclusion in such a work, which is what
makes the combination distributable, but that is a one-way relationship and
nothing here becomes GPL by sitting in this tree.

Each directory carries its upstream licence and a `PROVENANCE.md` (or `README.md`
for the Boost subset) recording the version, where it came from, and what was
deliberately left out.

## Why some are committed and one is not

`blake3` and `tomlplusplus` are small enough that vendoring them outright is
simpler than any fetch mechanism.

`boost` is committed as a **932-header subset** of Boost 1.86.0 — 4.2 MB instead
of 104 MB. Only Boost.Spirit X3 is used directly; the rest is what X3 reaches
through Fusion, MPL and Preprocessor. `make boost-subset` regenerates it from a
full Boost tree and verifies the result by recompiling every translation unit
against the subset alone.

`gperftools` is not committed because it is a build, not a header set: it needs
CMake and produces static archives. `./fetch.sh` (or `make deps`) downloads it,
verified against a pinned SHA-256, and builds `tcmalloc_minimal`.

## libcurl is not here

libcurl is used by the S3 cache layer but is neither vendored nor linked. It is
opened with `dlopen` only when an S3 layer is constructed, so a disk-only build
never maps it — see `src/storage/curl_api.h`. Only its headers are needed at
compile time. SHA-256 and HMAC for request signing are implemented in
`src/hash/sha256.cc` rather than taken from OpenSSL.

## Regenerable scratch

`.dl/` holds downloaded archives, and `boost-full-*/` a full Boost tree used only
by the subset generator. Both are gitignored and recreated on demand;
`make distclean` removes them along with the built gperftools tree.
