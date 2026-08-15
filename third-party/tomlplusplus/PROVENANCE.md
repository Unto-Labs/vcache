# toml++ 3.4.0

Upstream: https://github.com/marzer/tomlplusplus, tag `v3.4.0`, the single-header
release (`toml.hpp` from the repository root). Used to parse the configuration
file (`src/core/config.cc`).

## What is vendored

| File | Purpose |
| --- | --- |
| `toml.hpp` | the entire library, header-only |
| `LICENSE` | MIT |

Nothing else from upstream is needed: the single-header release is
self-contained, and vcache compiles it with `TOML_EXCEPTIONS=0` so parse errors
arrive as a result object rather than a throw. Breaking a build over a malformed
config file would be worse than ignoring the file, so the no-exceptions mode is
deliberate.

## Updating

Replace `toml.hpp` with the new release's single header and run `make test`; the
configuration tests in `tests/integration_test.sh` exercise the parser through
`--show-config` and the config-file override cases.
