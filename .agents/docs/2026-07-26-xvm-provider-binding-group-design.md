# XVM Provider-Scoped Binding Group Design

> Date: 2026-07-26
> Status: approved for implementation
> Target release: xlings 0.4.70
> Related repositories: `openxlings/libxpkg`, `openxlings/xim-pkgindex`

## TL;DR

`xlings use` must switch a package release's binding group, not one flat
`target -> version` entry at a time.

The existing graph already intends to provide this behavior, but it has no
release ownership, permits dangling edges, removes versionless sibling targets
wholesale, and applies libraries/headers outside the graph transaction. The
result is the observed state where `gcc` has four versions, `g++` has only one,
and an attempted switch can write a nonexistent `g++` version into the active
workspace.

The new contract is:

1. every registered target version records its canonical package provider and
   release;
2. every explicit binding component has a stable provider-scoped group id;
3. `use` resolves and validates the complete group before any mutation;
4. program workspace entries, libraries, headers, and shims are applied from
   the same immutable switch plan;
5. uninstall removes exact provider-owned versions and reciprocal edges;
6. legacy graphs remain readable, but invalid legacy state fails closed and is
   repaired by re-running the owning package's config/install path in an
   isolated or user-selected home.

This preserves multiple providers for the same target (`gcc`, `cc`, `npm`,
`libatomic.so.1`) and member-specific version keys (`15.1.0`,
`gcc-15.1.0`, `node-22.17.1`) without inferring ownership from names.

## 1. User-visible requirements

### 1.1 Required behavior

- `xlings use gcc 15` switches every member of the selected GCC binding group,
  including `g++`, `c++`, `cc`, GCC tools, and registered libraries.
- `xlings use g++ 15` performs the same switch in reverse: any member is a
  valid group entry point.
- `xlings use g++` lists every installed `g++` version in the current SubOS,
  not only the version most recently re-registered.
- A library target can be used as an entry point when it is registered in a
  group; its real SubOS library view changes together with program members.
- Switching in one SubOS does not alter another SubOS's workspace or materialized
  library/header view.
- A corrupt or ambiguous group never causes a partial switch.

### 1.2 Compatibility requirements

- Existing valid `VInfo.bindings` JSON remains readable and switchable.
- Old xlings binaries can still read the primary `versions` data written by the
  new release; the legacy bidirectional edge representation remains serialized
  during the transition.
- Namespaced version keys continue to work.
- A group may map different target version strings for one package release.
- One package may expose multiple independent binding groups.
- One target name may be supplied by multiple packages, provided the concrete
  target version keys do not collide.

### 1.3 Safety requirements

- Tests and live verification must use temporary `XLINGS_HOME`, `HOME`, and
  SubOS roots. They must never execute install/remove/use against the host
  xlings home.
- Versionless sibling removal must never erase all versions.
- A failed plan must leave workspace JSON, shims, libraries, and headers
  unchanged.
- Config JSON writes use same-directory staging plus atomic replacement.

## 2. Confirmed failure mechanism

The current database has two independent state layers:

```text
global VersionDB:
  target -> VInfo {
    versions[targetVersion] -> VData
    bindings[peer][thisVersion] -> peerVersion
  }

per-SubOS workspace:
  target -> { active, installed[] }
```

`add_version()` creates both sides of a binding edge, but:

- it creates a missing peer through `db[peer]`, producing placeholder nodes;
- it does not require the peer version to exist;
- `remove_version()` removes only `VData`, never reciprocal edges;
- uninstall falls back to the outer package version only when
  `op.name == detachTarget`;
- a versionless sibling op executes `db.erase(op.name)` and returns before
  `installed[]` cleanup.

GCC's recipe emits versionless removes for ordinary programs, the virtual root,
and `cc`. Removing one GCC release can therefore erase every `g++` and root
version, while the requested `gcc` release alone is removed exactly. Reinstalling
16 reconstructs only the 16 component. Stale edges and `installed[]` entries
remain.

The current `cmd_use()` then:

1. changes headers/libs for the starting target only;
2. recursively follows whatever edges remain;
3. does not verify that a reached `(target, version)` has `VData`;
4. writes every reached pair to workspace.

This explains both observed modes:

- `gcc@15 -> root@15` stops at the damaged root, so `g++` remains 16;
- another flavor reaches `g++@15-musl` through a stale edge, writes it active,
  and the shim later fails because only `g++@16` exists.

