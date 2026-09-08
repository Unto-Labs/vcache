# Recipe: caching Linux kernel builds

Copy, paste, build. The kernel needs no patching; all of this is on the vcache
side.

```console
$ mkdir -p ~/.local/libexec/vcache
$ for c in gcc g++ cc; do ln -sf "$(command -v vcache)" ~/.local/libexec/vcache/$c; done
$ export PATH=~/.local/libexec/vcache:$PATH

$ export VCACHE_ROOTS=/usr/src/linux-7.0.12=/usr/src/linux   # <your tree>=<a shared prefix>
$ export VCACHE_INCOMING_PREFIX_MAPS=strip                  # needed once you use O=

$ cd /usr/src/linux-7.0.12 && make -j"$(nproc)"
```

Building out of tree needs one more root —
`VCACHE_ROOTS=<src>=/usr/src/linux:<objdir>=/usr/obj/linux` — and nothing else
changes. That is the whole recipe; the rest of this page is why each line is
there and what it measures.

On a 64-core machine, Ubuntu's own kernel config, `make all modules`: **393 s
uncached, 195 s from a cache warmed in a different directory**, 99.91% hit rate,
with `System.map` and `Module.symvers` byte-identical to the uncached build.

`make kernel-test` reproduces the hit rates and the byte-identity on your own
machine, starting from your own `/boot/config-$(uname -r)`. It does not time an
uncached baseline, and the `System.map` comparison needs a full link — see
[Verifying it yourself](#verifying-it-yourself) for both by hand.

## Why each line is there

### The masquerade, rather than `make CC="vcache gcc"`

Both work. The symlinks are better here because kbuild records the expanded
command line for every target in a `.<target>.cmd` file and rebuilds anything
whose recorded command no longer matches. `CC="vcache gcc"` changes every one of
them, so turning the cache on or off forces a full rebuild of a tree that was
otherwise up to date. Under a masquerade the recorded command is still `gcc`,
and a tree built without vcache is picked up unchanged by a build with it.

`CONFIG_CC_VERSION_TEXT` is safe either way: vcache passes `--version` straight
through, so kbuild reads gcc's own banner and the `.config` does not move.

### The root

Give every checkout the same name after the `=` and they share entries:

```console
$ VCACHE_ROOTS=/usr/src/linux-7.0.12=/usr/src/linux vcache --show-roots
/usr/src/linux-7.0.12 -> /usr/src/linux
```

An in-tree kernel build spells almost every path relative to the top of the
tree, so it looks like it should be directory-independent already. Two things
spoil that, and the root fixes both:

- Debug info records the working directory in `DW_AT_comp_dir`, which is
  absolute.
- The host tools under `tools/` (objtool, fixdep, libsubcmd) are built by a
  different Makefile that uses **absolute** paths throughout — include
  directories, dependency files, and dependency targets.

Naming the prefix is the part that is easy to get wrong. Written without an `=`,
the prefix is derived from the directory's basename — so `linux-7.0.12` and
`linux-7.0.13` would get *different* prefixes and never share an entry, which is
exactly backwards, since sharing between neighbouring releases is most of the
value. Every tree has to resolve to the same right-hand side, whatever it is.

`/usr/src/linux` is a good choice because it is where a kernel source tree
conventionally lives, so `DW_AT_comp_dir` names something a person and a
debugger both recognise. The one thing to know about it is that the prefix now
names a path that can really exist: if some machine has a *different* tree at
`/usr/src/linux`, a debugger will find those sources and show them without
saying anything. If that worries you, `<tree>=linux` maps to `/vcache/linux`
instead, which cannot be mistaken for a real directory. Both forms are ordinary
root specs — see [configuration.md](configuration.md#spec-syntax).

### `strip`

kbuild adds `-fmacro-prefix-map=$(srcroot)/=` of its own, but only when building
out of the source tree — `make O=...`, and external module builds. vcache
refuses to run when it finds a prefix map it did not put there, because the two
would interact silently under gcc's last-match-wins rule. `strip` says vcache's
mapping wins. An in-tree build never passes one, so the variable is harmless
there and required as soon as you use `O=`.

## What it buys

Ubuntu's `/boot/config-7.0.0-30-generic` on a vanilla 7.0.12 tree, gcc 13.3,
`make -j64 all modules` on a 64-core machine — 26,528 compilations producing
28,552 objects, 879 archives and 6,891 modules:

| | wall | hit rate |
| --- | --- | --- |
| no vcache | 393 s | — |
| vcache, cold cache | 499 s | 0.06% |
| vcache, warm cache, **a different directory** | **195 s** | **99.91%** |

Two things to read off that. A cold cache costs 27% — the price of preprocessing
every translation unit twice and storing 10 GiB of entries. And the warm build
is 2.0× rather than 20×, because at 64-way parallelism the compiles were never
the whole of the wall time: modpost, `gendwarfksyms`, linking 6,891 modules and
compressing them are not cached and do not get faster.

The 24 misses in the warm build are the compilations that *should* miss: 20
`lib/test_fortify` targets, which are meant not to compile, and four objects
that embed the build timestamp through `include/generated/compile.h`
(`init/version-timestamp.c`, `arch/x86/boot/version.c`,
`arch/x86/boot/header.S`, `arch/x86/boot/compressed/kaslr.c`). Pin
`KBUILD_BUILD_TIMESTAMP` if you want those four as well.

### Across kernel versions

On `kernel/ mm/` (529 compilations), against a cache warmed by earlier releases:

| cache holds | building | hit rate | shareable ceiling |
| --- | --- | --- | --- |
| 7.0.12 | 7.0.12, elsewhere | 100% | 100% |
| 7.0.12 | 7.0.13 | 25.7% | 116 of 508 objects |
| 7.0.12, 7.0.13 | 7.0.14 | 98.7% | 500 of 508 objects |

The 7.0.13 number is not a cache failure: that release changed
`include/linux/mm.h` and `include/linux/tracepoint.h`, which almost everything
includes, so only 116 of the 508 objects are byte-identical between the two
trees. vcache hit all 116. Read a kernel hit rate against that ceiling, which
you measure by building both trees against separate empty caches and comparing
the objects — never against 100%.

### Across configurations

Same trees, `kernel/ mm/ fs/` (1,902 compilations), starting from the local
kernel's config and changing one option:

| change | `.config` lines | hit rate | ceiling |
| --- | --- | --- | --- |
| `CONFIG_EXT2_FS=m` | 4 | 97.4% | 1,875 of 1,877 |
| `CONFIG_DEBUG_LIST=y` | 2 | 5.7% | 1,325 of 1,877 |

Both are honest results, and the gap between them is the whole point.

Adding a filesystem changes `include/generated/autoconf.h`, which kbuild
force-includes into *every* translation unit. A cache keyed on the files a
compilation reads would miss on all 1,877. vcache keys on the preprocessed text,
where the new `CONFIG_EXT2_FS` macro is simply never expanded, so it misses 49:
the ten new `fs/ext2` objects, one file in `fs/ext4` that really does test the
symbol, and 38 from kbuild re-running Kconfig — its host tools and its
`cc-option` probes, a one-off that does not repeat on the next build.

`CONFIG_DEBUG_LIST` is the other kind of change, and it marks the limit of the
approach. It rewrites the inline bodies in `include/linux/list.h` and
`list_bl.h`, so nearly every translation unit's preprocessed text really does
differ — while only the ones that instantiate those inlines emit a different
object. 1,325 objects were byte-identical and only 109 were hit. No cache that
hashes preprocessed text can close that gap; knowing the object is unchanged
requires compiling it.

## What is not cached, and why

From the full build above, by count:

| reason | what it is |
| --- | --- |
| `no input file` | source on stdin — `cc-option` probes, and the `*.gendwarfksyms.o` scratch objects kbuild pipes into the compiler — plus `--version` queries and host-tool link commands |
| `not a compile-only invocation (no -c)` | host programs compiled and linked in one step |
| `preprocess-only invocation (-E)` | linker scripts (`.lds.S`), the gdb-scripts template, and the `-E` form of `cc-option` |
| `multiple input files` | host programs linked from several objects at once |
| `.incbin ...` | see below |

83 uncacheable invocations against 26,528 cached ones. All of them fall through
to the compiler unchanged, so the build always makes progress.

One consequence is worth knowing: because the `*.gendwarfksyms.o` objects are
compiled from stdin, vcache passes them through *without* its prefix map, so
those particular files keep the local path in their debug info. They are scratch
input to `gendwarfksyms` and are never linked into anything.

### `.incbin`

`.incbin` tells the *assembler* to splice a file in verbatim. Neither the file's
name nor its contents reach the preprocessed text, so two compilations that
preprocess identically can legitimately owe different objects. vcache declines
them.

This is not a corner case in the kernel — it is on the boot path. Every file the
build above declined, and what each one embeds:

| file | embeds |
| --- | --- |
| `arch/x86/boot/compressed/piggy.S` | the compressed kernel image |
| `arch/x86/realmode/rmpiggy.S` | the real-mode trampoline |
| `arch/x86/purgatory/kexec-purgatory.S` | the kexec purgatory blob |
| `usr/initramfs_data.S` | the built-in initramfs |
| `certs/system_certificates.S` | the built-in certificate list |
| `certs/revocation_certificates.S` | the revocation list |
| `kernel/kheaders.c` | a tar of the tree's own headers |

Other configurations add more — `kernel/configs.c` embeds the build's own
`.config` under `CONFIG_IKCONFIG`, and `lib/bootconfig-data.S` the default boot
config.

Before vcache declined them, building 7.0.13 against a cache warmed by 7.0.12
served `kernel/kheaders.o` from the 7.0.12 entry: the objects were byte-identical
and the payloads were not, so the 7.0.13 kernel would have published 7.0.12's
headers through `/sys/kernel/kheaders.tar.xz`. Nothing in the build said so.

## Debug info

Objects carry `/usr/src/linux` where the tree used to be — that is the mapping
doing its job, and it is why they are shareable at all. If the sources really do
live at `/usr/src/linux`, a debugger needs nothing further. Otherwise point it
back:

```
(gdb) set substitute-path /usr/src/linux /usr/src/linux-7.0.12
(lldb) settings set target.source-map /usr/src/linux /usr/src/linux-7.0.12
```

`System.map`, symbol addresses and code are unaffected: only the paths recorded
in DWARF and in `__FILE__` change.

## Out-of-tree and external modules

Declare the object tree as a root too, with the same canonical name in every
build directory:

```console
$ export VCACHE_ROOTS=/usr/src/linux-7.0.14=/usr/src/linux:/tmp/build-a=/usr/obj/linux
$ export VCACHE_INCOMING_PREFIX_MAPS=strip
$ make -C /usr/src/linux-7.0.14 O=/tmp/build-a -j"$(nproc)"
```

Without `strip` an `O=` build stops on the first compile, which is at least
unambiguous:

```
vcache: refusing to run: the command line contains
        -fmacro-prefix-map=/usr/src/linux-7.0.14/=
```

The second root matters more than it looks. vcache maps the working directory
by default, which covers the top-level `O=` build, but the host tools under
`tools/` are built by a sub-make whose working directory is a *subdirectory* of
the object tree, and they name `$(objtree)/include` absolutely. Naming the
object tree explicitly closes that gap: measured on `kernel/ mm/`, two object
directories over one source tree hit **92.5%** with the source root alone and
**100%, zero misses**, with both.

In-tree and out-of-tree builds do **not** share entries with each other — an
in-tree build spells its include paths relative to the tree and an `O=` build
spells them absolutely, so the preprocessed text differs. An `O=` build meeting
a cache warmed entirely in-tree hit 1.2%. Pick one shape and keep to it.

## What kbuild does after the compiler, and why it is safe

`objtool`, `gendwarfksyms` (module symbol versions) and `recordmcount` all run
as separate commands *after* `$(CC)` returns, on the object file. vcache never
sees them; they rewrite whatever object is in place, cached or freshly compiled.
Because a cached object is byte-identical to the compiled one, so is everything
they derive from it.

`fixdep` is the one that would notice a mistake: it reads the `.d` file kbuild
asks for with `-Wp,-MMD,<file>` and fails outright if it is missing. vcache
stores that file alongside the object with its paths canonicalised, and rewrites
them back to the local tree on a hit. It writes one after a *failed* compile
too, as gcc does — `lib/test_fortify` compiles code that is meant not to build,
and kbuild still hands the `.d` to fixdep afterwards.

## Verifying it yourself

There is a test for all of this, kept out of `make test` because it downloads
two kernel tarballs, wants ~10 GB of disk and takes a few minutes:

```console
$ make kernel-test
$ make kernel-test KERNEL_TEST_ARGS="--versions 6.19.13,6.19.14 --keep"
$ tests/linux_kernel_test.sh --help
```

It starts from this machine's own `/boot/config-$(uname -r)`, builds `kernel/`
and `mm/` from two directories and two releases, and checks the five things a
kernel build is uniquely good at breaking: that the second directory hits on
everything, that its objects are byte-identical to the compiled ones, that the
dependency files kbuild hands to `fixdep` came back, that `kernel/kheaders.o`
embeds *its own* tree's header archive rather than the other release's, and
that an `O=` build is refused without `incoming_prefix_maps=strip` and shares
completely with it. `--targets "all modules"` runs the same checks over the
whole tree.

By hand, the claim worth checking is that a cached object is the object the
compiler would have produced. Build a tree cold, build a second copy of the same
tree warm, and compare every object:

```console
$ for t in tree-a tree-b; do
    ( cd $t && find . -name '*.o' -print0 | sort -z | xargs -0 sha256sum ) > $t.sums
  done
$ diff tree-a.sums tree-b.sums && echo "every object identical"
```

That is the check this recipe was developed against. Of 36,322 objects, archives
and modules, 36,294 came back byte-identical between the tree that compiled them
and the tree that got every one of them from the cache. The 28 that did not are
each explained above: four embed the build timestamp, five embed per-tree
generated blobs and are declined for `.incbin`, eighteen are the
`*.gendwarfksyms.o` scratch objects compiled from stdin, and the last is
`vmlinux.o`, which aggregates them.

Two stronger checks, because object equality is not quite the question a kernel
build cares about:

```console
$ cmp plain/System.map     cached/System.map
$ cmp plain/Module.symvers cached/Module.symvers
```

Both are byte-identical to a build that never touched the cache: the same
symbols at the same addresses, and the same module ABI checksums — which
`gendwarfksyms` derives from the DWARF in the cached objects, so it is a real
test of them. Strip the debug info from any two corresponding objects and they
are identical too; the paths are the only thing the cache changes.
