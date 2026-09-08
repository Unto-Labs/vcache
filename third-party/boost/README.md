# Boost 1.86.0, minimal subset

932 of 15828 headers, 1.1M instead of
104M. Only Boost.Spirit X3 is used directly; the rest is what
X3 pulls in through Fusion, MPL and Preprocessor.

Regenerate with `third-party/regen-boost-subset.sh` (or `make boost-subset`)
after adding an include that reaches a new part of Boost. `SUBSET-FILES.txt`
lists the exact contents.

Boost is distributed under the Boost Software License 1.0; see LICENSE.

## Compiler and platform coverage

The subset is the union of two traces: gcc on Linux, and clang on macOS. Boost
picks its config headers from predefined macros, so each trace selects a
different set and both are needed:

| | gcc on Linux | clang on macOS |
| --- | --- | --- |
| `config/compiler/` | `gcc.hpp` | `clang.hpp`, `clang_version.hpp` |
| `config/platform/` | `linux.hpp` | `macos.hpp` |
| `config/stdlib/` | `libstdcpp3.hpp` | `libcpp.hpp` |

Four headers out of 936 differ between them; everything else is shared.

A compiler can be added from anywhere -- pass them all to the script and it
writes the union. A *platform* cannot: no run on Linux will ever open
`platform/macos.hpp`. Trace that one where it applies and merge the result:

```console
$ gh workflow run boost-subset.yml            # runs on macos-14, uploads an artifact
$ gh run download <run-id> -n boost-subset-macos -D /tmp/macos-subset
$ ./third-party/regen-boost-subset.sh --merge-from /tmp/macos-subset
```

The merge refuses if a header present in both differs, which is what catches two
traces taken from different Boost versions.
