#!/usr/bin/env python3
"""The pointer's history retention policy (#476, contract 10).

This policy is load-bearing, not housekeeping: the set of snapshots it keeps IS
the set a client can route to. Keep too few and an older client finds nothing it
can use; keep by recency alone and the one contract generation it needs falls off
the end while eight identical ones stay.

So the rule under test is the union of:
    A) the newest RECENT_KEEP snapshots, whatever they require
    B) the newest snapshot of each distinct `requires` value
"""
from pathlib import Path
import json
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MERGE = ROOT / "tools/merge_index_pointer.py"


def manifest(version, requires=None, generated=None):
    doc = {
        "format_version": 1,
        "index_version": version,
        "index_name": "xim",
        "generated_at": generated or f"2026-08-04T00:00:{int(version[-2:]):02d}Z",
        "artifact": {"name": f"xim-index-{version}.tar.gz",
                     "sha256": "a" * 64, "size": 1000},
    }
    if requires:
        doc["requires"] = requires
    return doc


def publish(work, sequence, client_latest=None):
    """Run the merge once per snapshot, as a real publish chain would."""
    dst = work / "pointer.json"
    for version, requires in sequence:
        src = work / "src.json"
        src.write_text(json.dumps({"format_version": 2,
                                   "indexes": {"xim": manifest(version, requires)}}))
        args = [sys.executable, str(MERGE), str(dst), str(src)]
        if client_latest:
            args.append(f"--client-latest=xlings={client_latest}")
        result = subprocess.run(args, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
    return json.loads(dst.read_text())


def versions_in(pointer):
    return [row["index_version"] for row in pointer["indexes"]["xim"]["history"]]


def test_recent_and_distinct_contracts_are_both_kept():
    xl = lambda v: {"xlings": {"min": v}}
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        # 20 snapshots, three contract generations. The oldest generation's
        # newest member is v05 -- far outside any recency window.
        sequence = []
        for i in range(1, 21):
            if i <= 5:
                req = xl("1.0.0")
            elif i <= 10:
                req = xl("2.0.0")
            else:
                req = xl("3.0.0")
            sequence.append((f"v{i:02d}", req))
        pointer = publish(work, sequence)
        kept = versions_in(pointer)

        # A: the newest 8, in order, newest first.
        assert kept[:8] == [f"v{i:02d}" for i in range(20, 12, -1)], kept

        # B: every contract generation still reachable, including the oldest,
        # whose newest member is far past the recency window.
        assert "v10" in kept, f"generation 2.0.0 unreachable: {kept}"
        assert "v05" in kept, f"generation 1.0.0 unreachable: {kept}"

        # And nothing redundant: one row per older generation, not five.
        assert kept.count("v09") == 0 and kept.count("v04") == 0, kept
        assert len(kept) == 10, f"expected 8 recent + 2 older generations: {kept}"


def test_history_is_newest_first_and_deduplicated_by_version():
    xl = {"xlings": {"min": "1.0.0"}}
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        pointer = publish(work, [("v01", xl), ("v02", xl), ("v02", xl)])
        kept = versions_in(pointer)
        assert kept == ["v02", "v01"], f"republishing a version must not duplicate it: {kept}"


def test_truncation_is_reported_not_silent():
    """A client must be able to tell 'nothing compatible exists' apart from
    'the compatible one fell off the end' -- different problems, different fixes.
    """
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        # 40 distinct contracts: every one is novel, so nothing can be dropped
        # by the dedup rule and the cap has to do the dropping.
        sequence = [(f"v{i:02d}", {"xlings": {"min": f"{i}.0.0"}}) for i in range(1, 41)]
        pointer = publish(work, sequence)
        entry = pointer["indexes"]["xim"]
        assert len(entry["history"]) == 32, len(entry["history"])
        assert entry["history_truncated"] is True


def test_client_latest_is_carried_at_top_level():
    """Without it a routed-back client cannot learn a newer client exists."""
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        pointer = publish(work, [("v01", None)], client_latest="2026.8.4.1")
        assert pointer["client_latest"]["xlings"] == "2026.8.4.1"
        assert pointer["format_version"] == 2


def test_sibling_index_keys_survive_a_publish():
    """Each index repo publishes independently; a plain overwrite drops the rest.
    This regressed once already when xim-pkgindex's per-repo CI published alone.
    """
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        dst = work / "pointer.json"
        dst.write_text(json.dumps({
            "format_version": 2,
            "indexes": {"scode": {"index_version": "s01",
                                  "artifact": {"name": "s.tar.gz", "sha256": "b" * 64}}},
        }))
        src = work / "src.json"
        src.write_text(json.dumps({"format_version": 2,
                                   "indexes": {"xim": manifest("v01")}}))
        result = subprocess.run([sys.executable, str(MERGE), str(dst), str(src)],
                                capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        pointer = json.loads(dst.read_text())
        assert set(pointer["indexes"]) == {"xim", "scode"}, pointer["indexes"].keys()


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"ok   {name}")
            except AssertionError as error:
                print(f"FAIL {name}: {error}", file=sys.stderr)
                failures += 1
    if failures:
        raise SystemExit(f"{failures} history-policy contract(s) failed")
    print("index pointer history policy: ok")