## 3. Ecosystem constraints discovered by audit

The official index currently contains 37 recipes with explicit bindings.

- There are no real multi-level trees; valid components are stars.
- Some packages expose multiple stars.
- Member versions often differ from root versions:
  `gcc-<v>`, `glibc-<v>`, `node-<v>`, `openssl-<v>`,
  `python-<v>`, and flavor-suffixed GCC versions.
- The same alias is intentionally supplied by multiple providers:
  GCC/LLVM/musl/mingw for compiler names, binutils/LLVM for tools,
  Node/standalone npm for `npm`, and multiple packages for shared libraries.
- Current recipe defects include:
  - e2fsprogs root registered after members;
  - syslinux self-binding;
  - musl and aarch64 flavor roots registered under a different version than
    their members reference;
  - openssl registering the same exact target/version twice;
  - several declared sibling executables left outside the intended group.

Therefore neither `target` nor a version naming convention is a safe owner
identity. Registration must receive provider identity from the resolved
`PlanNode`.

## 4. Alternatives considered

### A. Patch traversal and versionless removal only

Add missing edge cleanup, infer sibling versions, and validate `cmd_use`.

This is necessary compatibility work but insufficient architecture: ownership
remains implicit, alias collisions are ambiguous, and another malformed recipe
can recreate the same class of corruption.

### B. Provider-scoped binding groups with legacy graph compatibility

Add provider/group metadata to each `VData`, build a production switch plan,
retain validated legacy bindings for downgrade and migration, and make removal
provider-aware.

This is the selected design. It fixes the present failure without replacing
shim dispatch or the entire VersionDB and provides a migration path for existing
homes.

### C. Replace VersionDB with a separate normalized bundle database

Store packages, releases, members, providers, and active selections in new
tables and migrate all consumers at once.

This is theoretically cleaner but unnecessarily risks shim dispatch,
project-mode overrides, SubOS ref-counting, and cross-platform release paths.
Option B establishes the same identity and transaction boundaries incrementally;
a future normalized store can consume those explicit identities.

## 5. Data model

### 5.1 Provider and group identity

`VData` gains optional, backward-compatible group metadata:

```cpp
struct BindingGroupRef {
    std::string provider;         // canonical package name, including namespace
    std::string providerVersion;  // resolved package release version
    std::string group;            // stable label inside that provider release
    std::string rootTarget;        // exact manifest-bearing target
    std::string rootVersion;       // exact manifest-bearing target version
};

struct VData {
    // Existing per-version fields: path, alias, envs, includedir/libdir.
    std::string kind;             // program | lib | group
    std::string sourceName;       // payload entry; empty for group
    std::string destinationName;  // shim/sysroot name
    std::optional<BindingGroupRef> bindingGroup;
    // Populated only on bindingGroup.rootTarget@rootVersion.
    std::map<std::string, std::string> bindingMembers;
    std::vector<HeaderAsset> bindingHeaders; // populated on the exact root
};
```

Every member stores the same `BindingGroupRef`. The exact root additionally
stores the complete member manifest. This is intentionally more than an owner
tag: scanning only surviving `VData` cannot detect that a member was deleted,
whereas a manifest makes both a missing root and a missing member observable.
`kind`, source, and destination are deliberately version-scoped. The legacy
`VInfo.type` and `VInfo.filename` fields remain read fallbacks only: target-level
metadata cannot represent two providers whose same target has different kinds
or filenames.

Root JSON shape:

```json
{
  "path": ".../gcc/15.1.0",
  "kind": "group",
  "bindingGroup": {
    "provider": "xim:gcc",
    "version": "15.1.0",
    "group": "xim-gnu-gcc",
    "rootTarget": "xim-gnu-gcc",
    "rootVersion": "15.1.0"
  },
  "bindingMembers": {
    "xim-gnu-gcc": "15.1.0",
    "gcc": "15.1.0",
    "g++": "15.1.0",
    "gcc-ar": "gcc-15.1.0",
    "libstdc++.so.6": "gcc-15.1.0"
  }
}
```

The identity tuple is:

```text
(provider, providerVersion, group)
```

It is independent of:

- target name (`gcc`, `g++`, `libstdc++.so.6`);
- member version key (`15.1.0`, `gcc-15.1.0`);
- package-index alias;
- `VData.alias`, which remains an execution command/argument mapping.

