#!/usr/bin/env python3
"""Chinese must reach the help text, and must stop at what you type.

THE DEFECT THIS LOCKS DOWN

2026.8.22.3 translated the section headings and the twelve top-level command
descriptions -- which live in a hand-written table in `ui/banner.cpp` -- and
missed every subcommand, whose descriptions come from `cli/spec.cpp`. The
result was worse than no translation at all:

    xlings subos -h
      用法                          <- translated
        xlings subos [SUBCOMMAND]
      子命令                        <- translated
        new     Create a SubOS      <- not
        use     Enter a SubOS       <- not

A reader concludes the translation is broken. So this checks the boundary in
both directions: the prose is Chinese, and the things a user types are not.
"""
import os
import re
import subprocess
import sys
import pathlib
import tempfile

XLINGS = os.environ.get("XLINGS_BIN")
if not XLINGS or not pathlib.Path(XLINGS).is_file():
    print("i18n coverage: XLINGS_BIN not set to a built binary", file=sys.stderr)
    sys.exit(1)

HAN = re.compile(r"[一-鿿]")


def run(args, lang):
    with tempfile.TemporaryDirectory() as home:
        env = dict(os.environ)
        for k in ("LC_ALL", "LC_MESSAGES", "LANGUAGE"):
            env.pop(k, None)
        env["LANG"] = lang
        env["HOME"] = home
        env["XLINGS_HOME"] = os.path.join(home, "xh")
        env["XLINGS_TERM_WIDTH"] = "100"
        r = subprocess.run([XLINGS, *args], capture_output=True, text=True, env=env)
        return r.stdout + r.stderr


failures = []

# ── Chinese reaches the help, at every depth ──────────────
for args, must_contain in (
    ([], "用法"),
    (["-h"], "子命令"),
    (["subos", "-h"], "管理 SubOS"),
    (["subos", "use", "-h"], "只运行一条命令"),   # an OPTION description
    (["install", "-h"], "安装软件包"),
    (["config", "-h"], "设置语言"),
):
    out = run(args, "zh_CN.UTF-8")
    if must_contain not in out:
        failures.append(
            f"`xlings {' '.join(args)}` under zh is missing {must_contain!r}")

# ── and stops at what the user types ──────────────────────
typed = run(["subos", "use", "-h"], "zh_CN.UTF-8")
for literal in ("--sandbox", "--cmd", "<COMMAND>", "xlings subos use"):
    if literal not in typed:
        failures.append(f"zh help lost a literal the user types: {literal!r}")

# Command names stay English even when their descriptions do not.
subos = run(["subos", "-h"], "zh_CN.UTF-8")
for name in ("new", "use", "list", "remove", "info", "stop"):
    if not re.search(rf"^\s+{name}\s", subos, re.M):
        failures.append(f"zh help renamed a subcommand: {name!r}")

# ── English is unchanged ──────────────────────────────────
en = run(["subos", "-h"], "en_US.UTF-8")
if HAN.search(en):
    failures.append("English help contains Chinese")
if "Manage SubOS environments" not in en:
    failures.append("English help lost its own description")

# ── Coverage floor ────────────────────────────────────────
# Counted from the catalogue rather than the output: the output only shows one
# command at a time, and the point is that the whole surface is covered.
zh = (pathlib.Path(__file__).resolve().parents[2]
      / "modules/i18n/src/i18n/zh.cppm").read_text()
entries = len(re.findall(r'^\s*\{\s*"', zh, re.M))
FLOOR = 100
if entries < FLOOR:
    failures.append(f"zh catalogue has {entries} entries, floor is {FLOOR}")

if failures:
    print("i18n coverage: FAILED", file=sys.stderr)
    for f in failures:
        print(f"  {f}", file=sys.stderr)
    sys.exit(1)

print(f"i18n coverage: ok ({entries} zh entries; help translated at every "
      f"depth, literals and command names untouched)")
