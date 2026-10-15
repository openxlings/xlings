#!/usr/bin/env python3
"""A lane of tests/lanes.toml as a workflow matrix: its shards, "1/n" .. "n/n".

    python3 tools/ci/lane_matrix.py unit           -> ["1/4", "2/4", "3/4", "4/4"]
    python3 tools/ci/lane_matrix.py --count distro -> 3
"""
import json
import sys
import tomllib
from pathlib import Path

args = sys.argv[1:]
count = args[:1] == ["--count"]
if count:
    args = args[1:]
if len(args) != 1:
    sys.exit(__doc__)
lanes = tomllib.loads((Path(__file__).resolve().parents[2] / "tests/lanes.toml").read_text())
if args[0] not in lanes:
    sys.exit(f"tests/lanes.toml has no lane {args[0]}")
shards = lanes[args[0]].get("shards", 1)
if not isinstance(shards, int) or not 1 <= shards <= 64:
    sys.exit(f"{args[0]}.shards must be 1..64")
print(shards if count else json.dumps([f"{i}/{shards}" for i in range(1, shards + 1)]))
