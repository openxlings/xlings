#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"
require_fixture_index

bin=$(find_xlings_bin)
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
home="$root/home"
index="$root/index"
cp -a "$FIXTURE_INDEX_DIR" "$index"
printf 'xim_indexrepos = {}\n' > "$index/xim-indexrepos.lua"
rm -f "$index/.xlings-index-cache.json"
mkdir -p "$index/pkgs/i"

cat > "$index/pkgs/i/infofixture.lua" <<'LUA'
local versions = {
    ["latest"] = { ref = "2026.8.3.10" },
    ["res_versioned"] = {},
    ["2026.8.3.10"] = {},
    ["2026.8.3.9"] = {},
    ["0.0.100"] = {},
    ["0.0.11"] = {},
    ["0.0.10"] = {},
    ["0.0.9"] = {},
    ["0.0.8"] = {},
    ["0.0.7"] = {},
    ["0.0.6"] = {},
    ["0.0.5"] = {},
    ["0.0.1"] = {},
}
package = {
    spec = "1",
    name = "infofixture",
    description = "Deterministic info output fixture",
    type = "script",
    archs = {"x86_64", "arm64"},
    xpm = { linux = versions, macosx = versions, windows = versions },
}
LUA

mkdir -p "$home"
cat > "$home/.xlings.json" <<EOF
{"mirror":"GLOBAL","index_repos":[{"name":"xim","url":"$index"}]}
EOF

run() { HOME="$root/user" XLINGS_HOME="$home" NO_COLOR=1 "$bin" "$@"; }
run install infofixture@0.0.1 -y >/dev/null

summary=$(run info infofixture)
# 0.0.1 is installed and the selected version is the newest, so the summary
# has to distinguish "this package is not installed" from "that version is
# not". Neither label may be a bare `installed`: the detail section below
# already owns that name for the list of versions on disk.
grep -q 'selected version.*2026.8.3.10' <<<"$summary"
grep -q 'selected installed.*no (other versions are)' <<<"$summary"
[[ $(grep -c '^ *installed ' <<<"$summary") -le 1 ]] \
  || fail "two rows labelled 'installed' in one panel"

grep -q 'more (--all-versions)' <<<"$summary"
! grep -q 'res_versioned' <<<"$summary"

all=$(run info infofixture --all-versions)
grep -q '0.0.1' <<<"$all"
! grep -q 'res_versioned' <<<"$all"
python3 - "$all" <<'PY'
import re, sys
text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", sys.argv[1])
ordered = ["2026.8.3.10", "2026.8.3.9", "0.0.100", "0.0.11", "0.0.10", "0.0.9"]
positions = []
for version in ordered:
    match = re.search(rf"(?<![0-9.]){re.escape(version)}(?![0-9.])", text)
    positions.append(-1 if match is None else match.start())
assert all(position >= 0 for position in positions), (ordered, positions, text)
assert positions == sorted(positions), (ordered, positions, text)
PY

echo "info output contract: ok"
