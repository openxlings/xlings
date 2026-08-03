#!/usr/bin/env python3
"""Merge freshly built index manifests into the published combined pointer,
carrying an addressable history of past snapshots (#476).

    merge_index_pointer.py <dst-pointer> <src-pointer> [--client-latest name=ver]...

Only the keys present in <src-pointer> are touched; sibling indexes keep theirs,
because each index repo publishes independently and a plain overwrite would drop
the others.

History is NOT "the last N snapshots". A client routing back needs the newest
snapshot whose declared contract it satisfies, and contracts change rarely while
snapshots are produced constantly -- xlings-res/xim-index holds 313 artifacts and
a handful of distinct contracts. So the retained set is the union of:

    A) the newest RECENT_KEEP snapshots, whatever they require
       -- so "roll back one" stays possible, which is what debugging needs;
    B) the newest snapshot of each distinct `requires` value
       -- so every contract generation stays reachable, which is what routing
          needs.

capped at MAX_HISTORY entries, newest first. Dropping past the cap sets
`history_truncated`, which lets a client tell "no compatible snapshot exists"
apart from "the compatible one fell off the end" -- different problems with
different fixes.
"""
import json
import os
import sys

RECENT_KEEP = 8
MAX_HISTORY = 32


def snapshot_of(manifest):
    """The history row describing a manifest."""
    row = {
        "index_version": manifest.get("index_version", ""),
        "generated_at": manifest.get("generated_at", ""),
        "artifact": manifest.get("artifact", {}),
    }
    requires = manifest.get("requires")
    if requires:
        row["requires"] = requires
    return row


def contract_key(row):
    """Identity of a row's contract, for the 'one per distinct contract' rule."""
    return json.dumps(row.get("requires", {}), sort_keys=True, separators=(",", ":"))


def merge_history(previous, incoming):
    """previous: existing history (newest first). incoming: the new head row."""
    rows = [incoming]
    for row in previous:
        # The incoming snapshot replaces any stale row for the same version --
        # a republish of the same index_version is the same snapshot.
        if row.get("index_version") == incoming.get("index_version"):
            continue
        rows.append(row)

    keep, seen_contracts = [], set()
    for position, row in enumerate(rows):
        recent = position < RECENT_KEEP
        contract = contract_key(row)
        novel = contract not in seen_contracts
        seen_contracts.add(contract)
        if recent or novel:
            keep.append(row)

    truncated = len(keep) > MAX_HISTORY
    return keep[:MAX_HISTORY], truncated


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    dst_path, src_path = args

    client_latest = {}
    for arg in sys.argv[1:]:
        if arg.startswith("--client-latest="):
            name, _, version = arg[len("--client-latest="):].partition("=")
            if name and version:
                client_latest[name] = version

    base = {"format_version": 2, "indexes": {}}
    if os.path.exists(dst_path):
        try:
            with open(dst_path, encoding="utf-8") as handle:
                base = json.load(handle)
        except Exception:
            # A corrupt published pointer must not stop a publish; it is fully
            # regenerated below for the keys we own, and sibling keys were
            # already unreadable.
            base = {"format_version": 2, "indexes": {}}
    base.setdefault("indexes", {})

    with open(src_path, encoding="utf-8") as handle:
        incoming = json.load(handle).get("indexes", {})

    for key, manifest in incoming.items():
        previous = base["indexes"].get(key, {}).get("history", [])
        history, truncated = merge_history(previous, snapshot_of(manifest))

        # Declaring a floor only helps if there is somewhere to route back TO.
        # The first publish after adopting this carries a one-entry history, so
        # a floor declared in that same publish strands every client below it
        # instead of routing it -- worse than the hard failure it replaces.
        # Publish history first, let it accumulate, then raise the floor.
        head_requires = manifest.get("requires", {}).get("xlings", {})
        if head_requires.get("min"):
            escape = [r for r in history[1:]
                      if not r.get("requires", {}).get("xlings", {}).get("min")]
            older = [r for r in history[1:]]
            if not older:
                print(f"[pointers] WARNING: {key} declares a client floor but "
                      f"publishes no older snapshot. Clients below "
                      f"{head_requires['min']} will have nowhere to route and "
                      f"will fail outright. Publish history before raising a "
                      f"floor.", file=sys.stderr)
            elif not escape:
                print(f"[pointers] note: {key} floor {head_requires['min']}; "
                      f"every published snapshot declares one, so clients older "
                      f"than the lowest will still fail.", file=sys.stderr)
        entry = dict(manifest)
        entry["history"] = history
        entry["history_truncated"] = truncated
        base["indexes"][key] = entry
        print(f"[pointers] {key}: {len(history)} snapshot(s) in history"
              f"{' (truncated)' if truncated else ''}")

    # Newest CLIENT version, carried at top level so it is readable no matter
    # which snapshot a client routes to. Without it a routed-back client cannot
    # learn that a newer client exists, and can never leave the old snapshot.
    if client_latest:
        base.setdefault("client_latest", {}).update(client_latest)

    base["format_version"] = 2
    with open(dst_path, "w", encoding="utf-8") as handle:
        json.dump(base, handle, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
