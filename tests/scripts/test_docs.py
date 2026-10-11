#!/usr/bin/env python3
"""The docs as a contract.

- every English chapter (docs/*.md, docs/luban/*.md) has its mirror under
  docs/zh/ with the same ##/### headings in the same order and the same code
  blocks byte for byte, and both carry their language line;
- every relative link in README*.md and docs/ resolves;
- nothing that must not be published is there (personal paths, links into
  .agents/, attributions, the old dated header);
- every `xlings` / `luban` command in a bash or console block parses against
  the command tree and its path answers --help;
- the interface answers with one terminal NDJSON line.
"""
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
parser.add_argument("--luban", default="",
                    help="the luban beside it (default: found next to --xlings)")
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
docs = root / "docs"
failures = []


def fail(message):
    failures.append(message)


def rel(path):
    return path.relative_to(root).as_posix()


# --- pairs -----------------------------------------------------------------

english = sorted(p for p in list(docs.glob("*.md")) + list((docs / "luban").glob("*.md")))
if len(english) < 20:
    fail(f"only {len(english)} English chapters found under docs/")

FENCE = re.compile(r"^```.*?^```", re.DOTALL | re.MULTILINE)


def headings(text):
    body = FENCE.sub("", text)
    return [line.split(" ", 1)[0] for line in body.splitlines()
            if line.startswith("## ") or line.startswith("### ")]


def code_blocks(text):
    return FENCE.findall(text)


for en in english:
    sub = en.relative_to(docs)
    zh = docs / "zh" / sub
    if not zh.is_file():
        fail(f"{rel(en)} has no Chinese mirror at {rel(zh)}")
        continue
    et, zt = en.read_text(), zh.read_text()
    up = "../" * len(sub.parts)
    lines_en, lines_zh = et.splitlines(), zt.splitlines()
    want_en = f"**English** | [简体中文](" + ("../" * (len(sub.parts) - 1)) + "zh/" + sub.as_posix() + ")"
    want_zh = f"[English]({up}{sub.as_posix()}) | **简体中文**"
    if len(lines_en) < 3 or lines_en[2] != want_en:
        fail(f"{rel(en)}: line 3 is not the language line `{want_en}`")
    if len(lines_zh) < 3 or lines_zh[2] != want_zh:
        fail(f"{rel(zh)}: line 3 is not the language line `{want_zh}`")
    he, hz = headings(et), headings(zt)
    if he != hz:
        fail(f"{rel(en)} and {rel(zh)}: headings differ ({len(he)} vs {len(hz)}: "
             f"{' '.join(he)} / {' '.join(hz)})")
    ce, cz = code_blocks(et), code_blocks(zt)
    if ce != cz:
        first = next((i for i, (a, b) in enumerate(zip(ce, cz)) if a != b), min(len(ce), len(cz)))
        fail(f"{rel(en)} and {rel(zh)}: code blocks differ (block {first + 1}; "
             f"{len(ce)} vs {len(cz)} blocks)")

for zh in sorted((docs / "zh").rglob("*.md")):
    if not (docs / zh.relative_to(docs / "zh")).is_file():
        fail(f"{rel(zh)} has no English original")

# --- links and forbidden strings ---------------------------------------------

published = [root / "README.md", root / "README.zh.md"] + sorted(docs.rglob("*.md"))
LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")
FORBIDDEN = [
    (re.compile(r"/home/(?!user\b|u\b|runner\b)[a-z][\w.-]*/"), "a personal home path"),
    (re.compile(r"[A-Z]:\\\\?Users\\\\?(?!runneradmin|user)\w+"), "a personal Windows path"),
    (re.compile(r"\]\((\.\./)*\.agents/"), "a link into .agents/"),
    (re.compile(r"\.agents/docs/\d{4}-"), "a reference to a working document in .agents/docs"),
    (re.compile(r"编写日期|更新日期"), "the old dated header"),
    (re.compile(r"maintainer's rule|维护者.{0,6}(说|规定|定的)"), "an attribution"),
    (re.compile(r"xlings subos create|xlings subos enter|\{\"action\":\"subos\.new\""), "a removed command"),
]
for path in published:
    text = path.read_text()
    if path.parent.name == "generated":
        continue
    body = FENCE.sub("", text)
    for target in LINK.findall(body):
        if re.match(r"^[a-z]+:", target) or target.startswith("#"):
            continue
        file = target.split("#", 1)[0]
        if not file:
            continue
        if not (path.parent / file).exists():
            fail(f"{rel(path)}: broken link {target}")
    for pattern, what in FORBIDDEN:
        match = pattern.search(text)
        if match:
            fail(f"{rel(path)}: {what}: {match.group(0)!r}")

