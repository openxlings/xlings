# xpkg Manifest (spec v1)

> Status: in force. Loaded by libxpkg (`mcpplibs/libxpkg`); installed by
> `src/core/xim/installer.cppm`. Additions are marked with the client version that
> reads them.

## 1. Overview

An xpkg manifest is a Lua script (`.lua`) declaring a package's metadata, its
platform resource matrix and its lifecycle hooks. The file defines a global table
named `package` at top level, and optionally the functions `install()`,
`config()` and `uninstall()`.

The libxpkg loader runs the file in a sandbox; xmake-style calls such as
`import()` do nothing while loading and take effect only during an install.

## 2. Required fields

| Field | Type | Meaning |
|---|---|---|
| `spec` | string | the spec version, `"1"` |
| `name` | string | the package name, lower case, unique within the index |
| `type` | string | the package type, §4 |
| `xpm` | table | the platform–version resource matrix, §5 |

## 3. Optional fields

| Field | Type | Meaning |
|---|---|---|
| `namespace` | string | a namespace such as `"subos"`, separating package names |
| `description` | string | one sentence |
| `licenses` | string[] | SPDX license identifiers |
| `repo` | string | the source repository URL |
| `homepage` | string | the project's home page |
| `docs` | string | the documentation URL |
| `archs` | string[] | supported architectures, e.g. `{"x86_64", "arm64"}` |
| `status` | string | `"dev"` (default) / `"stable"` / `"deprecated"` |
| `categories` | string[] | category tags |
| `keywords` | string[] | search keywords |
| `xvm_enable` | boolean | register with the xvm version manager (default false) |
| `maintainers` | string[] | maintainers |
| `authors` | string[] | authors |
| `programs` | string[] | the executable names the package provides |

## 4. Package types (`type`)

| Value | Internal number | For |
|---|---|---|
| `"package"` | 0 | a standard package (the default) |
| `"script"` | 1 | a script tool |
| `"template"` | 2 | a project template |
| `"config"` | 3 | a configuration set |
| `"subos"` | 4 | a SubOS environment |

## 5. The `xpm` platform–version matrix

`xpm` is a top-level table whose keys are platform names (`linux` / `macosx` /
`windows`) and whose values are that platform's version entries.

### 5.1 Version entries

In a platform table each string key is a version, and its table has these
optional fields:

| Field | Type | Meaning |
|---|---|---|
| `url` | string / table | the download URL; a table is a mirror table `{GLOBAL=..., CN=...}` |
| `sha256` | string / nil | the checksum; nil means not yet computed |
| `ref` | string | refers to another version (an alias), e.g. `latest` → a real version |
| `revision` | integer | the packaging revision, a non-negative integer, 0 when absent; §5.1.1 |
| `floor` | integer | the oldest installed revision still usable, `0 ≤ floor ≤ revision`, 0 when absent; §5.1.2 |

```lua
["latest"] = { ref = "20.1.7" },
["20.1.7"] = {
    url = "https://example.com/pkg-20.1.7.tar.xz",
    sha256 = "abcdef...",
},
```

#### 5.1.1 The packaging revision (xlings 2026.9.27.1, libxpkg 0.0.58)

When the upstream version stays the same and what is installed changes (a
rebuilt archive, a corrected install hook), the version entry says so by raising
`revision`. An absent value, or one that is not a non-negative integer, reads as
0; a `ref` alias carries no revision of its own — the value comes from the entry
the alias finally points to.

xlings writes the revision into the payload's `.xpkg-install.json`
(`"revision": N`) when it installs it. There is one rule: a payload whose
recorded revision equals the recipe's revision for that version is current;
otherwise it counts as not installed, and the next `xlings install` reinstalls
that version and says why, e.g.
`reinstalling glibc@2.44.3: recipe revision 1, installed revision 0`.

- A record without `revision` (written by a client before 2026.9.27.1) reads as
  0, so a recipe declaring `revision = 1` supersedes every older payload, and a
  recipe declaring none triggers no reinstall.
- A payload directory without `.xpkg-install.json` is not judged; behaviour is as
  before.