### 5.2 Group assignment

Registration processes one config hook's XVM operations as a batch:

1. materialize every add node without edges;
2. reject duplicate exact `(target, version, provider)` writes;
3. build components from structured/legacy binding references;
4. validate every referenced exact root after all nodes exist;
5. assign each component a stable group label:
   - explicit `group` from a future/new libxpkg operation wins;
   - otherwise the component's binding-root target is used;
   - an unbound node forms a single-node group named after its target;
6. choose one exact root, write its full member manifest, write the same root
   reference to every member, and retain compatible bidirectional bindings.

Batch processing also defines safe reconfiguration:

- an exact registration owned by the same provider release is an idempotent
  upsert;
- an owner-less legacy exact node can be adopted only when its payload metadata
  matches the current package registration;
- an exact node owned by another provider is a conflict and fails closed;
- old edges/manifests are replaced only for exact nodes in the validated batch,
  never by target-wide erase.

Batching removes the current order dependency while still rejecting phantom
roots, self-bindings, conflicting target versions in one group, and
cross-provider binding references.

### 5.3 Canonical group manifest

The canonical member set is the exact root's persisted `bindingMembers` map.
Resolution validates:

- the referenced root VData exists;
- the root reference points to itself;
- root provider/version/group matches the member reference;
- every manifest member VData exists and points back to the same root;
- the starting member occurs in the manifest under its requested version;
- no target is assigned more than one version.

The serialized `VInfo.bindings` map is compatibility metadata, not the canonical
source for provider-aware entries. A missing member is therefore a hard
integrity error rather than a silently smaller group.

For legacy entries without group metadata, the resolver uses a strictly
validated binding graph.

### 5.4 Header assets and materialized-view ownership

Headers are group assets, not phantom target versions. A `HeaderAsset` records
its source directory and optional destination prefix in the root manifest. The
cross-repository XVM operation carries a group label; when omitted it is
accepted only if the config batch has one unambiguous group. No `headers`
operation may create `db[node.name].versions[...]` implicitly.

Each SubOS config persists a materialized-view ownership ledger beside
`workspace`:

```text
materialized[path] = {
    bindingGroupRef,
    memberTarget,
    memberVersion,
    source
}
```

It covers individual library and header destinations. Ownership must never be
inferred from symlink targets because Windows may use hardlinks or copies. The
ledger makes replacement, conflict detection, rollback, and later removal
deterministic even when different providers expose the same filename.

## 6. Switch architecture

### 6.1 Pure planning

A new production helper returns either a complete plan or structured errors:

```cpp
struct BindingSelection {
    std::map<std::string, std::string> members;
    BindingSource source; // ProviderGroup | LegacyGraph
};

std::expected<BindingSelection, BindingError>
resolve_binding_selection(const VersionDB&, target, version);
```

Provider-group resolution:

- reads the start `VData.bindingGroup`;
- loads the exact root's persisted member manifest;
- validates every member and its back-reference;
- verifies one version per target and supported target types.

Legacy resolution:

- traverses by `(target, version)`, not target alone;
- verifies source and destination VData;
- verifies the reciprocal edge;
- rejects a cycle only when it produces a conflicting target version;
- rejects asymmetric, dangling, or conflicting paths.

No filesystem or Config mutation occurs during planning.

### 6.2 Preflight and apply

`cmd_use()` becomes:

```text
acquire per-home/SubOS XVM state lock
    -> reload versions/workspace/materialized ledger while holding the lock
resolve requested member version
    -> build BindingSelection
    -> preflight every program/lib/header source
    -> stage every materialized-view and shim change beside its destination
    -> update an in-memory workspace copy for every member
    -> replace staged filesystem destinations while retaining backups
    -> atomically persist workspace + materialized ledger once (commit point)
    -> delete backups and release the lock
```

Every fallible filesystem creation occurs before the commit point. A failed
destination replacement or JSON commit restores filesystem backups and leaves
the old workspace/ledger. Fault-injection tests cover each stage. Generic shims
are staged exactly like other destinations; they are not a fallible
post-commit action.

The per-home/SubOS lock is shared by `use`, registration, and removal. State is
reloaded after lock acquisition so two processes cannot overwrite changes
loaded before either acquired the lock. Workspace JSON is written through a
same-directory temporary file and atomic rename/replace.

