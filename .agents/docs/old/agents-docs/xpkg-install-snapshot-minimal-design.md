# xpkg Install Snapshot Minimal Design

## Background

`xlings remove` currently loads the package description from the latest local
package index. If the index is updated after a package was installed, the
uninstall hook can change even though the installed payload still belongs to the
older package description.

This document keeps the fix intentionally small:

- after install, save the package description snapshot to `install_dir/.xpkg.lua`
- during uninstall, prefer `install_dir/.xpkg.lua`
- if the snapshot does not exist, keep the existing fallback to the current index

## Goals

1. Keep installed package lifecycle behavior stable after package index updates.
2. Avoid branch-per-version or package-index history lookup logic.
3. Keep the runtime compatibility model unchanged for now.
4. Preserve existing uninstall behavior for packages installed before this change.

## Non-Goals

1. Do not solve old `xlings/libxpkg` parsing new xpkg files during install.
   If an old runtime cannot parse a new package description, it should report the
   incompatibility and ask the user to upgrade.
2. Do not introduce `metadata.json`, install-plan snapshots, schema migration, or
   versioned adapters in this change.
3. Do not guarantee uninstall when both the snapshot is invalid and the current
   index no longer supports the package. Snapshot existence means the snapshot is
   the intended source of uninstall semantics.

## Design

### Install

When a package reaches the successful install path, copy the xpkg file used by
the resolver into:

```text
<install_dir>/.xpkg.lua
```

The copy uses overwrite semantics so reinstalling or remapping an existing
package refreshes the snapshot from the resolved xpkg.

Snapshot save is part of the install success path. If the copy fails, the install
operation should fail before the package is marked installed. This avoids a state
where xlings reports a successful install but cannot later use the expected
snapshot for removal.

### Uninstall

Before creating the xpkg executor for uninstall:

1. compute `snapshot = <install_dir>/.xpkg.lua`
2. if `snapshot` exists, create the executor from `snapshot`
3. if `snapshot` does not exist, create the executor from the current index xpkg

The fallback is only for missing snapshots, which preserves compatibility with
packages installed before this feature. If a snapshot exists but cannot be parsed
or its uninstall hook fails, the uninstall should surface that error rather than
silently falling back to a different package description.

### Scope

This feature intentionally snapshots only the package description file itself.
That means the xpkg should be self-contained for uninstall hooks. Hooks that need
external files from the package index remain dependent on the index layout, which
is acceptable for this minimal change. The executor input is switched to the
snapshot, while the existing package/index execution context remains unchanged.

## Implementation Plan

1. Add an e2e regression test that installs a fixture package, verifies
   `install_dir/.xpkg.lua` exists, mutates the package index uninstall hook, then
   verifies `xlings remove` still runs the snapshotted uninstall hook.
2. Add a small helper in `src/core/xim/installer.cppm` to compute and save
   `.xpkg.lua`.
3. Call the helper on the install success path before `mark_installed`.
4. In uninstall, select `install_dir/.xpkg.lua` as the executor input when it
   exists; otherwise keep the current index path.
5. Run the targeted e2e test and a focused existing remove/install regression.
