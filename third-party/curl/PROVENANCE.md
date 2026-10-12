# libcurl declarations (subset)

Upstream: https://github.com/curl/curl, `include/curl/curl.h`. Checked against
curl 8.14.1. Used only when the host has no curl headers of its own; the
Makefile probes for `<curl/curl.h>` and puts `include/` on the path otherwise.

## What is vendored

| File | Purpose |
| --- | --- |
| `include/curl/curl.h` | the types and constants `src/storage/curl_api.h` and `src/storage/s3_storage.cc` use |
| `COPYING` | the curl licence |

libcurl itself is not here and is never linked: it is opened with `dlopen` when
an S3 layer is constructed (see `src/storage/curl_api.h`). Its ten entry points
are resolved by name, so no function is declared. What remains is six types
(`CURL`, `curl_slist`, `curl_off_t`, `CURLcode`, `CURLoption`, `CURLINFO`) and
eighteen constants: `CURLE_OK`, the fifteen `CURLOPT_` options vcache sets,
`CURLINFO_RESPONSE_CODE` and `CURL_GLOBAL_DEFAULT`.

These values are libcurl's ABI. curl does not renumber options within soname 4,
so the subset does not go stale as curl releases; it only grows when vcache
starts using something new.

## What upstream ships that is not here

Everything else: the rest of `curl.h`, and `curlver.h`, `easy.h`, `multi.h`,
`system.h`, `typecheck-gcc.h` and the other headers it includes. Function
prototypes would only be noise next to a table of function pointers, and the
type-checking macros are exactly what `curl_api.h` turns off.

## Updating

When vcache starts using a new option or info code, add it here with the value
curl.h gives it, written the same way (`CURLOPTTYPE_* + n`). Then run
`make test` both ways: without curl's headers, and with them (installed, or
`make CURL_CFLAGS=-I<dir>` naming a curl source tree's `include/`). Its S3
section drives every option here against a mock object store, and with real
headers present `tests/curl_abi_check.sh` compares every constant, type size
and `curl_slist` offset declared here against them.
