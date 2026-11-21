# Bundles (`bundle.toml`)

> Status: in force since 2026.11.21.1. Written by `xlings download --to`, read by
> `xlings install --from` and by every archive directory lookup; implemented in
> `src/core/xim/archives.cppm` and `src/core/xim/bundle.cppm`. Additions are marked
> with the client version that reads them.

## 1. Overview

A bundle is a directory that carries what an install needs on a machine without
a network: the archives of the packages' closures, optionally a snapshot of every
index that resolved them, and a manifest, `bundle.toml`, at its top. A bundle is
also an archive directory ([`.xlings.json` `archives`](xlings-json.md)): any of
its archives is found by its sha256 whether the directory is given to `install
--from`, listed in `archives` or in `XLINGS_ARCHIVES`.

```
<dir>/
├── bundle.toml
├── archives/<sha256>-<file name>      one per archive, named by content
└── index/<name>/                      with --with-index: one per index
```

The layout under `<dir>` is described by the manifest; a reader takes paths from
the manifest, not from this picture.

## 2. Writing: `xlings download`

```bash
xlings download <package>[@<version>]... --to <dir> [--with-index] [--host <os>-<arch>]
```

- The packages resolve as `install` resolves them for a home that has nothing
  installed: the dependency closure of the index's answer, on the host named by
  `--host` (`linux`, `macosx` or `windows`, then an architecture such as
  `x86_64`, `aarch64`, `arm64`; default this machine). On Linux the closure
  includes `xim:patchelf`, which an install on a host without patchelf takes first.
- Each archive is obtained through the archive sources (the download cache, the
  archive directories, the network) and written to `archives/<sha256>-<file
  name>`, the file name being the last segment of its URL. A file is written
  beside its final name, verified and renamed into place, so no reader sees a
  partial file. For a recipe that states no sha256, the digest of the bytes
  obtained is recorded.
- A package that downloads nothing (payload-free) contributes no archive. One
  whose source is a git repository, or that the system's package manager
  installs, is not carried; the command names it.
- With `--with-index`, every index that answered a package of the closure is
  copied to `index/<name>` without its `.git` tree and parsed-index cache,
  replaced whole.
- The manifest is replaced atomically. When `<dir>` already holds one, its
  entries are kept: an archive entry of the same package, version and host is
  replaced, an index entry of the same name is replaced, and the tables this
  client does not know are written back unchanged after the known ones. A
  manifest that cannot be read stops the command before anything is written.

## 3. The manifest

TOML. The tables this specification defines:

```toml
[bundle]
format = 1
created_by = "xlings 2026.11.21.1"
host = "linux-x86_64"

[[archive]]
package = "xim:gcc"
version = "16.1.0"
host = "linux-x86_64"
sha256 = "5469a5fdf6a8bedc76e8049a7f2a0d1ecbe87d6ddf3cdf28bf1a3a9978460dd6"
file = "archives/5469a5fdf6a8bedc76e8049a7f2a0d1ecbe87d6ddf3cdf28bf1a3a9978460dd6-gcc-16.1.0-r1-linux-x86_64.tar.gz"

[[index]]
name = "xim"
revision = "8c1f3e0d2b..."
path = "index/xim"
```

| Table, key | Meaning |
|---|---|
| `[bundle] format` | the manifest's format, `1`; a reader refuses a higher one and names `xlings self update` |
| `[bundle] created_by` | `xlings <version>` of the last writer |
| `[bundle] host` | `<os>-<arch>` of the last `download` into the directory; each archive names its own |
| `[[archive]] package` | `<namespace>:<name>` |
| `[[archive]] version` | the package's version |
| `[[archive]] host` | `<os>-<arch>` the archive is for |
| `[[archive]] sha256` | lower-case hex; the archive's identity |
| `[[archive]] file` | its path relative to the bundle, `/`-separated |
| `[[index]] name` | the index's name, which is its namespace |
| `[[index]] revision` | the git commit of the copied tree, `artifact:<version>` for an index installed from an artifact, empty when the tree has neither |
| `[[index]] path` | the snapshot's directory relative to the bundle, `/`-separated; it holds `pkgs/` |

A reader ignores every table and every key it does not know. Other programs
append their own tables to the same file (mcpp writes `[[git]]`); xlings keeps
them when it rewrites the manifest. Values are TOML basic strings and integers;
a value of another type in a known table is ignored.

## 4. Reading: `xlings install --from`

```bash
xlings install --from <dir> <package>[@<version>]...
```

- `<dir>` becomes the first archive directory of the command, before
  `XLINGS_ARCHIVES` and `archives`; the download cache is still asked first.
- When the manifest lists indexes, each snapshot is an index of the command
  under its name (its `dir` is the snapshot) and they are the only ones: the
  home's indexes, their sub-indexes, a project's indexes and the local recipes
  are not read, nothing is fetched to resolve, and an absent package is not
  retried after a refresh. An `[[index]]` whose path leaves the bundle or holds
  no `pkgs/` stops the command with its name.
- Without indexes in the manifest, or without a manifest, `<dir>` is an
  archive directory only and the home's indexes resolve.
- `--from` is passed on to an install run in a SubOS's domain (`--subos`) and in
  the system layer (`--system`), as an absolute path.

An archive is taken from the bundle only when its bytes have its identity: the
sha256 the recipe states, or, for a recipe that states none, the `sha256` of the
`[[archive]]` with the same `package`, `version` and `host` in the manifest of
an archive directory.