Full crash-atomicity across JSON plus many independent header/library paths
would require a generation-based sysroot; that is outside this release. A crash
journal/generation design remains a possible follow-up. The current release
does not claim stronger durability than it implements.

### 6.3 Programs and shims

Program shims are version-independent xlings launchers. Installation creates
them; `use` only ensures missing shims exist after successful preflight. Version
selection is entirely the committed workspace map.

`VData.kind == "group"` is a selectable virtual root: it participates in list,
use, installed sets, manifests, and diagnostics, but has no executable payload
and never creates a shim. This replaces the current accidental behavior where a
default-program marker such as `xim-gnu-gcc` produces a bogus shim.

### 6.4 Libraries

For every selected `VData.kind == "lib"` member:

- source is `VData.path / VData.sourceName`;
- destination is the SubOS library directory plus
  `VData.destinationName`;
- the old destination is replaced only through its materialized-view ledger
  entry and is retained as a transaction backup;
- the new file is linked/copied through the existing cross-platform link
  abstraction;
- failures roll back before workspace commit.

Installing an additional version without `--use` must not silently repoint an
already-active library.

### 6.5 Headers

Header assets recorded in the root manifest are expanded into individual staged
destinations and recorded in the materialized-view ledger. They switch for the
whole selection, not only the CLI starting target. Existing recipe-managed
headers remain outside this ownership model until explicitly migrated and are
not claimed as transactionally switched.

### 6.6 Installed sets and listing

Every registered member type, including binding roots and libraries, is added to
the current SubOS `installed[]` set. Therefore:

- `xlings use <program>` lists all locally installed member versions;
- `xlings use <lib>` can list and select library versions;
- switching a globally available group into a fresh SubOS atomically adds every
  selected member to that SubOS's installed sets.

## 7. Removal architecture

### 7.1 Provider-aware removal

Removal is explicitly two-phase:

1. resolve every group owned by the requested provider release and atomically
   detach all members/assets from the current SubOS, applying a coherent
   surviving-group fallback where available;
2. scan every SubOS/project installed set for the exact owner tuples. If another
   environment still references the release, retain global VersionDB entries
   and payload. Otherwise run the uninstall hook and globally purge the exact
   owned registrations and payload.

Those exact target versions are authoritative. Recipe `xvm.remove` operations
are validated against them but do not define ownership.

Removal:

- deletes only exact owned versions;
- removes both sides of all binding edges for those exact versions;
- prunes empty binding maps and empty target entries;
- updates `installed[]`;
- removes a shim only when no usable program version remains;
- switches an active group to a complete surviving group when possible,
  otherwise clears all removed members coherently;
- rematerializes libraries/headers for the fallback group.

The reference check operates on every group member, not only the package/root
target. Removing a release from SubOS A must leave SubOS B's workspace,
materialized view, and global payload usable.

### 7.2 Legacy versionless operations

For old provider-less entries, uninstall snapshots the validated legacy
selection before deletion and maps each versionless sibling op through that
selection.

If no unique exact version can be derived, uninstall fails before mutation.
The old `db.erase(op.name)` fallback is removed. Deliberate all-version removal
must use an explicit API/operation.

`remove_all` is transported as a distinct `op = "remove_all"` operation. It is
scoped to target versions owned by the currently executing provider; it cannot
erase owner-less or other-provider entries. Provider-less legacy all-version
removal is rejected.

## 8. Legacy integrity and repair

### 8.1 Read compatibility

Entries without group metadata are legacy. Valid legacy groups continue to
switch through strict graph validation.

### 8.2 Invalid legacy state

If validation finds a dangling/asymmetric/conflicting graph:

- `use` exits nonzero;
- no workspace or filesystem state changes;
- the diagnostic names the exact invalid pair;
- it recommends re-running the owning package/version install/config when that
  owner can be inferred, otherwise reports the affected targets.

Re-running an already-installed package config performs the idempotent/adoption
rules in §5.2, writes group references/manifests, and atomically replaces only
the batch's compatible old edges without re-downloading a valid payload.

### 8.3 Doctor integration

`xlings self doctor` gains XVM checks for:

- active/installed versions missing from VersionDB;
- dangling/asymmetric legacy edges;
- provider group conflicts;
- missing program/library payloads.

`--fix` may repair mechanical metadata and generic shims, but must not execute
package hooks or download payloads. Missing registrations receive explicit
install/reconfigure guidance.

## 9. Cross-repository contract

