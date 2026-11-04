# fpm: Freedom Package Manager

A fast, small package manager in C, in the spirit of Void's xbps and Arch's
pacman + AUR helpers:

- **Binary repositories** you can host yourself (any static web server, or a
  plain directory). Indexes are signed with ed25519, and every download is checked.
- **Recipes** for building from source, AUR style: community recipe
  repositories in git, with a review step before anything new or changed is built.
- **Automatic shared-library dependencies**: fpm reads the ELF files it packages,
  so a program that links `libfoo.so.1` pulls in whatever provides it, and an
  upgrade that would remove a library something still uses is refused.
- **Generations and rollback**, atomic installs, crash repair, holds, hooks,
  provides / conflicts / replaces, split packages, parallel builds and downloads.

About 5,800 lines of C11 with no library dependencies; a ~130 KB binary.
Runtime tools: GNU `tar`, `zstd`, `curl`, `sh`, `openssl` (signatures).
Optional: `bwrap` (sandboxed builds), `git`, `strip`.

On 4,000 installed packages, `list`, `info`, `owner`, `rdeps`, `orphans` and
planning an install or removal each take about 0.01–0.02 s.

## Commands

```
packages:   install  remove  upgrade  autoremove  hold  unhold  rollback  history  repair
queries:    search  info  list  files  owner  deps  rdeps  orphans  check  show
repos:      sync  build  index  keygen  clean
```

| Command | What it does |
|---|---|
| `install PKG...` | From a binary repository, or built from a recipe, with all dependencies |
| `remove PKG...` | Refuses if an installed package still needs it (by name, provide or library) |
| `upgrade` | Everything outdated, except held packages |
| `autoremove` / `orphans` | Remove / list dependencies nothing explicitly installed needs |
| `hold` / `unhold PKG` | Pin a package at its current version |
| `rollback [GEN]` / `history` | Return to an earlier system state / list them |
| `repair` | Finish a transaction a crash interrupted (also automatic) |
| `search [TERM]` | Binary repositories and recipes |
| `info PKG` | Versions, where it comes from, dependencies, libraries, size |
| `deps PKG` / `rdeps PKG` | What it needs / what needs it |
| `check [PKG...]` | Missing files and edited config files |
| `show PKG` | Print a recipe and its build script |
| `sync` | Update binary indexes (signature-checked) and recipe repositories (git) |
| `build PKG...` | Build into the package cache without installing |
| `index DIR [--sign KEY]` | Write a repository index for the packages in DIR |
| `keygen NAME` | Create `NAME.key` (private) and `NAME.pub` (public) |
| `clean [--keep N] [--sources]` | Delete cached packages no generation needs |

Options: `-y` (don't ask), `-n` (dry run), `-f` (force), `--overwrite` (replace
existing files no package owns, e.g. when adopting a hand-built system), `-S` (build from source
even if a binary exists), `-P N` (parallel builds), `--reinstall`, `--rebuild`,
`--keep-build`, `-e` (list: explicit only), `--root DIR`, `--config FILE`.

A transaction looks like this:

```
$ fpm install hello
Packages (2):
  greet-data                   1.0-1              repo main
  hello                        1.0-1              repo main
Download: 2.4 KiB   Installed size: +10.7 KiB (binary packages)
Proceed? [Y/n]
==> Downloading greet-data 1.0-1 from main
==> Downloading hello 1.0-1 from main
==> Installing greet-data 1.0-1
==> Installing hello 1.0-1
```

## Configuration: `/etc/fpm.conf`

```ini
cflags   = -O2 -pipe -march=native
jobs     = 0            # make -j per build; 0 = all CPUs
parallel = 2            # packages building at once
prefer   = binary       # or source: which wins when both have the same version
# proxy  = socks5h://127.0.0.1:9050

[repo main]                                   # binary repository
url = https://pkgs.example.org/x86_64         # https://, http:// or file://
key = /etc/fpm/keys/main.pub

[recipes community]                           # recipe repository (AUR style)
path = /var/db/fpm/recipes/community
git  = https://git.example.org/recipes.git    # fpm sync clones / pulls it
review = yes                                  # show new or changed recipes before building
require_signed = yes                          # refuse an unsigned git HEAD
```

Packaging policy: `usrmerge = yes` moves files a build put in `/bin`, `/sbin`,
`/lib`, `/lib64` or `/usr/sbin` into `/usr/bin` and `/usr/lib` (for systems
where those are symlinks), and `purge = GLOB...` deletes generated clutter from
every package (default `usr/share/info/dir`; e.g. add `*.la`).

Repositories are searched in order; the first one that has a package wins. When
both a binary and a recipe exist, the newer version wins (`prefer` breaks ties).

## Running your own repository

```sh
fpm keygen /root/keys/main                 # main.key (secret) + main.pub (publish)
fpm build hello zlib openssl               # or build on a dedicated build machine
cp /var/cache/fpm/pkgs/*.fpm.tar.zst /srv/repo/x86_64/
fpm index /srv/repo/x86_64 --sign /root/keys/main.key
# serve /srv/repo with any web server; users add [repo] with your main.pub
```

`fpm index` only reads archives it hasn't seen before and lists every version
in the directory (delete old archives to drop them), then writes
`index.fpm.zst` plus its signature. Installs take the newest version unless a
constraint asks otherwise (`fpm install 'zlib<1.3'`, or a dependency like
`zlib>=1.2`), and `rollback` re-downloads old versions the local cache no
longer has. Clients refuse:

