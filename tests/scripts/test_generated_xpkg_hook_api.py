#!/usr/bin/env python3
"""The hook interface's reference, rendered from the interface manifest of the
libxpkg xlings is built with (`xlings --xpkg-api-json`), and checked for drift.

    test_generated_xpkg_hook_api.py --xlings <path> [--write]
"""
from pathlib import Path
import argparse
import json
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--xlings", required=True)
parser.add_argument("--write", action="store_true",
                    help="rewrite docs/generated/xpkg-hook-api.md in place")
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
api = json.loads(subprocess.check_output([args.xlings, "--xpkg-api-json"], text=True))


def cell(text):
    return text.replace("|", "\\|")


lines = ["# The xpkg hook interface", "",
         "<!-- Generated from `xlings --xpkg-api-json` (libxpkg's interface manifest);",
         "     do not edit by hand. Regenerate with:",
         "       python3 tests/scripts/test_generated_xpkg_hook_api.py \\",
         "         --xlings <path-to-xlings> --write",
         "-->", "",
         "What a hook can call, as the libxpkg this xlings is built with states it. "
         "The rules are [the xpkg format](../specs/xpkg.md) §6: an **action** makes its "
         "statement true or raises, a **query** answers, a **declaration** is handed to "
         "the host, a **pure** name computes (the only kind a recipe's top level calls). "
         "A recipe of spec \"3\" sees exactly these; spec 1 and 2 also have the "
         "compatibility layer below.", "",
         "## Standard Lua in spec 3", "",
         "Globals: " + ", ".join(f"`{n}`" for n in api["stdlib"]["globals"]) + ".", "",
         "Libraries, whole: " + ", ".join(f"`{n}`" for n in api["stdlib"]["libraries"])
         + "; of `os`: " + ", ".join(f"`os.{n}`" for n in api["stdlib"]["os"]) + ".", ""]

for module in api["modules"]:
    lines += [f"## `{module['name']}` — {module['summary']}", "",
              f"`import(\"xim.libxpkg.{module['name']}\")`", "",
              "| Name | Kind | Holds after it / answers | Since | Platforms |",
              "|---|---|---|---|---|"]
    for name in module["names"]:
        lines.append(f"| `{cell(name['signature'])}` | {name['kind']} | {cell(name['summary'])} | "
                     f"{name['since'] or '—'} | {cell(name['platforms']) or '—'} |")
    lines.append("")

lines += ["## The compatibility layer (spec 1 and 2)", "",
          "Frozen: no name is added or changed. What a recipe of spec \"3\" writes instead:", "",
          "| Name | In spec 3 |", "|---|---|"]
for row in api["compat"]:
    lines.append(f"| `{cell(row['name'])}` | {cell(row['instead']) or 'nothing: it is not needed'} |")
lines.append("")

generated = "\n".join(lines).rstrip() + "\n"
reference = root / "docs/generated/xpkg-hook-api.md"
if args.write:
    reference.write_text(generated)
    print(f"wrote {reference.relative_to(root)}")
elif not reference.is_file() or generated != reference.read_text():
    raise SystemExit(
        "the generated hook interface reference is stale. Regenerate with:\n"
        f"  python3 tests/scripts/test_generated_xpkg_hook_api.py --xlings {args.xlings} --write")
