#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"
require_fixture_index
bin=$(find_xlings_bin)
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
home="$root/.xlings"
mkdir -p "$home/data" "$home/subos/default" "$home/subos/other"
cp -a "$FIXTURE_INDEX_DIR" "$home/data/xim-pkgindex"
printf 'xim_indexrepos = {}\n' > "$home/data/xim-pkgindex/xim-indexrepos.lua"
rm -f "$home/data/xim-pkgindex/.xlings-index-cache.json"
cat > "$home/.xlings.json" <<EOF
{"mirror":"GLOBAL","activeSubos":"default"}
EOF
cat > "$home/subos/default/.xlings.json" <<'EOF'
{"workspace":{
  "xpkg-helper":{"active":"0.0.1","installed":["0.0.1","0.0.0"]},
  "tool-a":{"active":"alpha:1.0.0","installed":["alpha:1.0.0"]},
  "tool-b":{"active":"beta:1.0.0","installed":["beta:1.0.0"]}
}}
EOF
cat > "$home/subos/other/.xlings.json" <<'EOF'
{"workspace":{"xpkg-helper":{"active":"0.0.1","installed":["0.0.1"]}}}
EOF
mkdir -p "$home/data/xpkgs/xim-x-xpkg-helper/0.0.1"
touch "$home/data/xpkgs/xim-x-xpkg-helper/0.0.1/.xim-installed"
mkdir -p "$home/data/xpkgs/orphan/9.9.9"
touch "$home/data/xpkgs/orphan/9.9.9/file"

run() { HOME="$root/user" XLINGS_HOME="$home" NO_COLOR=1 "$bin" "$@"; }
current=$(run list)
all=$(run list --all)
agent=$(run list --agent)
interface=$(run interface list_packages --args '{}')

for output in "$current" "$all" "$agent" "$interface"; do
  grep -q 'xpkg-helper@0.0.1' <<<"$output"
  grep -q 'xpkg-helper@0.0.0' <<<"$output"
  grep -q 'degraded: payload missing' <<<"$output"
  grep -q 'alpha:tool-a@1.0.0' <<<"$output"
  grep -q 'beta:tool-b@1.0.0' <<<"$output"
  ! grep -q 'orphan@9.9.9' <<<"$output"
done
[[ $(grep -o 'xpkg-helper@0.0.1' <<<"$all" | wc -l) -eq 1 ]]

# `--all` exists to widen the listing past the current subos, so it has to say
# which subos each row came from. The inventory has always carried the
# attribution; it just never reached the output, leaving the wider listing a
# flat set of names with no way to tell where any of them lives.
grep -q 'xpkg-helper@0.0.1.*in default, other' <<<"$all" \
  || fail "list --all does not attribute rows to their subos: $all"
! grep -q 'in default' <<<"$current" \
  || fail "the single-subos listing should not repeat the subos on every row"
echo "exact inventory contract: ok"
