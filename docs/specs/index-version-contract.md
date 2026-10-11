# Index Version Contract — for Publishers

> Status: in force since 2026.8.4.1. Reference tools: `tools/merge_index_pointer.py`,
> `tools/check_index_compat.py`.

An index snapshot may declare which client versions it needs. A client that
reads the declaration is **routed to the newest snapshot it can use**, instead of
failing.

This is the contract a **publisher** implements; implementing it needs no
reading of the xlings sources.

## 1. Why

Once an index uses something only newer clients have (a new xvm registration
node kind, a new `spec` version), older clients fail hard:

```
error: unsupported registration node kind 'files'
```

With the contract declared, an older client **falls back to the snapshot it can
still use**, and is told how to upgrade:

```
[index] xim: using 20e53c6 instead of e2aad0b
[index]   e2aad0b requires xlings >= 2026.8.4.1, this is 2026.8.2.1
[index]   upgrade to 2026.8.4.1: xlings self update
```

## 2. Declaring it in the index tree

The index tree's root holds `index-compat.json` (optional; absent means no
constraint):

```json
{
  "requires": {
    "xlings": { "min": "2026.8.4.1" }
  }
}
```

- `requires` maps **a consumer's name → its constraint**.
- `min` is **inclusive**, `max` **exclusive**: satisfied when
  `min <= client version < max`. Both are optional.
- xlings interprets only the `"xlings"` key; the others are **passed through
  unchanged** for their consumers to judge (read through
  `xlings index list --json`).
- Serialised, it is at most **4 KiB** — it goes into the pointer file every
  `xlings update` fetches.

**There are no range expressions**: no `^`, `~`, `||`. Two bounds are the range.
Misreading this contract makes a whole index unusable for a class of clients, so
the syntax is deliberately too poor to misread.

Versions compare component by component, numerically, with any number of
components — a four-part `2026.8.4.1` compares correctly, and `2026.x` is always
above `0.4.x`.

## 3. When to raise the floor

**When the index starts to depend on something only newer clients have.**

Raising the floor freezes older clients on an old snapshot, so it is **a safety
net, not the first choice**. First see whether a recipe-level capability probe
lets the single recipe degrade gracefully; raise the floor only when the index
as a whole depends on the new capability.

| | Granularity | For |
|---|---|---|
| a recipe capability probe | one recipe | a few recipes trying something new |
| the index version contract | a whole snapshot | the index as a whole depending on it |

## 4. The pointer file

A client reads a **pointer file** (`<name>-pointers.json`, a repository file a
git push may overwrite). Besides the current snapshot it holds an **addressable
history**:

```json
{
  "format_version": 2,
  "indexes": {
    "mcpplibs": {
      "format_version": 1,
      "index_version": "e2aad0b",
      "index_name": "mcpplibs",
      "generated_at": "2026-08-04T14:52:11Z",
      "artifact": { "name": "mcpp-index-e2aad0b.tar.gz", "sha256": "…", "size": 371707 },
      "requires": { "xlings": { "min": "2026.8.4.1" } },
      "history": [
        { "index_version": "e2aad0b", "generated_at": "…",
          "requires": { "xlings": { "min": "2026.8.4.1" } },
          "artifact": { "name": "…", "sha256": "…", "size": 371707 } },
        { "index_version": "0adb288", "generated_at": "…",
          "artifact": { "name": "…", "sha256": "…", "size": 369884 } }
      ],
      "history_truncated": false
    }
  },
  "client_latest": { "xlings": "2026.8.4.1" }
}
```

- **`history` is newest first, and `history[0]` is the current snapshot** (the
  repetition is deliberate: a client filters one list).
- **Every history entry carries its `artifact.sha256`.** A client verifies the
  entry it selects — a security property, not an option. **So only a snapshot
  listed in `history` can be selected**: outside the list there is no hash to
  verify.
- **No `requires` means no constraint**: any client may use it.
- **`history_truncated`** lets a client tell "no compatible snapshot" from "the
  compatible one was cut off".
- **`client_latest`** is the newest client version, independent of snapshots.
  **It cannot be left out**: a client routed to an old snapshot reads that
  snapshot's xlings recipe (pointing at that era's `latest`); without this field
  it never learns a newer client exists, and never upgrades.

### What `history` keeps

Not "the last N". Contracts change rarely and snapshots are produced often
(xlings-res/xim-index held 313 artefacts but only a handful of distinct
contracts). Keep the **union** of:

```
A) the last 8 snapshots, whatever their requires     -> rolling back one version works
B) the newest snapshot of each distinct requires     -> every contract generation stays reachable
capped at 32; beyond that the oldest are dropped and history_truncated is set
```

Keeping only A lets the generation an older client needs fall out of the window
while the window holds 8 snapshots with the same contract.

`tools/merge_index_pointer.py` is the reference implementation (reusable as is:
it merges read–modify–write and keeps sibling indexes' keys).

## 5. Client behaviour (what a publisher needs to know)

```
snapshots  = history (without history: the current snapshot alone)
candidates = those whose requires.xlings the client satisfies
candidates non-empty -> the newest one
candidates empty     -> an error listing the choices; the local index tree is left as it is
```

- `xlings index list` tells two things apart: `*` is **the tree on disk** (what
  the client is reading now), `>` is **what the next `update` fetches**. After
  the pointer moves and before `update` they differ; in `--json` they are
  `installed` and `current`. When "my package disappeared", look at `installed`.
- A user can pin a snapshot with `xlings index use <name> <version>`; a pin
  **bypasses the contract check** (the user asked for it) but **never the
  sha256 check**.
- A pin to a version outside `history` is an error, never a fallback to the
  newest.
- `xlings self update` **is not routed** (otherwise a client routed to an old
  snapshot could never upgrade).

## 6. Deployment order — getting this backwards hurts users

**Before a floor is declared, the history must already hold a snapshot to fall
back to.**

When a publisher first adopts this, the pointer's `history` holds only the
current entry. Declaring a floor **in the same release** leaves clients below it
**nowhere to go**: they fail with `E_INDEX_NO_COMPATIBLE_SNAPSHOT`, which is
worse than the hard failure it replaces.

The right order:

```
1. publish pointers with history, without a floor   (a few releases; history accumulates)
2. check the history holds some unconstrained or low-constraint snapshots
3. declare the floor in index-compat.json
```

`tools/merge_index_pointer.py` warns when this is broken, but cannot stop it —
the decision is the publisher's.

Likewise, when `tools/check_index_compat.py` reports "needs >= X", **do not copy
it straight into `index-compat.json`**: first make sure the history holds a
snapshot below X for older clients to land on.

## 7. Checklist

- [ ] the index tree holds `index-compat.json` (when a constraint is needed)
- [ ] the manifest the build produces carries `requires`
- [ ] the pointer carries `history` (newest first, each with its sha256) and `history_truncated`
- [ ] the pointer carries `client_latest`
- [ ] `format_version` is `2`
- [ ] merging the pointer kept sibling indexes' keys
- [ ] CI checks that every new capability the recipes use is covered by the declared `min`
