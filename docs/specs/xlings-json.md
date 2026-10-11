# `.xlings.json`

> Status: in force. Read and written by `src/core/config.cppm`; checked and repaired
> by `xlings self doctor`. Additions are marked with the client version that reads them.

## Overview

`.xlings.json` is xlings's one configuration file, in JSON. Where it is placed
decides its role: the home's configuration, a project's, or a SubOS workspace.

| Role | Path | For |
|---|---|---|
| the home | `$XLINGS_HOME/.xlings.json` (default `~/.xlings/.xlings.json`) | user defaults, the active SubOS, mirrors |
| a project | `<project>/.xlings.json` | the project's tools, its SubOS binding |
| a SubOS workspace | `$XLINGS_HOME/subos/<name>/.xlings.json` | what the SubOS has installed and active |

A project's `mirror`, `lang`, `uiMode`, `theme` and `tui` override the home's.

## The home's fields

| Field | Type | Meaning |
|---|---|---|
| `mirror` | `string` | the mirror (`GLOBAL`, `CN`): the default index URLs and the resource server |
| `lang` | `string` | the interface language: `"auto"` (default, follows the system) / `"zh"` / `"en"` |
| `uiMode` | `string` | the front end: `"auto"` (default) / `"cli"` / `"tui"` ([diagnostics](diagnostics.md), §7) |
| `theme` | `string` | a **path reference** to a colour theme, or a shipped theme's name ([themes](themes.md)) |
| `tui` | `object` | the tui front end's own settings; for now only `interactive` (bool, default `false`) |
| `hintsSeen` | `array` | the one-time hints already shown; maintained by xlings |
| `activeSubos` | `string` | the active global SubOS |
| `versions` | `object` | the home's version database, keyed by **target name** (not necessarily a package name); see below |
| `version` | `string` | the xlings version that made or last checked this home; written by a successful `self doctor --fix`, to tell whether records are in an older client's format |
| `index_repos` | `array` | package index repositories, each with `name` and `url` |
| `xim` | `object` | where the default index itself comes from; see below |
| `XLINGS_RES` | `object \| string \| array` | resource servers; see below |

### `versions` entries

The keys of `versions` are **xvm target names**: the names `xlings use` switches.
A target is not necessarily a package: llvm registers `clang`, `lld`, `llvm-ar`
and more, all targets, while the index has the one package `llvm`.

```json
"versions": {
  "python": {
    "type": "program",
    "filename": "python3",
    "versions": {
      "3.12.0":       { "path": ".../xim-x-python/3.12.0/bin" },
      "local:3.11.0": { "path": ".../local-x-python/3.11.0/bin" }
    }
  }
}
```

| Field | Meaning |
|---|---|
| `type` | `program` / `lib` / `files` / `group` |
| `filename` | the real file name in the payload, when it differs from the target's |
| `versions` | keyed by **version key**; each value is that version's record |
| `bindings` | legacy pairwise binding edges (older clients); newer clients write `bindingGroup` |

**A version key carries the namespace.** It is written `<namespace>:<version>`
(e.g. `local:3.11.0`), and a bare version has no namespace. Note the order is the
**reverse** of a command-line coordinate:

| Where | Written | Example |
|---|---|---|
| a version key (in this file) | `ns:version` | `local:0.0.27` |
| a command-line coordinate | `ns:package@version` | `local:mcpp@0.0.27` |
| `xlings use`'s version argument | `ns:version` | `xlings use mcpp local:0.0.27` |

**Who decides whether a key has a prefix, and when** (part of the contract since
2026.9.2.1):

- a package from the **default index** (the `index_repos` entry named `xim`) gets
  a **bare key**; any other index's gets the `ns:` prefix. The entry's **name**
  decides, never its **position** in the array — the entries are peers, and
  reordering them (`xlings index use`, `config --index-repo`) changes no key.
- a key's spelling is **decided at its first write and never derived again**.
  The same provider registering the same version again (a reinstall, a repair)
  keeps the spelling on disk; only a version never registered is keyed by the
  current rule.
- readers are **lenient both ways**: `xim:2.1.222` finds a record stored as
  `2.1.222` and the reverse. But an `ns:v` query **never** matches `other:v` in
  another namespace — that is another index's payload.
