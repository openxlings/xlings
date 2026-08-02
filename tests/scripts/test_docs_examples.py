#!/usr/bin/env python3
from pathlib import Path
import argparse
import json
import os
import re
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--xlings", required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
files = [
    root / "README.md",
    root / "README.zh.md",
    root / "docs/quick-start/multi-version.md",
    root / "docs/quick-start/subos-and-agent.md",
    root / "docs/design/subos-isolation.md",
    root / "docs/design/interface-protocol.md",
]
text = "\n".join(path.read_text() for path in files)
for stale in ("xlings subos create", "xlings subos enter",
              '{"action":"subos.new"', "版本: 0.4.36"):
    if stale in text:
        raise SystemExit(f"stale command/protocol remains: {stale}")
if not re.search(r"macOS.*HOME", text, re.DOTALL):
    raise SystemExit("macOS HOME-redirection warning missing")

spec = json.loads(subprocess.check_output(
    [args.xlings, "--command-reference-json"], text=True))

def aliases(option):
    return {piece.strip().split()[0].split("<")[0].split("[")[0]
            for piece in option["syntax"].split(",")}

root_options = set().union(*(aliases(o) for o in spec["options"]))
paths = set()
for match in re.finditer(r"```(?:bash|console)\n(.*?)```", text, re.DOTALL):
    for raw in match.group(1).splitlines():
        line = raw.strip()
        if not line.startswith("xlings "):
            continue
        try:
            tokens = shlex.split(line)
        except ValueError as error:
            raise SystemExit(f"unparseable documented command: {line}: {error}")
        if len(tokens) < 2:
            continue
        node = next((item for item in spec["subcommands"]
                     if tokens[1] == item["name"]), None)
        if node is None:
            raise SystemExit(f"unknown documented command: {line}")
        path = [node["name"]]
        if len(tokens) > 2 and not tokens[2].startswith("-"):
            child = next((item for item in node["subcommands"]
                          if tokens[2] == item["name"]), None)
            if child is not None:
                node = child
                path.append(child["name"])
        allowed = root_options | set().union(
            *(aliases(o) for o in node["options"]))
        for token in tokens[2:]:
            if not token.startswith("-"):
                continue
            option = token.split("=", 1)[0]
            if option not in allowed:
                raise SystemExit(f"undocumented parser option {option}: {line}")
        paths.add(tuple(path))

if not paths:
    raise SystemExit("no documented xlings commands found")
for path in sorted(paths):
    result = subprocess.run([args.xlings, *path, "--help"],
                            text=True, capture_output=True)
    if result.returncode != 0:
        raise SystemExit(f"documented command path failed help validation: {path}\n"
                         f"{result.stdout}{result.stderr}")

with tempfile.TemporaryDirectory(prefix="xlings-doc-interface-") as temp:
    env = os.environ.copy()
    env.update({"HOME": temp, "XLINGS_HOME": str(Path(temp) / ".xlings"),
                "NO_COLOR": "1"})
    for capability, expected_code in (("system_status", 0), ("no_such", 1)):
        result = subprocess.run(
            [args.xlings, "interface", capability, "--args", "{}"],
            text=True, capture_output=True, env=env)
        events = [json.loads(line) for line in result.stdout.splitlines() if line]
        terminal = [event for event in events if event.get("kind") == "result"]
        if len(terminal) != 1 or terminal[0].get("exitCode") != expected_code:
            raise SystemExit(f"invalid terminal NDJSON event for {capability}: {events}")
        if result.returncode != expected_code:
            raise SystemExit(f"interface exit mismatch for {capability}")

print(f"documentation examples: ok ({len(paths)} command paths + NDJSON)")
