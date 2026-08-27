#!/usr/bin/env python3
"""Point a fixture glibc recipe's `latest` at one version, and remove the rest.

Used by subos_runtime_binding_test.sh S4, which needs an index that answers
with a version no constant in the tree contains. Leaving the other entries in
place would leave a second answer to the one question the test asks.

Exits non-zero if the rewrite did not take, because a recipe that silently kept
its old versions makes S4 assert against the wrong number and report the
feature broken.
"""
import pathlib
import re
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <glibc.lua> <version>", file=sys.stderr)
        return 2
    path, version = pathlib.Path(sys.argv[1]), sys.argv[2]
    text = path.read_text()

    block = (
        "    xpm = {\n"
        "        linux = {\n"
        f'            ["latest"] = {{ ref = "{version}" }},\n'
        f'            ["{version}"] = {{ url = '
        f'"https://example.invalid/glibc-{version}.tar.gz" }},\n'
        "        },\n"
        "    },"
    )
    new, count = re.subn(r"    xpm = \{.*?\n    \},", block, text,
                         count=1, flags=re.S)
    if count != 1:
        print(f"{path}: no xpm block matched", file=sys.stderr)
        return 1
    path.write_text(new)

    check = path.read_text()
    if f'"{version}"' not in check:
        print(f"{path}: rewrite did not take", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
