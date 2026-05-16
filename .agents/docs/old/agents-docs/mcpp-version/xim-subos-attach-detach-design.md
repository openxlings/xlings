# XIM Attach/Detach and Subos Reference Design

## Problem

The current C++ `xim` path handles package payload installation, but it does not
fully preserve the old Lua semantics around subos mapping:

1. If a payload is already present in the store, `install` skips the package
   `install` hook, which is correct.
2. But the current implementation also skips the package `config` hook, so the
   package is not re-attached to the current subos/workspace.
3. `remove` does not yet implement "detach current subos first, delete payload
   only when no other subos still references it".

This causes visible regressions:

- `xlings install foo@ver` in a second subos can print `all packages already installed`
  without mapping `foo@ver` into that subos.
- `remove` is not yet aligned with the old Lua reference-based payload lifecycle.

## Existing Model

There are two distinct layers:

1. Payload layer
   A real installed package payload exists in a store directory, for example:
   - global: `<XLINGS_HOME>/data/xpkgs/...`
   - project: `<project>/.xlings/data/xpkgs/...`

2. Subos/workspace layer
   A subos chooses which version is active through its workspace file:
   - global subos: `<XLINGS_HOME>/subos/<name>/.xlings.json`
   - project anonymous: `<project>/.xlings.json`
   - project named subos: `<project>/.xlings/subos/<name>/.xlings.json`

Package hooks already match this layering:

- `install` hook: materialize payload
- `config` hook: register/map into xvm/subos
- `uninstall` hook: detach/remove xvm-side registration

## Target Semantics

### Install

`xlings install foo@ver` should mean:

1. Ensure payload exists.
   - If missing: run `install`
   - If already present: skip `install`
2. Attach the version to the current subos.
   - Always run `config`
   - Then ensure the requested target version is active in the current subos

The second step must happen even when the payload already exists.

### Remove

`xlings remove foo` should mean:

1. Detach `foo@current-version` from the current subos
2. Check whether the same payload version is still referenced by any other
   subos/workspace in the same scope
3. Only when the answer is "no":
   - run `uninstall`
   - remove payload directory
   - clear installed status for that package/version

If another subos still references the same version, the command must stop after
detach and keep the payload intact.

## Scope Rules

Reference scanning must respect package scope:

- global package: scan `<XLINGS_HOME>/subos/*/.xlings.json`
- project package: scan current project workspace files only
  - `<project>/.xlings.json`
  - `<project>/.xlings/subos/*/.xlings.json`

Project-local packages must not be scanned across unrelated projects.

## Subos Initialization

`xlings subos new <name>` should keep creating the minimal template:

- `bin/`
- `lib/`
- `usr/`
- `generations/`
- `.xlings.json` with:

```json
{
  "workspace": {}
}
```

This is already correct and remains the baseline for attach/detach logic.

## Implementation Plan

### 1. Install path

Refactor installer execution into:

- ensure payload
- run `config`
- process xvm operations

For already-installed payloads:

- skip `install`
- still run `config`

### 2. Explicit target activation

For explicit CLI targets, after `config` has registered the version, activate the
requested version in the current subos via `xvm use` semantics.

Dependencies should not be auto-activated unless the package hook explicitly does so.

### 3. Detach and reference scan

Implement:

- current workspace config path resolution
- workspace reference scan by `target + version`
- detach current subos mapping
- conditional payload removal when reference count reaches zero

### 4. Tests

Add isolated-home tests for:

1. create subos template contents
2. install package in `s1`
3. install same package in `s2`
   - payload reused
   - current subos mapping created
4. remove package from `s1`
   - payload remains because `s2` still references it
5. remove package from `s2`
   - payload is deleted when last reference disappears

All E2E runs must use an isolated `XLINGS_HOME`, never the machine's real home.