- a bare key and a prefixed key for the same version with the same `path` are
  **one registration written twice** (damage from when the rule followed array
  position). Every reader treats the pair as one record, the one with
  `bindingGroup` winning; `xlings self doctor` reports `duplicate version key`,
  and `--fix` merges them and rewrites every SubOS reference to the merged key.

Common fields of a version record:

| Field | Meaning |
|---|---|
| `path` | the payload directory, laid out `data/xpkgs/<ns>-x-<package>/<version>/...` |
| `alias` | the real command it forwards to; an absolute path points outside the payload |
| `envs` | environment variables set while this version is active |
| `bindingGroup` | the release group: `provider` / `providerVersion` / `rootTarget` ... |
| `bindingMembers` | the members, recorded on the group's root |
| `fileSrc` / `fileDst` | `type = "files"` only: the source in the payload and the destination under the SubOS root, both relative |

`xlings self doctor` checks these records against the disk; `--fix` repairs what
disagrees ([self-management](../06-self-management.md)).

### The lifecycle of declared files (`type = "files"`)

A recipe declares an entry that is neither a program nor a library (headers,
`.pc` files, certificates) with `xvm.files{src=, dst=, binding=}`. It **registers
as a target of its own**, named by libxpkg `<pkg>.files.<n>` (`<n>` counts the
declarations in order at install time), bound to the release — **not hung on the
package's own entry**.

The naming is part of the contract because it decides how to ask:

