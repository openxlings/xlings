#!/usr/bin/env python3
from pathlib import Path
import argparse
import json
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--xlings", required=True)
# Regenerating by hand means running a command nobody remembers and pasting the
# output into the right file. The check that knows the file is stale is the one
# that knows how to fix it.
parser.add_argument("--write", action="store_true",
                    help="rewrite docs/generated/command-reference.md (and luban's) in place")
parser.add_argument("--luban", default="",
                    help="the luban beside it (default: found next to --xlings)")
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
raw = subprocess.check_output([args.xlings, "--command-reference-json"], text=True)
spec = json.loads(raw)

lines = ["# Generated Command Reference", "",
         "<!-- Generated from `xlings --command-reference-json`; do not edit by hand.",
         "     Regenerate with:",
         "       python3 tests/scripts/test_generated_command_reference.py \\",
         "         --xlings <path-to-xlings> --write",
         "-->", ""]

def walk(node, path):
    current = path + ([] if node["name"] == "xlings" else [node["name"]])
    name = "xlings" + ((" " + " ".join(current)) if current else "")
    arguments = " ".join(
        (("<" if item["required"] else "[") + item["name"]
         + (">" if item["required"] else "]")
         + ("..." if item.get("variadic") else ""))
        for item in node["arguments"])
    usage = name + ((" " + arguments) if arguments else "")
    lines.extend([f"## `{usage}`", "", node["description"], ""])
    if node["options"]:
        rendered = "; ".join(
            f"`{item['syntax']}` — {item['description']}"
            for item in node["options"])
        lines.extend(["Options: " + rendered, ""])
    for child in node["subcommands"]:
        walk(child, current)

walk(spec, [])
generated = "\n".join(lines).rstrip() + "\n"
reference = root / "docs/generated/command-reference.md"
if args.write:
    reference.write_text(generated)
    print(f"wrote {reference.relative_to(root)}")
elif generated != reference.read_text():
    raise SystemExit(
        "generated command reference is stale. Regenerate with:\n"
        f"  python3 tests/scripts/test_generated_command_reference.py "
        f"--xlings {args.xlings} --write")

def assert_help(node, path):
    if path:
        result = subprocess.run([args.xlings, *path, "--help"],
                                text=True, capture_output=True)
        if result.returncode != 0:
            raise SystemExit(f"help failed for {' '.join(path)}")
        output = result.stdout + result.stderr
        if "\x1b" in output or "\x00" in output:
            raise SystemExit(f"help emitted control bytes for {' '.join(path)}")
        for option in node["options"]:
            aliases = [piece.strip().split()[0].split("<")[0].split("[")[0]
                       for piece in option["syntax"].split(",")]
            if not any(alias in output for alias in aliases):
                raise SystemExit(
                    f"CommandSpec/parser help drift for {' '.join(path)}: "
                    f"missing {option['syntax']}")
    for child in node["subcommands"]:
        assert_help(child, path + [child["name"]])

assert_help(spec, [])

for command, expected_code in ((["no-such-command"], 1),
                               (["self", "doctor", "--bogus"], 2)):
    result = subprocess.run([args.xlings, *command], text=True,
                            capture_output=True)
    if result.returncode != expected_code or result.stdout:
        raise SystemExit(
            f"invalid command was not a stderr-only failure: {command}: {result}")
    if "unknown" not in result.stderr.lower():
        raise SystemExit(f"invalid command lacks a stable diagnostic: {command}")


# luban's tree, the same way (docs/generated/luban-command-reference.md).
luban = Path(args.luban) if args.luban else next(
    (c for c in (Path(args.xlings).parent / "luban", Path(args.xlings).parent / "luban-tool" / "luban")
     if c.is_file()), None)
if luban is None:
    raise SystemExit("no luban beside the xlings under test; name it with --luban")
ltree = json.loads(subprocess.check_output([str(luban), "--command-reference-json"], text=True))
llines = ["# Generated Command Reference: luban", "",
          "<!-- Generated from `luban --command-reference-json`; do not edit by hand.",
          "     Regenerate with:",
          "       python3 tests/scripts/test_generated_command_reference.py \\",
          "         --xlings <path-to-xlings> --write",
          "-->", "",
          "Levels: `common` is what `luban --help` shows, `more` what `luban help --all` adds,",
          "`expert` what `luban help --expert` adds.", ""]

def lwalk(node, path):
    current = path + ([] if node["name"] == "luban" else [node["name"]])
    name = "luban" + ((" " + " ".join(current)) if current else "")
    arguments = " ".join(
        (("<" if item["required"] else "[") + item["name"]
         + (">" if item["required"] else "]")
         + ("..." if item.get("variadic") else ""))
        for item in node["arguments"])
    usage = name + ((" " + arguments) if arguments else "")
    level = node.get("level")
    llines.extend([f"## `{usage}`" + (f" ({level})" if level and current else ""), "", node["description"], ""])
    if node["options"]:
        llines.extend(["Options: " + "; ".join(
            f"`{item['syntax']}` — {item['description']}" for item in node["options"]), ""])
    for child in node["subcommands"]:
        lwalk(child, current)

lwalk(ltree, [])
lgenerated = "\n".join(llines).rstrip() + "\n"
lreference = root / "docs/generated/luban-command-reference.md"
if args.write:
    lreference.write_text(lgenerated)
    print(f"wrote {lreference.relative_to(root)}")
elif not lreference.exists() or lgenerated != lreference.read_text():
    raise SystemExit(
        "generated luban command reference is stale. Regenerate with:\n"
        f"  python3 tests/scripts/test_generated_command_reference.py "
        f"--xlings {args.xlings} --write")

print("generated command reference: ok")