- an index whose signature doesn't verify against the configured key,
- an index with no signature (unless the repo says `allow_unsigned = yes`),
- an index **older** than the one they already have (a stale or malicious mirror
  can't roll them back to vulnerable versions),
- an archive whose sha256 doesn't match the signed index.

## Recipes

```
community/zlib/
├── recipe          metadata (required)
├── build           shell script, run with sh -e (optional: metapackages have none)
├── post-install    run after install (optional)
├── pre-remove      run before removal, while all files are still there (optional;
│                   if it fails, nothing is removed unless --force)
└── *.patch ...     local files referenced as sources
```

```ini
name        = zlib
version     = 1.3.1
release     = 1
description = Compression library implementing the deflate method
license     = Zlib
url         = https://zlib.net
depends     = glibc
makedepends = make
provides    = libz
conflicts   = zlib-ng-compat
replaces    = zlib-old
arch        = x86_64          # default: this machine; "noarch" for data-only packages
source      = https://zlib.net/fossils/zlib-${version}.tar.gz
sha256      = 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23

zlib-dev.description = Headers and static library for zlib
zlib-dev.depends     = zlib
zlib-dev.files       = usr/include usr/lib/*.a usr/lib/pkgconfig
```

- **Dependencies** take version constraints (`foo>=1.2`, `foo=2`, `foo<3`) and may
  name anything a package `provides` (e.g. `depends = sh`). Shared-library
  dependencies don't need to be listed: they are found from the ELF files.
- **conflicts** stops the install while the other package is installed;
  **replaces** removes the other package and takes over its files (for renames
  and merges).
- **Sources**: tarballs (unpacked), local files (copied), and git:
  `git+URL#commit=…` (pinned, works offline once mirrored), `#tag=…` or
  `#branch=…` (not reproducible). Submodules are fetched too, at the commits
  the checkout pins (this needs the network). Remote tarballs need a real
  `sha256`; git uses `SKIP` because the commit pins it.
- **Options**: `options = !strip` keeps binaries unstripped (signed or
  prebuilt ones); `options = !shlibs` records no shared-library requirements,
  for programs whose libraries come from their own runtime (e.g. a container),
  not the system.
- **Split packages**: `<sub>.files` moves matching paths (globs; a directory
  takes its contents) into a separate package; each subpackage can also have
  `depends`, `provides`, `conflicts`, `replaces` and `description`.
- `build` runs in the source directory with `DESTDIR`, `srcdir`, `name`,
  `version`, `release`, `JOBS`, `MAKEFLAGS`, `CFLAGS`, `CXXFLAGS`, `LDFLAGS` set.

## Hooks

Run once after a transaction when a changed file matches. Packages ship them in
`/usr/share/fpm/hooks/`; a file of the same name in `/etc/fpm/hooks/` overrides
one (an empty file disables it).

```ini
# /usr/share/fpm/hooks/ldconfig.hook
paths = usr/lib/lib*.so* usr/lib64/lib*.so*
exec  = ldconfig
```

With `--root`, hooks run inside the target root (`chroot`), which needs root;
`chroot = no` runs them on the host with `$ROOT` set instead.

## How it works

**Builds** fetch and verify sources, then run the build script in bubblewrap when
available: the system is read-only, only the build tree is writable, and there's
no network. Independent packages build in parallel; each writes
`/var/cache/fpm/logs/<recipe>.log`, and a failure prints its end.

**Installs** unpack to a stage directory and check, before touching anything:
architecture, shared libraries (everything needed is provided; nothing still in
use is dropped), and file conflicts. Files then move into place with atomic
`rename()`, so running programs never see half-written binaries. Files under
`/etc` that you edited are kept (the new version is saved as `.fpmnew`; on removal
yours stays as `.fpmsave`).

**Binary downloads** happen up front and in parallel; nothing is installed if any
of them fails or doesn't match the signed index.

**Generations**: after every transaction fpm records exactly which packages and
versions are installed. Archives stay in the cache, so `rollback` can restore any
recorded state; it's recorded as a new generation, so you can roll forward too.

**Crash repair**: each install or removal is journaled first. After a crash the
next command that changes anything (or `fpm repair`) redoes the interrupted step.

**SHA-256** is used for exactly two things: verifying downloads (sources and
binary packages) and detecting edited `/etc` files.

**Database**: plain text under `/var/lib/fpm` (`installed/<pkg>/{meta,files,hold}`,
`generations/`, `sync/<repo>/`, `reviewed/`, `pending`), read once per run and
cached in memory. A lock file prevents two fpm processes from running at once.

## Build & test

```sh
make                    # ./fpm
make check              # 162 end-to-end tests in temporary roots, no root needed
make LDFLAGS=-static    # static binary (needs a static libc)
sudo make install       # /usr/bin/fpm and /etc/fpm.conf
```

The tests cover installs with dependencies, conflicts, cycles, version
constraints, checksums, dry runs, config-file handling, upgrades, rollback,
crash repair, parallel builds (timed), git sources and submodules, split
packages, provides / conflicts / replaces, pre-remove scripts, holds, hooks,
queries, AUR-style review, and binary
repositories end to end: key generation, signed indexes, installs without
building, automatic library dependencies, refusing an upgrade that breaks a
library user, multiple versions per package (constraints, downgrades, rollback
re-downloading), and rejecting a replayed index, a wrong signature, a missing
signature and a tampered archive.

## Limitations

- Repair redoes the interrupted step; if a multi-package command was cut off,
  run it again for the packages it hadn't reached.
- Rollback needs each old version either in the local cache or still in a
  repository.
- Git submodules aren't mirrored, so rebuilding a recipe that uses them needs the
  network even when the main commit is cached.

The name `fpm` is also used by an unrelated Ruby packaging tool. If both end up on
one system, rename this binary (e.g. `freedom`) in the Makefile.

## License

Licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE).
