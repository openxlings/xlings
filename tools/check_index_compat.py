#!/usr/bin/env python3
"""Check that an index tree's declared client floor covers what its recipes use.

    check_index_compat.py <index-tree>

A floor nobody remembers to raise is not a contract. This scans the recipes for
constructs that only newer clients understand and asserts the tree's
`index-compat.json` declares a `requires.xlings.min` at least that high --
turning "the author promised" into "the tree says so, and it was checked".

Meant to run in the INDEX repo's CI (xim-pkgindex and any third-party index),
not in xlings's. It lives here because xlings owns the table: it is the one that
knows which of its own versions introduced which capability.

Exit 0 = the declared floor covers everything found.
Exit 1 = something in the tree needs a client newer than the tree admits.
"""
from pathlib import Path
import json
import re
import sys

# Construct -> the first xlings version that understands it.
#
# Append when a capability lands; the entry is what makes the next index adopting
# it fail loudly here instead of on a user's machine. Each pattern is matched
# against recipe text, so keep them specific enough not to fire on prose.
CAPABILITIES = [
    (re.compile(r'kind\s*=\s*["\']files["\']'),  "0.4.70",
     "xvm registration kind 'files'"),
    (re.compile(r'kind\s*=\s*["\']group["\']'),  "0.4.60",
     "xvm registration kind 'group'"),
    (re.compile(r'\bspec\s*=\s*["\']2["\']'),    "0.4.52",
     "xpkg spec 2"),
    (re.compile(r'\bsha256_by_arch\b|\barch_alias\b|\$\{arch\}'), "0.4.52",
     "per-arch resource entry"),
]


def compare_versions(left: str, right: str) -> int:
    """Component-wise numeric compare; mirrors version_order::compare.

    Not semver: xlings versions are four components (YYYY.M.D.N) and semver's
    three-component parser cannot read them at all.
    """
    def parts(value):
        out = []
        for chunk in value.split("-")[0].split("."):
            out.append(int(chunk) if chunk.isdigit() else -1)
        return out

    a, b = parts(left), parts(right)
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else 0
        y = b[i] if i < len(b) else 0
        if x != y:
            return -1 if x < y else 1
    return 0


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    tree = Path(sys.argv[1])
    if not (tree / "pkgs").is_dir():
        print(f"not an index tree (no pkgs/): {tree}", file=sys.stderr)
        return 2

    declared = ""
    compat = tree / "index-compat.json"
    if compat.is_file():
        try:
            doc = json.loads(compat.read_text(encoding="utf-8"))
            declared = doc.get("requires", {}).get("xlings", {}).get("min", "")
        except Exception as error:  # noqa: BLE001
            print(f"index-compat.json unreadable: {error}", file=sys.stderr)
            return 1

    needed, why = "", []
    for recipe in sorted((tree / "pkgs").rglob("*.lua")):
        text = recipe.read_text(encoding="utf-8", errors="replace")
        for pattern, version, label in CAPABILITIES:
            if not pattern.search(text):
                continue
            why.append((recipe.relative_to(tree), label, version))
            if not needed or compare_versions(version, needed) > 0:
                needed = version

    if not needed:
        print("index compat: no version-gated construct found; any client works")
        return 0

    print(f"index compat: newest construct needs xlings >= {needed}")
    for path, label, version in sorted(why)[:10]:
        print(f"  {path}: {label} (>= {version})")
    if len(why) > 10:
        print(f"  … {len(why) - 10} more")

    if declared and compare_versions(declared, needed) >= 0:
        print(f"index compat: declared floor {declared} covers it")
        return 0

    print("", file=sys.stderr)
    print(f"FAIL: this index uses constructs needing xlings >= {needed}, but "
          f"index-compat.json declares "
          f"{declared or 'no floor at all'}.", file=sys.stderr)
    print(f"      Older clients would hard-fail on them instead of being routed "
          f"to a snapshot they can use.", file=sys.stderr)
    print(f'      Fix: set requires.xlings.min to "{needed}" (or higher) in '
          f"index-compat.json.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
