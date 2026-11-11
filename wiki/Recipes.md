# Recipes

Each folder in `pkgs/` is a **recipe** that builds one apk package. The format
is this repository's own, smaller than Alpine's `APKBUILD`. `tools/lib.sh`
turns a recipe into a signed `.apk` with apk-tools 3's `apk mkpkg`. abuild is
not used.

## Files

```
pkgs/NAME/
  meta          name-less metadata, as shell variables
  build         the build script
  sources       optional: what to download
  sha256sums    optional: checksums for sources
  files/        optional: local files, patches, configs
  scripts/      optional: NAME.pre-install, NAME.post-install, ...
```

### meta

```sh
version=1.0.5
# Above any Alpine release of 1.0.5, so apk prefers this build.
release=100
desc="libudev replacement fed by mdevd, with sound cards visible to PipeWire"
url=https://github.com/illiliti/libudev-zero
license=ISC
makedepends="build-base linux-headers"
provides="so:libudev.so.1=1"
provider_priority=20
```

| Field | |
|---|---|
| `version`, `release` | the package is `NAME-VERSION-rRELEASE`. Raise `release` for a rebuild of the same version |
| `desc`, `url`, `license` | as in Alpine |
| `depends` | runtime dependencies. Shared libraries are found by themselves (below), so list only programs and data |
| `makedepends` | Alpine packages needed to build, installed as a virtual `.busylinux-build` package and removed after |
| `provides`, `replaces`, `provider_priority` | as in apk |
| `options` | `nostrip` keeps debugging symbols (the kernel) |

The file is sourced by `sh` in a subshell, so recipes cannot leak values
into each other. Comments are welcome.

A `release` of **100 or more** is a convention here. When Alpine has the
same package and version, the higher release makes apk prefer this build.
The `@busylinux` pin keeps it anyway.

### sources and sha256sums

One source per line, with an optional subfolder to unpack into:

```
https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.62.tar.xz
git+https://github.com/illiliti/libudev-zero#1.0.5
https://example.org/extra.tar.gz  third_party/extra
```

- **Tarballs** (`.tar`, `.tar.*`, `.tgz`) are unpacked with the top folder
  stripped. Other files are copied as they are. Downloads are cached in
  `sources/` in the build cache.
- **git+URL#TAG** makes a shallow clone of that tag.

`sha256sums` has one line per source, `CHECKSUM  KEY`:

```
490a7aedf1b355c565d3a6c05333ccccb5da82064eca46b590448a7099d7d651  linux-6.18.62.tar.xz
7a6eee2db11f2eb0f7fd065ae9597fcd270e6734  libudev-zero#1.0.5
```

For a download, the key is the file name and the checksum its SHA-256. For
git, the key is `REPO#TAG` and the "checksum" is the **commit hash** the tag
must point to. A moved tag fails the build like a changed tarball would. A
wrong or missing line fails with the line to put there, so a new version
is: change `version` and the URL, build once, and copy the hash from the error
after checking it against upstream.

### build

Runs with `sh -e` in the unpacked source folder, with `files/` copied into
it. `$1` is the install root, `$2` the version, and `MAKEFLAGS` uses every
core:

```sh
#!/bin/sh -e
patch -p1 < sound-cards.patch
make PREFIX=/usr
make PREFIX=/usr DESTDIR="$1" install-shared
```

After it runs:

1. **Stripping**: ELF files lose symbols (`.a` and `.o` only debug info,
   kernel modules nothing), unless `options="nostrip"`.
2. **Library dependencies**: every ELF's `NEEDED` libraries that the package
   does not ship become `so:` dependencies, as abuild does. The `-dev`
   packages that pulled them in during the build are gone afterwards, and
   without this the program would fail with "symbol not found" or
   "library not found".
3. **Packaging and signing** with `apk mkpkg`.

### scripts

`scripts/NAME.post-install`, `NAME.pre-upgrade` and so on become the
package's install scripts, as in Alpine.

## Building one

On the build machine, all recipes build whenever the container runs. Only
recipes whose files changed are rebuilt. On the installed machine:

```sh
doas tools/update.sh NAME
```

See [Updating](Updating.md) for how that works.

## Adding a package

1. Make `pkgs/NAME/` with `meta`, `build`, and `sources` + `sha256sums` if it
   downloads anything.
2. Build it: `doas tools/update.sh NAME`. It installs it too.
3. If it should be in every image, add `NAME@busylinux` to `BASE` in
   `tools/build.sh`. Otherwise it is in the local repository for
   `apk add NAME@busylinux`.
4. CI shellchecks `build` scripts. The boot test installs the desktop
   packages and checks their libraries resolve; add a check to
   `tests/boot-test.sh` for anything that should not break unnoticed.

## The recipes

| Recipe | Why it is built here |
|---|---|
| `linux-busylinux` | the kernel and its sched_ext scheduler, see [Hardware and kernel](Hardware-and-Kernel.md) |
| `busylinux-init` | `inittab`, `rcS`, `rcK`, services, sysctls, firewall, profile; replaces `alpine-baselayout`'s OpenRC bits |
| `libudev-zero` | Alpine's, plus a patch that makes sound cards visible to PipeWire |
| `nftables` | without GNU GMP and Readline |
| `busybox-runit` | runit's `runsvdir`, `runsv`, `sv`, `svlogd` and `chpst` from BusyBox (one 94 KB binary), in place of Alpine's `runit`; Alpine's BusyBox has none of them |
| `dwl` | patches and `config.h` are compiled in |
| `gammastep` | Wayland-only, without GeoClue, GUI or translations |
| `pipemixer`, `cclip` | not in Alpine |
| `claude-code` | not in Alpine: Anthropic's own musl build from the npm registry, unstripped (stripping cuts the program off its Bun runtime), updated by the recipe and not by itself |
| `wgcf` | newer than Alpine's |
| `busylinux-fonts` | fonts Alpine lacks or packages older, with the fontconfig setup that ties them together |
