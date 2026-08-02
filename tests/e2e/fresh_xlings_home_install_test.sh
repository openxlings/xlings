#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"
require_fixture_index

bin=$(find_xlings_bin)
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
home="$root/explicit-home"
mkdir -p "$home/data"
cp -a "$FIXTURE_INDEX_DIR" "$home/data/xim-pkgindex"
printf 'xim_indexrepos = {}\n' > "$home/data/xim-pkgindex/xim-indexrepos.lua"
rm -f "$home/data/xim-pkgindex/.xlings-index-cache.json"
cat > "$home/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [{"name": "xim", "url": "$FIXTURE_INDEX_DIR"}]
}
EOF

[[ ! -e "$home/subos" ]] || fail "precondition: subos tree already exists"
HOME="$root/user" XLINGS_HOME="$home" "$bin" install xpkg-helper -y

state="$home/subos/default/.xlings.json"
[[ -f "$state" ]] || fail "issue #471: workspace state was not created"
python3 - "$state" <<'PY'
import json, pathlib, sys
workspace = json.loads(pathlib.Path(sys.argv[1]).read_text())["workspace"]
entry = workspace["xpkg-helper"]
assert entry["active"] == "0.0.1", entry
assert entry["installed"] == ["0.0.1"], entry
PY

list_output=$(HOME="$root/user" XLINGS_HOME="$home" "$bin" list)
grep -q 'xpkg-helper@0.0.1' <<<"$list_output"
echo "fresh XLINGS_HOME first install: ok"
