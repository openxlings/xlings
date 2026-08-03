#!/usr/bin/env python3
"""Two-way parity between the CommandSpec and the argv parsers that actually run.

The spec is a *second* description of the CLI surface. Help text, the generated
command reference and `validate_manual_argv` are all rendered from it, so any
test that reads one of those and compares it to the spec is comparing the spec
to itself -- it stays green while the spec and the real parser disagree.

This driver never reads the spec's own rendering. It goes both ways:

  forward  every (command path, option) the spec publishes is handed to a real
           `xlings` process; the run must not come back with a parse-level
           diagnostic. Business failures ("subos not found", "package index not
           available") are expected and ignored -- they prove the argv was
           accepted and execution reached the command itself.

  reverse  every option literal the hand-written argv loops compare against
           must exist in the spec, so the parser cannot quietly accept a flag
           that `--help` and the generated reference never mention.

Everything runs against a throwaway XLINGS_HOME wired to an empty local index,
so no probe touches the user's home or the network.
"""
from pathlib import Path
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]

# Diagnostics the argv layer emits. Seeing one of these means the parser
# rejected a spelling the spec advertises.
PARSE_DIAGNOSTICS = (
    "unknown option",
    "unknown subcommand",
    "surplus positional argument",
    "missing value for option",
    "unknown command",
    "unknown 'self' action",
    "unrecognized",
    "unexpected argument",
)

# A value for options that take one. Deliberately not a value the parser is
# allowed to special-case: an option whose acceptance depends on a hard-coded
# value whitelist is exactly the drift this driver exists to catch.
OPTION_VALUES = {
    "--lang": "en",
    "--mirror": "GLOBAL",
    "--add-xpkg": "__parity_probe__.lua",
    "--index-repo": "parityns:https://example.invalid/index.git",
    "--storage": "shared",
    "--image-size": "1G",
    "--from": "__parity_probe__",
    "--shell": "powershell",
    "--sandbox": "proot",
    "--cmd": "exit 0",
    "--ttl": "5",
    "--args": "{}",
    "--args-file": "__parity_probe__.json",
}

# Positional stand-ins. Every one of them is a name that cannot resolve, so the
# command fails at the business layer immediately after parsing -- which is the
# only thing this driver measures.
POSITIONAL = {
    "packages": "__parity_probe__",
    "package": "__parity_probe__",
    "keyword": "__parity_probe__",
    "filter": "__parity_probe__",
    "target": "__parity_probe__",
    "name": "__parity_probe__",
    "script-file": "__parity_probe__.lua",
    "generation": "1",
    "capability": "system_status",
    "version": "0.0.1",
    "reason": "parity",
}

# `xlings self install` rewrites the target home from the running package and
# `self update` reaches the network. Neither carries an option in the spec, so
# the forward pass never needs them; listing them here documents the exclusion
# instead of leaving it to coincidence.
NEVER_EXECUTE = {("self", "install"), ("self", "update"), ("self", "migrate")}


def walk(node, path=()):
    if node["name"] != "xlings":
        path = path + (node["name"],)
    yield path, node
    for child in node["subcommands"]:
        yield from walk(child, path)


def aliases(option):
    """`-g, --global` -> ['-g', '--global']; strips <VALUE> / [KIND] suffixes."""
    out = []
    for piece in option["syntax"].split(","):
        piece = piece.strip()
        if not piece:
            continue
        out.append(re.split(r"[ <\[=]", piece, maxsplit=1)[0])
    return [alias for alias in out if alias.startswith("-")]


def probe_argv(path, node, alias, option):
    argv = list(path)
    for argument in node["arguments"]:
        if argument["required"]:
            argv.append(POSITIONAL.get(argument["name"], "__parity_probe__"))
    argv.append(alias)
    if "<" in option["syntax"]:
        argv.append(OPTION_VALUES.get(alias, "__parity_probe__"))
    elif "[" in option["syntax"]:
        argv.append(OPTION_VALUES.get(alias, "__parity_probe__"))
    return argv


def run(xlings, argv, env):
    try:
        result = subprocess.run([xlings, *argv], text=True, capture_output=True,
                                env=env, timeout=90)
    except subprocess.TimeoutExpired:
        # Reaching a hang means the argv was accepted and the command started
        # real work; that is a pass for parity, but say so out loud rather than
        # letting a silent timeout look like a clean run.
        return f"[timeout] {' '.join(argv)}"
    return result.stdout + result.stderr