### 9.1 libxpkg

The Lua operation API becomes symmetric:

- `xvm.add(name, opt)` and `xvm.remove(name, version)` both default to the
  current runtime package version;
- `xvm.remove_all(name)` is the only explicit all-version operation;
- `xvm.setup()` propagates its resolved version to root, programs, and libs;
- `xvm.teardown()` removes the same exact version;
- a structured optional group label can be transported without parsing target
  ownership from strings;
- header operations carry group identity instead of relying on the package name.

The current official index does not call `setup/teardown`, so direct operation
tests are required.

The libxpkg change is released first. The xlings integration then updates both
`mcpp.toml` and `mcpp.lock` to that exact patch release; CI must not silently
continue using 0.0.46.

### 9.2 xim-pkgindex

The GCC-family and audited malformed recipes are corrected:

- exact add/remove root versions;
- no self-binding;
- no phantom root;
- no duplicate exact registration;
- intended siblings included in their group;
- explicit versions on teardown.

These recipe changes remain compatible with the previous xlings release, so
they can merge before the stricter validator.

## 10. Test strategy

### 10.1 Unit tests

- group reference, exact-root manifest, per-version kind/source/destination,
  header-asset, and materialized-ledger JSON round trips;
- legacy JSON compatibility and `VInfo` materialization fallback;
- provider-group planning from root and leaf;
- different root/member versions;
- multiple groups per provider, multiple providers per target, and same target
  with provider-specific kind/source metadata;
- namespace handling;
- virtual group roots never create program shims;
- valid legacy cycles;
- missing VData, asymmetric edge, conflicting target version;
- idempotent same-owner reconfiguration, compatible owner-less adoption, and
  other-owner conflict;
- exact version removal and reciprocal edge cleanup;
- raw empty legacy remove normalization and explicit provider-scoped
  `remove_all`;
- no mutation on plan/preflight failure;
- library/header materialization, ownership collision, Windows copy/hardlink
  behavior, and rollback at every injected failure point;
- lock-then-reload behavior under concurrent stale readers;
- current-SubOS detach followed by all-SubOS reference accounting.

Tests call production helpers. The existing copied traversal lambda is removed.

### 10.2 Hermetic E2E

A two-version GCC-like fixture contains:

- package/root target;
- two executable members with real shims;
- a transformed member version;
- a library file;
- a header file;
- versionless legacy teardown operations.

Scenarios:

1. install v1 and v2;
2. list from a non-package executable;
3. switch root -> v1 and execute every shim;
4. switch leaf -> v2 and execute every shim;
5. switch from a library target;
6. verify workspace, installed sets, library, and header views;
7. corrupt one peer and prove the failed switch changes nothing;
8. remove non-active v1 and preserve v2;
9. remove active v2 and apply a coherent fallback;
10. run different selections in two SubOS instances, detach from one, and prove
    the other keeps its workspace, materialized view, and global payload;
11. reconfigure the same provider idempotently, adopt a compatible legacy
    entry, and reject a conflicting provider without mutation;
12. exercise explicit provider-scoped `remove_all` without touching another
    provider's same-named target.

Shell coverage runs on Linux and macOS. PowerShell coverage runs on Windows and
executes real `.exe` shims; file content is used where Windows symlink privilege
is unavailable.

### 10.3 Live isolated GCC verification

On Linux, use a temporary xlings home and the modified local package index:

```text
install gcc@15.1.0
install gcc@16.1.0
use gcc 15
execute gcc and g++
use g++ 16
execute gcc and g++
verify registered lib links
remove one version
repeat both directions
```

The host `.xlings.json`, host SubOS workspace, host executable/shim directories,
and relevant host package payload metadata are hashed before and after to prove
they were untouched.

## 11. Rollout and success gates

1. Merge compatible recipe corrections.
2. Land/release the libxpkg operation-contract fix.
3. Land xlings 0.4.70 with provider metadata, strict planner, materializer, and
   legacy fallback.
4. Keep writing legacy edges through at least the next minor compatibility
   window.
5. Consider removing legacy traversal only after field migration evidence.

Completion requires:

- xlings unit tests pass with GCC 16;
- hermetic Linux/macOS/Windows E2E passes;
- isolated real GCC 15/16 bidirectional switching passes;
- libxpkg and pkgindex tests pass;
- every PR's required CI checks are green;
- no host xlings state changes.
