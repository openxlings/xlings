# Unified XLINGS_HOME Bootstrap Design

## Goal

Make release packages and installed homes look like the same thing:

- a portable extracted package is an `XLINGS_HOME`
- an installed `~/.xlings` directory is also an `XLINGS_HOME`
- both use the same top-level structure

The release package should become a minimal bootstrap home instead of shipping a
fully populated runtime tree.

## Unified Model

`XLINGS_HOME` always means the directory that contains:

```text
<XLINGS_HOME>/
  .xlings.json
  bin/xlings
  data/
  subos/
  config/
```

Two deployment modes use the same model:

1. Portable mode
   - `XLINGS_HOME = <extracted-release-dir>`
   - user runs `<dir>/bin/xlings`
   - the directory may be moved freely
2. Installed mode
   - `XLINGS_HOME = ~/.xlings`
   - user installs from a bootstrap package with `xlings self install`

The important rule is that the home root itself is stable. Portable mode must
not introduce an extra nested `./.xlings/` runtime root.

## Bootstrap Release Shape

The release archive should initially contain the minimum bootstrap files:

```text
<XLINGS_HOME>/
  .xlings.json
  bin/xlings
```

After `xlings self init`, the runtime skeleton is created in place:

```text
<XLINGS_HOME>/
  .xlings.json
  bin/xlings
  data/
    xpkgs/
    runtimedir/
    xim-index-repos/
    local-indexrepo/
  subos/
    current -> default
    default/
      bin/
      lib/
      usr/
      generations/
      .xlings.json
  config/
    shell/
```

This keeps the user-facing layout uniform:

- extracted portable package: same root structure
- installed system home: same root structure

## Detection Rules

`Config` should resolve `XLINGS_HOME` with this order:

1. `XLINGS_HOME` environment variable
2. executable parent root when:
   - `<exe>/../.xlings.json` exists
   - `<exe>/../bin/xlings` exists
3. fallback `~/.xlings`

This replaces the older self-contained detection that depended on `xim/` and
`xmake.lua` being present in the package root.

## Runtime Initialization

`xlings self init` becomes the canonical way to materialize the bootstrap home.

It must create:

- `data/xpkgs`
- `data/runtimedir`
- `data/xim-index-repos`
- `data/local-indexrepo`
- `subos/default/bin`
- `subos/default/lib`
- `subos/default/usr`
- `subos/default/generations`
- `subos/current -> default`
- `subos/default/.xlings.json`
- `config/shell/*`

It must also create builtin shims in `subos/default/bin` from `bin/xlings`.
On macOS these shims should be symlinks so the packaged `bin/xlings` remains
the only Mach-O binary in the bootstrap package.

## `self install` Semantics

`xlings self install` should keep its current role:

- source: the current bootstrap home
- target: the installed home, usually `~/.xlings`

Behavior:

1. detect bootstrap source by `.xlings.json` + `bin/xlings`
2. copy bootstrap files to target
3. run the same home-layout initialization used by `self init`
4. install shell profile hooks against the target home

This makes a minimal release package installable without requiring it to ship
prebuilt `data/`, `subos/`, or `config/` trees.

## Resource Ownership

For this bootstrap design, required runtime templates must no longer depend on
files being present in the release archive.

The C++ code should generate:

- shell profile templates under `config/shell/`
- default subos workspace file
- index-repo placeholder file when needed

This allows the release package to stay minimal while keeping installed and
portable homes consistent.

## Scope

Included in this change:

- new home detection markers
- bootstrap home initialization
- minimal release package layout
- `self install` compatibility with the minimal package
- CI/workflow updates for the new package shape

Explicitly out of scope:

- further shrinking to a single-file binary without `.xlings.json`
- redesigning project-local `<project>/.xlings` behavior
- removing legacy commands unrelated to home bootstrapping

## Acceptance Criteria

1. An extracted release directory containing the bootstrap files
   (`.xlings.json` and `bin/xlings`) can run `./bin/xlings self init`
   successfully.
2. After `self init`, `xlings config` reports the extracted directory as
   `XLINGS_HOME`.
3. `xlings self install` can install that bootstrap package to `~/.xlings`.
4. The installed `~/.xlings` and the portable extracted directory both end up
   using the same top-level directory structure.
5. Release scripts and CI workflows validate the bootstrap package rather than a
   pre-expanded runtime tree.
