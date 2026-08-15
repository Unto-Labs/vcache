# Boost 1.86.0, minimal subset

932 of 15828 headers, 1.1M instead of
104M. Only Boost.Spirit X3 is used directly; the rest is what
X3 pulls in through Fusion, MPL and Preprocessor.

Regenerate with `third-party/regen-boost-subset.sh` (or `make boost-subset`)
after adding an include that reaches a new part of Boost. `SUBSET-FILES.txt`
lists the exact contents.

Boost is distributed under the Boost Software License 1.0; see LICENSE.
