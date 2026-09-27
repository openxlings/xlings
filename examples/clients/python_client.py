#!/usr/bin/env python3
"""Minimal xlings interface v1 client (Python 3).

Demonstrates spawning xlings and consuming the NDJSON wire format
event-by-event — no third-party deps. Mirrors examples/clients/bash_jq.sh.

Run:
    XLINGS_HOME=/tmp/xlings-client-test python3 examples/clients/python_client.py
"""
from __future__ import annotations
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Iterator


def find_xlings() -> str:
    for cand in [
        "build/linux/x86_64/release/xlings",
        "build/macosx/arm64/release/xlings",
        "build/macosx/x86_64/release/xlings",
    ]:
        p = Path(cand).resolve()
        if p.is_file() and os.access(p, os.X_OK):
            return str(p)
    on_path = shutil.which("xlings")
    if on_path:
        return on_path
    sys.exit("ERROR: xlings binary not found (no ./build/... and not on PATH)")


def call(xlings: str, *args: str) -> Iterator[dict]:
    """Yield parsed NDJSON objects from `xlings <args...>`."""
    proc = subprocess.run(
        [xlings, *args],
        capture_output=True, text=True, check=False,
    )
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            yield json.loads(line)
        except json.JSONDecodeError:
            # If a single JSON object was printed (e.g. --version), it
            # arrives as one line just like NDJSON — same path.
            continue


def main() -> int:
    xlings = find_xlings()
    print(f"→ using {xlings}")

    # 1. protocol_version (one-line JSON, not NDJSON)
    [version_obj] = list(call(xlings, "interface", "--version"))
    print(f"protocol_version: {version_obj['protocol_version']}")
    # Major version only: a minor bump is additive, and a capability is
    # detected by its presence on the wire, not by this number.
    assert version_obj["protocol_version"].split(".")[0] == "1"

    # 2. capability list (one big JSON object)
    [list_obj] = list(call(xlings, "interface", "--list"))
    caps = sorted(c["name"] for c in list_obj["capabilities"])
    print(f"capability count: {len(caps)}")
    for name in caps:
        print(f"  - {name}")

    # 3. env capability (NDJSON)
    print("→ env:")
    for ev in call(xlings, "interface", "env", "--args", "{}"):
        if ev.get("kind") == "data" and ev.get("dataKind") == "env":
            p = ev["payload"]
            print(f"  XLINGS_HOME = {p['xlingsHome']}")
            print(f"  activeSubos  = {p['activeSubos']!r}")
            print(f"  binDir       = {p['binDir']}")

    # 4. list_subos
    print("→ sub-OSs:")
    for ev in call(xlings, "interface", "list_subos", "--args", "{}"):
        if ev.get("kind") == "data" and ev.get("dataKind") == "subos_list":
            for e in ev["payload"]["entries"]:
                tag = "active" if e["active"] else "inactive"
                print(f"  - {e['name']} ({tag}, {e['pkgCount']} pkg)")

    # 5. plan_install xim:bun (dry-run, doesn't install)
    print("→ plan_install xim:bun (dry-run):")
    exit_code = None
    for ev in call(xlings, "interface", "plan_install", "--args", '{"targets":["xim:bun"]}'):
        if ev.get("kind") == "data" and ev.get("dataKind") == "install_plan":
            for pkg in ev["payload"].get("packages", []):
                # plan packages come as [nameVer, ""] tuples
                name = pkg[0] if isinstance(pkg, list) else pkg
                print(f"  + {name}")
        elif ev.get("kind") == "result":
            exit_code = ev.get("exitCode")
    print(f"plan_install exitCode: {exit_code}")

    print("✓ python_client done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