> **To list the files a release placed, walk its members**
> (`release_file_placements`). Asking `file_placement` by package name is always
> empty — not "no files", but the wrong question. Between 2026.7.27.0 and
> 2026.8.26.1 the removal path asked exactly that, so it reclaimed no file and
> said nothing (openxlings/xlings#423).

Both ends are relative: a payload is shared by several SubOSes, and an absolute
destination recorded on it would be right for one and wrong for every other.
The top of `dst` may be only `usr/`, `etc/` or `share/`: no absolute path, no
`..`, no `bin/` (that is where shims live).

**The client's guarantee (since 2026.8.26.1).** When a release is no longer
needed by this SubOS — a full removal (the payload deleted), a detach (another
SubOS still uses the payload), a re-registration that no longer declares the
destination, or `use` switching to a version with fewer files — the destinations
it declared are reclaimed. The four paths share one rule:

| The destination | Action |
|---|---|
| no surviving declaration | deleted, with empty directories that existed only for it (two-level paths such as `usr/include` stay) |
| a surviving declaration that is **active in this SubOS** | **re-pointed** at that release's payload |
| a surviving declaration not active in this SubOS | deleted |
| a link pointing outside the payload store (someone replaced it) | kept, with a warning: that is `xvm-sysroot-drift`, not for removal to decide (POSIX only; on Windows files are hard links or copies and only the declaration decides) |

So a recipe **never** hand-writes an `rm` in `uninstall()` mirroring its
declarations.

**How to verify.** Not with `[ -e ]`: it follows symbolic links, and a dangling
link that **still exists** after the payload is deleted reads as absent. Use
`-xtype l`; and for the detach half (the link does not dangle) only a
reconciliation works: the links under the SubOS pointing into the payload store,
minus the `fileDst`s this SubOS's active declarations name, must be empty.

### `index_repos`

```json
"index_repos": [
  { "name": "ros2", "url": "https://github.com/example/ros2-index" },
  {
    "name": "mcpplibs",
    "url": "https://github.com/mcpp-community/mcpp-index.git",
    "artifact": "https://github.com/xlings-res/mcpp-index",
    "source": "auto"
  }
]
```

The default index is always kept; user repositories are added to it.

| Field | Type | Meaning |
|---|---|---|
| `name` | `string` | the index's namespace (required) |
| `url` | `string` | a git remote or a local path (required; the fallback in artefact mode) |
| `artifact` | `string \| object` | (0.4.68+) the artefact base: a GitHub / GitCode repository URL, a static HTTP directory, a local directory or `file://`; or a region object `{"GLOBAL": .., "CN": ..}`, which is **an ordered chain** tried in turn before git ([index distribution](index-distribution.md), §5) |
| `source` | `string` | (0.4.68+) `auto` (default: artefact first, git as fallback) \| `artifact` (artefact only) \| `git` (git only) |

#### Entries are peers; `name` decides three things

The entries have **no rank**, and their order means nothing. Each entry's `name`
decides:

1. **the directory** — `data/<name>` (except the default index `xim`, at
   `data/xim-pkgindex`);
2. **the namespace** — packages are referred to as `<name>:<pkg>`;
3. **which index is downloaded** — the pointer entry keyed `<name>`.

(Before 2026.8.30.1 the default index was array index 0 while what it downloaded
was hard-wired to the official index, so a sub-index listed first received the
official index's content, #576. Now `data/<name>` only ever receives `<name>`'s
own index.)

An `index_repos` entry whose name and url match a sub-index the default index
declares is configuration for that sub-index (a pin, an artefact base), not a
second repository; the same name with another url is refused.

#### When an entry uses artefacts

**Configuration** decides, not the shape of the URL:

- it declares `artifact` itself; **or**
- its `url` equals the **declared source** for its `name`:
  - `xim` → `xim.index-repo` in this file (below);
  - a sub-index → the URL the default index's `xim-indexrepos.lua` declares for
    that name.

Otherwise there is no artefact, and it uses git. A local path or `file://` never
uses artefacts. `source: "git"` turns artefacts off for any entry — the escape
hatch for a self-hosted index with an official name.

**The artefact is the mechanism, git the fallback**: `auto` means "artefacts
first", not "one of the two". An entry declaring several regional bases tries
each in turn; git's turn comes only after the whole chain. `xlings index list`
shows the order.

### `xim`

Written by `xlings self install`; says where **the default index itself** comes
from:

```json
"xim": {
  "index-repo": "https://github.com/openxlings/xim-pkgindex.git",
  "index-base": "https://github.com/xlings-res/xim-index",
  "mirrors": {
    "index-repo": {
      "GLOBAL": "https://github.com/openxlings/xim-pkgindex.git",
      "CN":     "https://gitee.com/sunrisepeak/xim-pkgindex.git"
    },
    "res-server": { "GLOBAL": "...", "CN": "..." }
  }
}
```

| Field | Meaning |
|---|---|
| `index-repo` | the default index's (`name: "xim"`) git URL; **in effect since 2026.8.30.1** (before, it was written and never read) |
| `mirrors.index-repo` | the same per region; read before the flat `index-repo`, so changing `mirror` after installing follows. A region object here takes **the first only**: it names a git remote, and a clone has one origin |
| `index-base` | the base for pointers and release assets (self-hosting); `XLINGS_INDEX_BASE_URL` wins. A string or a region object — **an ordered chain** tried in turn, never falling back to the official servers once exhausted: an override is an override |

`index-repo` is also the default index's **declared source**: the `index_repos`
entry named `xim` receives official artefacts only when its `url` equals it. Point
it at a fork and official artefacts no longer land in your directory.

### Network bounds of an index refresh (environment, 2026.9.16.1+)

| Variable | Default | Effect |
|---|---|---|
| `XLINGS_UPDATE_TIMEOUT` | `300` (s) | the budget of one refresh; past it the remaining sources are **skipped and reported** (local copies stay). `off` disables |
| `XLINGS_INDEX_HTTP_TIMEOUT` | `10:120` | the index's HTTP `<connect s>:<per attempt s>`; tighter than package downloads because indexes are small and there is always another candidate. `off` uses the download defaults |
| `XLINGS_GIT_NETWORK_TIMEOUT` | `60` (s) | sets `GIT_HTTP_LOW_SPEED_TIME` (with `GIT_HTTP_LOW_SPEED_LIMIT=1000`); **a value the operator set is kept**. `off` sets nothing |
| `XLINGS_DOWNLOAD_LOW_SPEED` | `10240:15` | the stall watchdog `<bytes>:<s>`; `off` disables |

`GIT_TERMINAL_PROMPT=0` is set as well (again only when unset): an index sync never
needs credentials, so an unbounded wait at the terminal becomes an error.

### Downloads and install hooks (environment, 2026.9.30.1+)

| Variable | Default | Effect |
|---|---|---|
| `XLINGS_DOWNLOAD_SPACE_CHECK` | on | once the server gives a size, a disk that cannot hold the remaining bytes stops the transfer at its start as a local failure (`E_DISK_FULL`), instead of half-way. Unknown free space does not block. `off` disables (file systems reporting free space wrongly) |
| `XLINGS_HOOK_OUTPUT` | a log | the output of commands an install hook runs goes to `<XLINGS_HOME>/logs/hooks/<ns>-<name>@<version>.<hook>.log` (rewritten each run); a failed hook prints the last 20 lines and the path. `inherit` sends them to the terminal (as does `-v`). Always a log in the interface and the TUI |
| `XLINGS_HOOK_HEARTBEAT` | `10:30` | after `<first s>` a hook prints `… <pkg> <hook> hook running <elapsed>: <last log line>`, then every `<interval s>`; `off` disables |

A failed download is handled by cause: wrong content (sha256 mismatch) or a
refusing source — the next candidate, the host demoted; an incomplete transfer —
retried on a later round; a local write failure — stop at once (another mirror
writes to the same disk), demote nobody, `E_DISK_FULL` with the free space.

### `XLINGS_RES`

Three forms:

```json
{
  "XLINGS_RES": {
    "DEFAULT": ["https://res1.example.com", "https://res2.example.com"],
    "cn": "https://cn-mirror.example.com"
  }
}
```

```json
{ "XLINGS_RES": "https://res.example.com" }
```

```json
{ "XLINGS_RES": ["https://res1.example.com", "https://res2.example.com"] }
```

An object groups servers by mirror; a string sets the default server; an array
sets the default servers. The older names `resource_servers`, `resource_server`
and `res_servers` are still read.

## A project's fields

| Field | Type | Meaning |
|---|---|---|
| `workspace` | `object` | the project's tools and versions (with per-platform values) |
| `projectScope` | `boolean` | `false`: no project mode, only a list of dependencies |
| `subos` | `string` | the named SubOS the project is bound to (`projectSubos` is the older name, still read) |
| `mirror` | `string` | overrides the home's |
| `lang` | `string` | overrides the home's |
| `uiMode` | `string` | overrides the home's |
| `theme` | `string` | overrides the home's; a relative path is relative to the **project root**, so a project can bring its colours |
| `tui` | `object` | overrides the home's |
| `index_repos` | `array` | the project's index repositories (the home's format) |
| `XLINGS_RES` | `object \| string \| array` | the project's resource servers (the home's format) |

