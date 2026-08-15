# vcache quick start

Getting from a fresh checkout to cross-directory cache hits. Five minutes.

For the full set of knobs, see [configuration.md](configuration.md).

## 1. Build

```console
$ ./third-party/fetch.sh     # builds tcmalloc; one time, needs network
$ make -j
$ make test                  # optional, ~30 s
```

The binary lands in `bin/vcache`. Put it somewhere on your `$PATH`.

## 2. Hook it up to your compiler

Either call it explicitly:

```console
$ vcache g++ -c foo.cc -o foo.o
```

or — usually easier — drop symlinks named after your compilers into a directory
early on `$PATH`, so your build system picks vcache up without any changes:

```console
$ mkdir -p ~/.local/libexec/vcache
$ for c in gcc g++ cc c++ rustc; do ln -sf "$(command -v vcache)" ~/.local/libexec/vcache/$c; done
$ export PATH=~/.local/libexec/vcache:$PATH
```

vcache recognises how it was invoked from `argv[0]` and finds the real compiler
further along `$PATH`.

## 3. Declare your source root — this is the important step

vcache works with no configuration at all, but it will only ever hit within a
single directory. To share entries across checkouts, machines or CI runners, you
must tell it which directory prefix is not meaningful:

```console
$ export VCACHE_ROOTS=/home/you/myproject
```

That rewrites `/home/you/myproject` to `/vcache/myproject` in linemarkers,
`__FILE__` and debug info. A colleague with the tree at `/build/myproject` sets
`VCACHE_ROOTS=/build/myproject`, gets the same canonical prefix, and the two
share cache entries.

Check what it resolved to:

```console
$ vcache --show-roots
/home/you/myproject -> /vcache/myproject
/home/you/myproject/build -> /vcache/cwd
```

The second line is the build directory, added automatically because
`DW_AT_comp_dir` records the working directory rather than the source directory.

## 4. Prove it works

Two checkouts of the same tree should produce one cache entry and identical
objects:

```console
$ git clone myproject /tmp/one && git clone myproject /tmp/two

$ cd /tmp/one && VCACHE_ROOTS=/tmp/one=proj vcache g++ -g -O2 -c -I include src/lib.cc -o /tmp/a.o
$ cd /tmp/two && VCACHE_ROOTS=/tmp/two=proj vcache g++ -g -O2 -c -I include src/lib.cc -o /tmp/b.o

$ cmp /tmp/a.o /tmp/b.o && echo identical
identical
$ vcache --show-stats | head -4
cache hit (disk)    1
cache hit (s3)      0
cache miss          1
```

`=proj` names the canonical prefix explicitly (`/vcache/proj`). Without it the
name is derived from the directory's basename, which here would be `one` and
`two` — two different prefixes, and therefore no shared entry.

## 5. Day-to-day commands

```console
$ vcache --show-stats     # hit rate and cache size
$ vcache --zero-stats     # reset counters
$ vcache --show-config    # everything that is in effect
$ vcache --show-roots     # the mapping for this directory
$ vcache --clear          # empty the cache
$ vcache --trim           # evict down to the size limit
```

## 6. If something looks wrong

**Everything misses.** Almost always the roots. Run `vcache --show-roots` in
both trees and confirm the right-hand sides match exactly. Different canonical
prefixes never share.

**vcache refuses to run**, complaining about `-ffile-prefix-map`. Your build
system already remaps paths. vcache owns that job, so it stops rather than
silently overriding you. Pass `--vcache-allow-prefix-maps` (or set
`VCACHE_INCOMING_PREFIX_MAPS=strip`) to let vcache's mapping win.

**Nothing is cached at all.** Check `vcache --show-stats` for a high
*uncacheable* count — linking, `-E`, `-M`-only dependency passes and multiple
inputs per invocation all fall through to the compiler by design.

**You need to know why.** `VCACHE_LOG=stderr` explains every decision for one
compilation, including the compiler's own error if preprocessing failed:

```console
$ VCACHE_LOG=stderr vcache g++ -c foo.cc -o foo.o
```

**Debugging a cached binary.** Paths in debug info are canonical, so point the
debugger at the real sources:

```
(gdb) set substitute-path /vcache/myproject /home/you/myproject
```

## Next steps

- [configuration.md](configuration.md) — every option, precedence, worked examples,
  and how to set up a shared S3 cache
- [preprocessor-problem.md](preprocessor-problem.md) — why this is needed, with
  measurements
- [design.md](design.md) — how it is put together