- The payload is present throughout a reinstall: the archive is downloaded and
  verified first, the old payload is moved to `<data>/stale/`, and the new one is
  installed at the original path (recipes write `install_dir()` into the files
  they install, so installing elsewhere and renaming is not possible). On success
  the old payload is deleted; on failure the incomplete new one is deleted and the
  old one moved back. Files other tools wrote into the old payload directory
  (such as mcpp's `.mcpp_ok`, `.mcpp-fixup.json`) go with the old payload.
- Before the old payload is moved, a marker recording its original path and the
  owning process is written in `<data>/stale/`. If the process is killed, a later
  `install` handles the leftover once that process no longer exists: moved back
  when the original path is missing or empty, deleted otherwise.

#### 5.1.2 The usable floor (xlings 2026.10.17.1, libxpkg 0.0.65)

`revision` says what a new install gets. Whether a payload installed at an older
revision may still be used is a separate fact, stated by `floor`: a payload is
*usable* when it is complete and its recorded revision is at least the entry's
`floor` (a payload without a record is usable). Absent, `floor` is 0 and every
installed revision stays usable; a value above `revision` reads as `revision`.
The recipe raises it only when the new revision fixes something every installed
payload has.

xlings reports both facts in `payload_status` and `install_targets`
(`current`: the recorded revision equals `revision`; `usable`), and its own
`install` replaces a payload that is not current, as in §5.1.1: an install is
the explicit request for the recipe's current packaging, and the dependencies it
plans follow the same rule because the new payload was written against them. A
client that only uses a payload keeps it while it is usable (mcpp's builds).

```lua
["2.44.3"] = {
    url = "https://example.com/glibc-2.44.3-linux-x86_64.tar.gz",
    sha256 = "abcdef...",
    revision = 1,
},
```

A **published resource never changes**: a rebuilt archive gets a new asset name
and the version's `revision` is raised (xim-pkgindex checks both).

### 5.2 Platform-level fields

Besides version entries a platform table may hold these keys, which the loader
does not read as versions:

- `deps` — dependencies, §7;
- `inherits` — a string: inherit another platform's table;
- `exports` — export declarations (advanced, for ELF patching).

### 5.3 Resource sources (xlings 0.4.63, libxpkg 0.0.44)

`xpm.source` is an optional default source that keeps the `platform -> version`
model. It is not a new version DSL: version entries, `ref`, mirrors and explicit
`url`s resolve as above.

```lua
xpm = {
    source = "xlings-res", -- the official resource server
    linux = {
        ["latest"] = { ref = "1.2.0" },
        ["1.2.0"] = {
            sha256 = {
                x86_64 = "<linux-x86_64-sha256>",
                aarch64 = "<linux-aarch64-sha256>",
            },
        },
    },
}
```

`source` takes two kinds of string:

- `"xlings-res"`: xlings builds the URL from the user's resource-server setting.
  The recommended form for official binaries, with a SHA-256 for every supported
  architecture.
- a URL template: an ordinary HTTP(S) template using `${name}`, `${version}`,
  `${os}`, `${arch}`, `${arch_alias}` and `${ext}`.

The source may sit at the root of `xpm` or in a platform table; the platform's
value overrides the root's. An explicit `url` in a version entry always overrides
the default `source`:

```lua
xpm = {
    source = "https://github.com/acme/foo/releases/download/${version}/foo-${os}-${arch_alias}.${ext}",
    linux = {
        ["1.0.0"] = {
            arch_alias = { x86_64 = "amd64", aarch64 = "arm64" },
            sha256 = { x86_64 = "<amd64-sha256>", aarch64 = "<arm64-sha256>" },
        },
    },
}
```

Resources for several architectures keep the version-entry model, in either form:

```lua
-- a URL template, with a hash per architecture
["1.0.0"] = {
    url = "https://example.test/foo-${version}-${arch}.tar.gz",
    sha256 = { x86_64 = "<x86-hash>", aarch64 = "<arm-hash>" },
}

-- an irregular upstream: one resource per architecture
["2.0.0"] = {
    x86_64 = { url = "https://example.test/foo-amd64.tar.gz", sha256 = "<x86-hash>" },
    aarch64 = { url = "https://example.test/foo-arm64.tar.gz", sha256 = "<arm-hash>" },
}
```

The older forms — `"XLINGS_RES"`, `res = true`, a single URL, a mirror table, a
single hash — remain accepted; `res = true` with `sha256` is historical input,
and a new package uses `xpm.source = "xlings-res"`. A newer libxpkg fails closed
when a multi-architecture entry lacks the hash for an architecture; an entry
with no hash at all still downloads for old packages' sake, but nothing then
verifies it cryptographically.

libxpkg's compat layer normalises every form into the final platform,
architecture, URL, mirror and SHA-256; the xlings installer keeps no second Lua
parser. A package author checks the target client versions and the index's CI,
and never parses `source` or builds a resource-server address in a hook. The
resolution order is [xpkg-resource-resolution](xpkg-resource-resolution.md).

## 6. Lifecycle hooks

Hooks are top-level functions (outside the `package` table) the installer calls
at its stages.

| Function | Called | Does |
|---|---|---|
| `install()` | after the resource is downloaded and unpacked | places the files in `pkginfo.install_dir()`; returns bool |
| `config()` | after a successful install, once per scope | registers xvm shims, sets environment; returns bool |
| `uninstall()` | when the user removes the package | removes the xvm registration and custom configuration; returns bool |

Every hook returns `true` for success and `false` for failure. For
`type = "subos"`, a missing hook is replaced by the installer's built-in default
(a skeleton directory and the xvm registration).

For a package that is neither `type = "subos"` nor defines `install()`, the
installer runs no code to place files: `pkginfo.install_dir()` holds exactly the
entries of the package's own archive, laid out as the archive lays them out —
its top-level directory is not stripped — and nothing else; the archive itself
stays in the download cache and is never moved into the install directory.

A hook reaches these modules through `import()`:

- `xim.libxpkg.pkginfo` — install paths, version information;
- `xim.libxpkg.xvm` — xvm operations;
- `xim.libxpkg.log` — logging.

A command a hook runs writes to `<home>/logs/hooks/<ns>-<name>@<ver>.<hook>.log`;
a recipe command that must reach the terminal passes
`system.exec(cmd, { tty = true })`. A recipe that changes what its `config()`
does raises `revision` (§5.1.1): that is the only signal every scope acts on.

## 7. Dependencies (`deps`)

`deps` sits at the `xpm.<platform>` level, in one of two forms.

### 7.1 Array form (older: runtime and build alike)

```lua
deps = { "node", "npm" }
```

Every entry is both a runtime and a build dependency.

### 7.2 Table form (recommended: explicit)

```lua
deps = {
    runtime = { "python" },
    build   = { "cmake", "ninja" },
}
```

- `runtime` — runtime dependencies, activated in the workspace after the install;
- `build` — build dependencies, available only while `install()` runs and not
  activated in the workspace.

A build dependency reaches the hook through the `XLINGS_BUILDDEP_<NAME>_PATH`
environment variable and PATH.

A bare dependency name means the package of that name in **the index the recipe
came from**; only a name that index lacks falls through to the global rule. A
version or platform the declaring index cannot satisfy is an error naming it,
never a reason to take another index's package of the same name.

### 7.3 Version constraints (a general grammar since 2026.8.9.2)

A dependency's `@` suffix and an install target's version hint share one grammar.
**Since 2026.8.9.2 a version is not limited to three-part semver:**

```
version  = field ('.' field)*  ('-' pre-release)?      -- a digit must precede '-'
field    = [0-9A-Za-z]+, cut into segments at digit/letter boundaries
segment  = a numeric segment (compared as a number) | an alphabetic segment (compared lexically)
valid    = at least one numeric segment; an alphabetic-only string ("latest", "abc") is a name, not a version
```

Ordering: numeric segments compare as numbers (`0.0.100` > `0.0.11`);
alphabetic ones lexically, and the boundary cut makes it a natural order
(`rc9` < `rc10`); **a number beats a letter** (`6.5` > `6.5rc1`, as semver puts
a pre-release below its release); a missing segment is 0 (`1.2` ≡ `1.2.0`); a
pre-release is below the same release; a parsable version always sorts before an
unparsable sentinel.

Constraint forms (an N-part version may appear anywhere):

| Expression | Meaning |
|---|---|
| `2.15.0.1` | exact (a prefix match on the parts written, at least three: matches `2.15.0.1`, not `2.15.0`) |
| `15` / `15.1` | prefix ranges `[15,16)` / `[15.1,15.2)` |
| `>=2.15.0.1` | a range; four-part and alphabetic segments take part (under the old grammar this **never matched** a four-part key) |
| `^2026.8.9.1` | carry at the first non-zero part: `[2026.8.9.1, 2027)` |
| `~2.15.0.1` | carry at the second part: `[2.15.0.1, 2.16)` |
| `1.2.*` | a wildcard, as the prefix range |

**Compatibility contract**: on numeric versions of up to three parts (with
pre-releases), every (version, expression) verdict is identical to before
2026.8.9.1 — the extension only defines inputs that were undefined.

**For indexes**: a constraint only the new grammar parses (such as
`>=2.15.0.1`) fails to parse on older clients; until the index's client floor is
at least 2026.8.9.2, pin four-part keys exactly.

## 8. Complete examples

### 8.1 `type = "subos"`

```lua
package = {
    spec = "1",
    name = "py-demo",
    namespace = "subos",
    description = "A Python development SubOS",
    licenses = {"MIT"},
    type = "subos",
    archs = {"x86_64", "arm64"},

    xpm = {
        linux = {
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"]  = {},
        },
        macosx = {
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"]  = {},
        },
        windows = {
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"]  = {},
        },
    }
}
-- a subos needs no hooks: the installer makes the skeleton and the xvm registration
```

### 8.2 `type = "package"`, with dependencies and hooks

```lua
package = {
    spec = "1",
    name = "llvm",
    description = "LLVM compiler infrastructure and toolchain",
    licenses = {"Apache-2.0 WITH LLVM-exception"},
    type = "package",
    repo = "https://github.com/llvm/llvm-project",
    docs = "https://llvm.org/docs/",
    archs = {"arm64"},
    status = "stable",
    categories = {"compiler", "toolchain"},
    keywords = {"llvm", "clang"},
    xvm_enable = true,

    xpm = {
        macosx = {
            deps = {
                runtime = { "libcxx" },
                build   = { "cmake" },
            },
            ["latest"] = { ref = "20.1.7" },
            ["20.1.7"] = {
                url = "https://example.com/LLVM-20.1.7-macOS-ARM64.tar.xz",
                sha256 = "abcdef1234567890...",
            },
        },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    os.tryrm(pkginfo.install_dir())
    os.mv(pkginfo.install_file():replace(".tar.xz", ""), pkginfo.install_dir())
    return true
end

function config()
    xvm.add(package.name, { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove(package.name)
    return true
end
```
