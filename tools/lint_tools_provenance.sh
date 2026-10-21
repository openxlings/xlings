#!/usr/bin/env bash
# Lint: every tool in the table (modules/subos/src/sandbox/tools.cpp) is a
# package of the index -- present, and for every architecture xlings is
# released for on its platform -- unless the operating system itself is the
# tool and the entry says why (the aarch64 design §2.1, TOOLS-PROVENANCE).
#
# The index is a checkout of openxlings/xim-pkgindex: XLINGS_PROVENANCE_INDEX
# (a directory), else a shallow clone of XLINGS_PROVENANCE_INDEX_REF (default
# main). The table is read from the source, so no build is needed.
# xtest: covers=TOOLS-PROVENANCE requires=network
set -euo pipefail
cd "$(dirname "$0")/.."

index="${XLINGS_PROVENANCE_INDEX:-}"
if [[ -z "$index" ]]; then
  index="$(mktemp -d)"; trap 'rm -rf "$index"' EXIT
  git clone -q --depth 1 --branch "${XLINGS_PROVENANCE_INDEX_REF:-main}" \
    https://github.com/openxlings/xim-pkgindex.git "$index"
fi

python3 - "$index" <<'PY'
import os, re, sys

index = sys.argv[1]
src = open("modules/subos/src/sandbox/tools.cpp", encoding="utf-8").read()
table = src[src.index("const std::vector<Entry>& table()"):src.index("const Entry* entry(")]
# {{"name", "package"[, "os"|{}][, "platform"][, "planned"]}, "xim-x-dir", ...
rows = re.findall(r'\{\{("[^"]*"(?:,\s*(?:"[^"]*"|\{\}))*)\},\s*"([^"]*)"', table)
if len(rows) < 10:
    sys.exit(f"read {len(rows)} tools from the table; the pattern no longer matches it")
bad = []
for fields, xpkgs_dir in rows:
    vals = [v.strip().strip('"') if v.strip() != "{}" else "" for v in re.split(r',\s*', fields)]
    name, package = vals[0], vals[1]
    os_reason = vals[2] if len(vals) > 2 else ""
    platform = vals[3] if len(vals) > 3 else ""
    planned = vals[4] if len(vals) > 4 else ""
    if not package:
        if not os_reason:
            bad.append(f"{name}: no package, and no reason the operating system provides it")
        continue
    if planned:
        print(f"  {name}: planned ({planned})")
        continue
    if xpkgs_dir != f"xim-x-{package}":
        bad.append(f"{name}: payload directory {xpkgs_dir} is not xim-x-{package}")
    recipe = os.path.join(index, "pkgs", package[0], f"{package}.lua")
    if not os.path.isfile(recipe):
        bad.append(f"{name}: package {package} is not in the index ({recipe})")
        continue
    text = open(recipe, encoding="utf-8").read()
    m = re.search(r'archs\s*=\s*\{([^}]*)\}', text)
    archs = set(re.findall(r'"([^"]+)"', m.group(1))) if m else set()
    plat = platform or "linux"
    if plat.startswith("linux"):
        if not re.search(r'\blinux\s*=\s*\{', text):
            bad.append(f"{name}: {package} has no linux block")
        # linux-<arch>: a package built for that architecture alone.
        missing = ({plat.split("-", 1)[1]} if "-" in plat else {"x86_64", "aarch64"}) - archs
        if missing:
            bad.append(f"{name}: {package} is not built for {', '.join(sorted(missing))} (archs = {sorted(archs)})")
    print(f"  {name}: {package} ({', '.join(sorted(archs))})")
if bad:
    for b in bad:
        print(f"::error::{b}")
    sys.exit(1)
print(f"tools provenance: {len(rows)} tools, each a package of the index or the OS's own")
PY