### Per-platform versions in `workspace`

Each tool's value takes one of two forms:

```json
"workspace": {
  "gcc": "16.1.0",
  "clang": {
    "linux": "19.1.0",
    "macosx": "19.1.0",
    "windows": "18.1.8",
    "default": "19.1.0"
  }
}
```

- **simple**: `"tool": "version"` — the same version on every platform;
- **per platform**: an object keyed by platform (`linux`, `macosx`, `windows`),
  resolved for the current platform at run time, with an optional `default`.

## A scope's install record: `configured` (2026.9.29.1+)

A payload in `data/xpkgs/` is a fact about the home; having run its `config()` is a
fact about one **scope** (a SubOS, or a project). Each scope's `.xlings.json`
carries a top-level `configured`, beside `workspace` and never inside it (an
older client reads every `workspace` key as a target name):

```json
"configured": { "xim:gcc@16.1.0": 1 }
```

The value is the recipe's `revision` when `config()` last ran in this scope.
`install` leaves a present payload's configuration alone only when the record
names the recipe's current revision and every ledger entry the payload owns is
claimed by the scope. No record means "configure" (the safe direction).
`--reconfig` runs every `config()` again.

## A SubOS workspace's fields

| Field | Type | Meaning |
|---|---|---|
| `workspace` | `object` | what is installed and active (below) |
| `subos_info` | `object` | what the SubOS is: its runtime and environment declarations (below) |
| `configured` | `object` | the scope's install record (above) |
| `storage` | `string` | `"shared"`, `"tmpfs"`, `"image"` |
| `imageSize` | `string` | the disk image's size when `storage` is `image` (e.g. `"4G"`) |

### `workspace` entries (SubOS)

```json
"workspace": {
  "python": "3.12.0",
  "gcc": {
    "active": "16.1.0",
    "installed": ["16.1.0", "15.1.0"]
  }
}
```