def forward(xlings, spec, env, failures):
    root_options = spec["options"]
    checked = 0
    for path, node in walk(spec):
        if not path or tuple(path) in NEVER_EXECUTE:
            continue
        options = list(node["options"])
        # Global options are published on the root and documented by
        # `xlings --help`, so every command has to accept them. They are the
        # ones a user or an agent is most likely to append by habit.
        if not node["subcommands"]:
            options = options + root_options
        for option in options:
            for alias in aliases(option):
                if alias in ("-h", "--help", "--version"):
                    continue
                argv = probe_argv(path, node, alias, option)
                output = run(xlings, argv, env)
                checked += 1
                lowered = output.lower()
                for diagnostic in PARSE_DIAGNOSTICS:
                    if diagnostic in lowered:
                        failures.append(
                            f"parser rejects a spelling the spec publishes:\n"
                            f"    xlings {' '.join(argv)}\n"
                            f"    -> {output.strip().splitlines()[0]}")
                        break
    return checked


# Every shape the hand-written argv loops use to recognise a flag.
LITERAL_PATTERNS = (
    re.compile(r'\b(?:a|arg|action|token|next)\s*==\s*"(-{1,2}[a-zA-Z][\w-]*)"'),
    re.compile(r'\.rfind\("(-{1,2}[a-zA-Z][\w-]*)=", 0\)'),
    re.compile(r'\bcmdline::Option\("([a-zA-Z][\w-]*)"\)'),
)

PARSER_SOURCES = (
    "src/core/subos.cppm",
    "src/core/xself.cppm",
    "src/cli.cppm",
)

# Flags consumed and removed before any command sees them, or diagnostics-only
# spellings that are deliberately undocumented.
REVERSE_EXEMPT = {"--command-reference-json"}


def spec_flags(spec):
    flags = set()
    for _, node in walk(spec):
        for option in node["options"]:
            flags.update(aliases(option))
    return flags


def reverse(spec, failures):
    published = spec_flags(spec)
    checked = 0
    for relative in PARSER_SOURCES:
        text = (ROOT / relative).read_text()
        for pattern in LITERAL_PATTERNS:
            for match in pattern.finditer(text):
                literal = match.group(1)
                flag = literal if literal.startswith("-") else "--" + literal
                if flag in REVERSE_EXEMPT:
                    continue
                checked += 1
                if flag not in published:
                    failures.append(
                        f"parser accepts an undocumented flag: {flag} "
                        f"({relative}) -- add it to cli/spec.cppm or stop "
                        f"accepting it")
    return checked


def isolated_env(home):
    index = home / "index"
    (index / "pkgs").mkdir(parents=True)
    (index / "xim-indexrepos.lua").write_text("xim_indexrepos = {}\n")
    xlings_home = home / "xlings-home"
    xlings_home.mkdir()
    (xlings_home / ".xlings.json").write_text(json.dumps({
        "mirror": "GLOBAL",
        "index_repos": [{"name": "xim", "url": str(index)}],
    }))
    env = os.environ.copy()
    env.update({
        "HOME": str(home / "user"),
        "USERPROFILE": str(home / "user"),
        "XLINGS_HOME": str(xlings_home),
        "XLINGS_NON_INTERACTIVE": "1",
        "NO_COLOR": "1",
    })
    (home / "user").mkdir()
    return env


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--xlings", required=True)
    args = parser.parse_args()
    xlings = str(Path(args.xlings).resolve())

    spec = json.loads(subprocess.check_output(
        [xlings, "--command-reference-json"], text=True))

    failures = []
    with tempfile.TemporaryDirectory(prefix="xlings-parity-") as temp:
        home = Path(temp)
        env = isolated_env(home)
        forward_checked = forward(xlings, spec, env, failures)
    reverse_checked = reverse(spec, failures)

    # A scan that matches nothing reports the same "ok" as a scan that matched
    # everything and found no drift. These floors are what stops a renamed
    # source file or a reworded argv loop from turning this driver into a
    # no-op that keeps passing.
    if forward_checked < 40:
        failures.append(f"forward pass executed only {forward_checked} option "
                        f"spellings -- the spec walk is not reaching the tree")
    if reverse_checked < 20:
        failures.append(f"reverse pass matched only {reverse_checked} parser "
                        f"literals -- LITERAL_PATTERNS no longer match the "
                        f"argv loops in {', '.join(PARSER_SOURCES)}")

    if failures:
        print("CommandSpec / parser parity failed:\n", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}\n", file=sys.stderr)
        raise SystemExit(1)
    print(f"cli spec parity: ok ({forward_checked} option spellings executed, "
          f"{reverse_checked} parser literals checked)")


if __name__ == "__main__":
    main()