readmes = (root / "README.md").read_text() + (root / "README.zh.md").read_text()
if not re.search(r"macOS.*HOME", readmes, re.DOTALL):
    fail("README: the macOS HOME-redirection warning is missing")

# --- commands ----------------------------------------------------------------


def aliases(option):
    return {piece.strip().split()[0].split("<")[0].split("[")[0].split("=")[0]
            for piece in option["syntax"].split(",")}


def options_of(node):
    found = set()
    for option in node.get("options", []):
        found |= aliases(option)
    return found


def check_commands(program, binary, tree, text):
    root_options = options_of(tree)
    paths = set()
    for block in re.finditer(r"```(?:bash|console)\n(.*?)```", text, re.DOTALL):
        for raw in block.group(1).splitlines():
            line = raw.strip()
            if line.startswith("$ "):
                line = line[2:]
            if not line.startswith(program + " "):
                continue
            try:
                tokens = shlex.split(line, comments=True)
            except ValueError as error:
                fail(f"unparseable documented command: {line}: {error}")
                continue
            if len(tokens) > 1 and tokens[1].startswith("<"):    # a placeholder: `luban <command> --help`
                continue
            if len(tokens) < 2 or tokens[1].startswith("-"):
                for token in tokens[1:]:
                    if token.startswith("-") and token.split("=", 1)[0] not in root_options:
                        fail(f"unknown {program} option {token}: {line}")
                continue
            node = next((item for item in tree["subcommands"] if item["name"] == tokens[1]), None)
            if node is None:
                fail(f"unknown documented command: {line}")
                continue
            path = [node["name"]]
            if len(tokens) > 2 and not tokens[2].startswith("-"):
                child = next((item for item in node.get("subcommands", []) if item["name"] == tokens[2]), None)
                if child is not None:
                    node = child
                    path.append(child["name"])
            allowed = root_options | options_of(node)
            for token in tokens[2:]:
                if token == "--":       # what follows is the command's own argv
                    break
                if not token.startswith("-") or token == "-":
                    continue
                if token.split("=", 1)[0] not in allowed:
                    fail(f"undocumented {program} option {token.split('=', 1)[0]}: {line}")
            paths.add(tuple(path))
    for path in sorted(paths):
        result = subprocess.run([binary, *path, "--help"], text=True, capture_output=True, env=env)
        if result.returncode != 0:
            fail(f"documented command path failed --help: {program} {' '.join(path)}\n"
                 f"{result.stdout}{result.stderr}")
    return len(paths)


temp = tempfile.TemporaryDirectory(prefix="xlings-docs-")
env = os.environ.copy()
for name in ("XLINGS_SUBOS_MODE", "XLINGS_ACTIVE_SUBOS", "XLINGS_SUBOS_LIB", "XLINGS_BIN"):
    env.pop(name, None)
env.update({"HOME": temp.name, "XLINGS_HOME": str(Path(temp.name) / ".xlings"), "NO_COLOR": "1"})

text = "\n".join(p.read_text() for p in published if p.parent.name != "generated")
xtree = json.loads(subprocess.check_output([args.xlings, "--command-reference-json"], text=True, env=env))
counted = check_commands("xlings", args.xlings, xtree, text)
if counted < 30:
    fail(f"only {counted} xlings command paths found in the docs")

luban = Path(args.luban) if args.luban else next(
    (c for c in (Path(args.xlings).parent / "luban", Path(args.xlings).parent / "luban-tool" / "luban")
     if c.is_file()), None)
if luban is None:
    fail("no luban beside the xlings under test; name it with --luban")
    lcounted = 0
else:
    ltree = json.loads(subprocess.check_output([str(luban), "--command-reference-json"], text=True, env=env))
    lcounted = check_commands("luban", str(luban), ltree, text)
    if lcounted < 10:
        fail(f"only {lcounted} luban command paths found in the docs")

# --- the interface -----------------------------------------------------------

for capability, expected_code in (("system_status", 0), ("no_such", 1)):
    result = subprocess.run([args.xlings, "interface", capability, "--args", "{}"],
                            text=True, capture_output=True, env=env)
    events = [json.loads(line) for line in result.stdout.splitlines() if line]
    terminal = [event for event in events if event.get("kind") == "result"]
    if len(terminal) != 1 or terminal[0].get("exitCode") != expected_code:
        fail(f"invalid terminal NDJSON event for {capability}: {events}")
    if result.returncode != expected_code:
        fail(f"interface exit mismatch for {capability}")

temp.cleanup()
if failures:
    raise SystemExit("docs: " + str(len(failures)) + " problem(s)\n  " + "\n  ".join(failures))
print(f"docs: ok ({len(english)} chapter pairs, {counted} xlings and {lcounted} luban command paths, NDJSON)")