- **a string** (older): the active version only, no installed list;
- **an object** (0.4.19+): `active` is the version in use, `installed` the
  installed versions;
- **per platform**: the project syntax, read for compatibility.

An object with an `active` or `installed` key is the object form; otherwise it
is the per-platform form.

### `subos_info` (2026.8.5+)

`workspace` records what a SubOS **has installed**; `subos_info` records what it
**is** — the runtime its binaries are built for, and the environment a process
entering it needs.

```json
"subos_info": {
  "schema_version": 1,
  "runtime": "glibc@2.44",
  "runtime_source": "index",
  "host_glibc": "2.39",
  "envs": {
    "compat.mesa@25.0.0": [
      { "var": "LIBGL_DRIVERS_PATH", "op": "set",     "value": "${pkgdir}/lib/dri" },
      { "var": "XDG_DATA_DIRS",      "op": "prepend", "value": "${pkgdir}/share" }
    ]
  },
  "created_at": "2026-08-05T14:23:11Z",
  "created_by": "xlings 2026.8.9.1"
}
```

| Field | Type | Meaning |
|---|---|---|
| `schema_version` | `integer` | `1` |
| `runtime` | `string` | the runtime binding `<name>@<version>`, self-describing (`glibc@2.44` means Linux/glibc). **May be absent** (2026.8.17.1+): "a known unknown", below |
| `runtime_source` | `string` | (2026.8.27.2+, optional) **how** that binding was decided: `explicit` / `index` / `fallback`. Absent means the xlings that wrote the block predates the field, not a fourth kind of source |
| `runtime_abi` | `string` | (2026.8.27.5+, optional) the ABI the runtime package **itself declares** (`linux-x86_64-glibc`), from its recipe's `exports.runtime.abi`. Absent: an older block, or a runtime with no recipe to ask (hosted, below). Readers fall back to deriving it from the name — and know that **derived and declared are two facts** |
| `host_glibc` | `string` | (2026.8.9+, optional) the host glibc version measured when the block was written (e.g. `"2.39"`). Absent means unknown (an old manifest, a non-glibc host, a failed probe); readers treat unknown as "cannot prove", never compare it as 0 |
| `envs` | `object` | keyed by **the declaring package's binding**; removing the package removes its whole section, with no clean-up code in the recipe |
| `created_at` / `created_by` | `string` | when and by which version the SubOS was **created**; written only by the run that created it |
| `described_at` / `described_by` | `string` | (2026.8.17.1+) when and by which version an existing SubOS was **described** afterwards. Either this pair or `created_*`; only both missing is a defect |

#### `runtime` absent: a known unknown (2026.8.17.1+)

Three states, which readers tell apart:

| On disk | Meaning |
|---|---|
| no `subos_info` block | the SubOS predates `subos_info` |
| a block **without a `runtime` key** | **looked, and cannot tell** — no record or payload could answer |
| a block with a well-formed `runtime` | the declaration holds |

When present, `runtime` is always `<name>@<version>`; an empty string is not a
valid spelling (absence is).

Before 2026.8.17.1 the invariant demanded a well-formed `runtime`, so a
description without evidence had two choices: invent one, or produce a block
`--fix` would rewrite forever. It invented one — measured, two SubOSes on a real
machine declared `glibc@2.44` while their `lib/libc.so.6` pointed at a 2.39
payload (openxlings/xlings#547). **A constant and a real answer look identical on
disk**; this state exists to make them differ.

A reader meeting the absence **degrades and says why**, rather than choosing a
default for it.

#### Where a binding came from: `runtime_source` (2026.8.27.2+)

`runtime` says **what** the binding is, not **how** it was decided. Both look the
same on disk with very different trust:

| Value | Meaning | Who |
|---|---|---|
| `explicit` | a person chose it (`subos new --runtime ...`) | the user |
| `index` | the index was **asked** at creation | resolution |
| `fallback` | the index could not answer; the compiled-in `DEFAULT_RUNTIME_FALLBACK` was used | a guess |
| absent | the xlings that wrote the block predates the field | — |

The fallback constant and the index's answer are **the same value** for long
stretches, so "the mechanism works" and "the mechanism is dead but the constant
happens to be right" are byte-identical in a manifest. This field has twice been
the only evidence: once a correct binding whose resolution never happened
(`fallback`), once a wrong version whose error named only the payload directory.

> General rule: **when adding a field two sources can produce in the same shape,
> record which one did.**

`fallback` is not an error — an offline machine or a cold CI start legitimately
lands there, behaving exactly as before the field existed. It says only that the
value was not checked.

#### Created versus described

`created_*` and `described_*` record different things; which pair a run writes
depends on what it is doing:

- **creating** (`xlings subos new`, `self init` on a fresh home) → `created_*`;
- **describing** (`self doctor --fix`, `xlings install` filling a block in, an
  upgrade path) → `described_*`.

Rewriting an existing block **carries the real `created_*` over**: a description
may add `described_*` but never erases who created the SubOS and when.

**`runtime` is a creation-time property; a new default affects only new
SubOSes.** The binding persists in each SubOS's own manifest; repair paths
(`self doctor --fix`, rewriting the block) **keep a valid recorded binding**.
(2026.8.17.1+) A missing or malformed binding no longer falls back to the
default; xlings asks in turn: the runtime active in the SubOS's workspace → the
payload `lib*/libc.so.6` links to (the only **observed**, not recorded, source) →
when neither answers, **the key is not written**. Dependency resolution keeps
existing SubOSes in place by pin-to-active: an active glibc that satisfies a
constraint beats the index's newest, so neither a new default nor the index's
`latest` pulls an existing SubOS onto a new runtime.

**At creation the default comes from the index, not a constant (2026.8.27.2+).**
Creating a SubOS asks the index for the runtime package's current version and
records `runtime_source: index`; only when it cannot answer does it fall to
`DEFAULT_RUNTIME_FALLBACK` and record `fallback`.

**(2026.8.27.4+) `self init` makes the index usable before deciding.** The
`subos/default` it creates is often born on a machine whose index was never
synchronised — every new user's first step. Reading only the disk then could not
answer, and the constant would become what new users actually get. So this path,
and only this one, may synchronise the index once before deciding:

| Case | Behaviour |
|---|---|
| an index on disk (new or old) | used as is, **no network** |
| no index, network available | synchronised once, then resolved |
| no index, offline or the sync fails | the constant, recorded as `fallback` — **as before** |

The other creation paths (`subos new`, a fork, a rebuild) read only the local
index: they run often, and the index is usually there by then. **Only where an
answer is about to be written permanently, with no earlier chance to fetch the
index, is a network round trip worth it.**

#### A declaration beats the index: resolution order (2026.8.27.5+)

**A SubOS declares exactly one runtime; everything installed into it links
against it.**

Dependency resolution takes a runtime package's version from three levels:

| Level | Source | Note |
|---|---|---|
| 1 | **the SubOS's declared `runtime`** | strongest: everything in the SubOS links against it |
| 2 | the package's version active in this SubOS | pin-to-active |
| 3 | the index | `select_best` |

**Why level 1 is needed.** On a fresh home nothing is active, level 2 has nothing
to say, and the first dependency written `glibc@>=2.39` resolves through
`select_best` — which takes **the largest satisfying version, ignoring
`latest`**. The SubOS would declare one libc and run another.

**`latest` and "the largest entry in the table" are two questions about the same
table**, which only look alike while nothing holds a version down. The index has
one now: `musl`'s `latest` is `1.2.5`, and the table holds `1.2.6`.

Degradation (missing any of these bricks an older SubOS):

| Case | Behaviour |
|---|---|
| the declared version is not in the index (withdrawn, offline) | levels 2 and 3, **never a hard failure** |
| the declaration conflicts with the active version | the declaration wins, **out loud** — someone installed another libc here |
| a hosted runtime | not pinned, below |

#### Vendored and hosted: two kinds of runtime, two invariants

**A SubOS's runtime is not always glibc, and not always a package.**

| | Examples | Payload | Invariant |
|---|---|---|---|
| **vendored** | `glibc`, `musl`, `wasi-libc` | in the store | installed and pinned |
| **hosted** | `ucrt`, `macos_sdk` | **none** | present and compatible; **never installed, never pinned** |

For a hosted runtime, "the declared runtime must be installed" is **false** — a
checker that does not know the difference reports a healthy Windows SubOS as
broken.

#### Rebinding is an operation, never a side effect (2026.8.27.5+)

```bash
xlings subos runtime <package@version> [name]
```

**An index update never changes an existing SubOS's binding.** Swapping libc
underneath a SubOS is exactly the danger of an INTERP and a RUNPATH from two
different runtimes.

The corollary: **a patched runtime reaches new SubOSes only**; existing ones
migrate when someone asks. Rebinding says on the spot that programs already
built still point at the old runtime and follow only when rebuilt.

#### `envs`

A package writes `envs` in `config()` through `subos.env{}` (xim-pkgindex's
xpackage V2). **Values use placeholders** — `${pkgdir}`, `${subosdir}`, `${home}`,
`${xlings_home}`; an absolute path would make the description true only on the
machine that wrote it. Placeholders expand on entry.

An empty `envs` is kept as `{}`, never omitted: absent and empty would be two
spellings of one meaning, and every reader would handle both.

On entry (`xlings subos use`) the variables are set in the process; **a value the
user exported wins**: `set` does not override it, `prepend` still joins it.
`xlings self doctor` checks the section's integrity, its agreement with the
installed packages, that the placeholders resolve, and whether several packages
compete for one variable.

#### What `op` means exactly

Two ops. **`set` is conditional — it does not "set":**

| `op` | Meaning |
|---|---|
| `set` | exported **only when the variable does not exist**. An existing variable (even empty) is kept — the user's value wins. **Not an override** |
| `prepend` | prepended to the existing value with the platform's path separator; when the variable does not exist, a plain assignment |

The name misled people: an op called `set` that does nothing when the variable
exists sent both a report and an issue in the wrong direction (an issue asked for
"a conditional op" that had always been there). Renaming it would make every
recipe using the new name fail to install on every released older client (an
unknown op is `EnvDeclMalformed` at install time), so the name stays and the
meaning is written here.

**"Does not exist" versus "is empty": empty counts as set, and the user's value
wins.** `export FOO=` is a choice. The four back ends agree:

| Back end | Generated / executed |
|---|---|
| POSIX | `: "${FOO=value}"; export FOO;` — **not `:=`**, which overrides an empty value too |
| fish | `if not set -q FOO; set -gx FOO …; end` |
| pwsh | `if ($null -eq $env:FOO) { … }` — **not `-not`**, which is true for an empty string |
| in process | `if (!utils::env_is_set(var))` — **not `existing.empty()`**, which cannot tell unset from empty |

Three of the four back ends used to get this backwards: only fish kept an empty
value. One declaration gave different environments under different shells, and
silently.

## Complete examples

### The home (`~/.xlings/.xlings.json`)

```json
{
  "mirror": "CN",
  "lang": "zh",
  "activeSubos": "dev",
  "version": "2026.7.29.1",
  "versions": {
    "python": {
      "type": "program",
      "versions": {
        "3.12.0": { "path": "/home/user/.xlings/data/xpkgs/xim-x-python/3.12.0/bin" }
      }
    }
  },
  "index_repos": [
    { "name": "ros2", "url": "https://github.com/example/ros2-pkgindex" }
  ],
  "XLINGS_RES": {
    "cn": "https://cn-res.example.com"
  }
}
```

### A project (`<project>/.xlings.json`)

```json
{
  "workspace": {
    "gcc": { "linux": "16.1.0", "macosx": "15.1.0" },
    "python": "3.12.0",
    "cmake": "3.30.0"
  },
  "subos": "myproject-env"
}
```

### A SubOS workspace (`~/.xlings/subos/dev/.xlings.json`)

```json
{
  "storage": "image",
  "imageSize": "4G",
  "subos_info": {
    "schema_version": 1,
    "runtime": "glibc@2.39",
    "envs": {},
    "created_at": "2026-08-05T14:23:11Z",
    "created_by": "xlings 2026.8.5.1"
  },
  "configured": { "xim:gcc@16.1.0": 0 },
  "workspace": {
    "gcc": {
      "active": "16.1.0",
      "installed": ["16.1.0", "14.2.0"]
    },
    "python": {
      "active": "3.12.0",
      "installed": ["3.12.0", "3.11.9"]
    }
  }
}
```
