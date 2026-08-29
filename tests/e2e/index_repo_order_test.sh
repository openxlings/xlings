#!/usr/bin/env bash
# E2E: the ORDER of index_repos must not decide which entry is the default
# index, and pinning the default index must not move it.
#
# Reproduced on the released 2026.8.27.4: `xlings index use xim <ver>`
# materialised the missing default entry at the END of the array (Config
# inserts it at the FRONT), index_repos[0] became a sub-index, and the official
# index artifact was downloaded into that sub-index's directory -- 185 packages
# served twice, under two namespaces.
#
# What this canNOT prove: the artifact download itself. Local fixtures are
# file:// URLs and is_local_repo_source() short-circuits before any pointer is
# consulted. That half is tests/unit/test_index_peer_sync.cpp.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

HOME_DIR="$(runtime_home_dir index_repo_order_home)"
SUB_DIR="$ROOT_DIR/tests/fixtures/xim-pkgindex-ordersub"
errors=()

cleanup() {
  rm -rf "$HOME_DIR" "$SUB_DIR"
  rm -f "$FIXTURE_INDEX_DIR/xim-indexrepos.lua"
}
trap cleanup EXIT
cleanup

assert_home_is_isolated "$HOME_DIR"

# ── 1. A sub-index with one package of its own, declared by the main fixture ──
mkdir -p "$SUB_DIR/pkgs/o"
cat > "$SUB_DIR/pkgs/o/onlysub.lua" <<'LUAEOF'
package = {
    name = "onlysub",
    description = "Exists only in the sub-index",
    authors = "test",
    license = "MIT",
    repo = "https://example.com/onlysub",
}
LUAEOF
(cd "$SUB_DIR" && git init -q && git add -A && git commit -q -m init)

cat > "$FIXTURE_INDEX_DIR/xim-indexrepos.lua" <<LUAEOF
xim_indexrepos = {
    ["ordersub"] = {
        ["GLOBAL"] = "file://$SUB_DIR",
    }
}
LUAEOF

# ── 2. The damaged shape: a non-default entry FIRST, the default entry last ──
mkdir -p "$HOME_DIR/subos/default/bin"
cp "$(find_xlings_bin)" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "ordersub", "url": "file://$SUB_DIR" },
    { "name": "xim",      "url": "$FIXTURE_INDEX_DIR" }
  ]
}
EOF

log "Running xlings update..."
run_xlings "$HOME_DIR" "$ROOT_DIR" update

# ── 3. THE differential: sub-index discovery ran against the DEFAULT index ──
#
# `ordersub` is declared in the default index's xim-indexrepos.lua, so a correct
# client syncs it a SECOND time into the sub-index tree. With the default index
# picked by POSITION, xim-indexrepos.lua was looked for in data/ordersub, which
# has no such file -- discovery returned nothing and this directory never
# appeared.
#
# Assert the DIRECTORY, not xim-indexrepos.json: save_sub_repos_json writes `{}`
# when nothing was discovered, so the FILE exists either way. That is exactly
# the assertion that would have passed against the broken build.
[[ -d "$HOME_DIR/data/xim-index-repos/xim-pkgindex-ordersub/pkgs" ]] \
  || errors+=("declared sub-index was never discovered — default index picked by position?")

# Precondition, not a differential: the xim entry gets its own directory either
# way. Here to make a setup failure legible instead of surfacing as the above.
[[ -d "$HOME_DIR/data/xim-pkgindex/pkgs" ]] \
  || errors+=("setup: the default index fixture did not sync")

# The first configured index source, read off the STRUCTURED record rather than
# the rendered panel. Two reasons: `xlings index` colours its output, and the
# suite's strip_ansi runs perl, which an isolated home has no reason to have --
# under `set -euo pipefail` that turns into a silent exit with no diagnosis.
# The JSON array preserves index_repos order.
# Two things this must NOT do, both of which kill the script silently under
# `set -euo pipefail` and look exactly like the defect under test:
#   - `awk ... exit` closes the pipe while xlings is still writing -> SIGPIPE
#     -> 141 with no message. (Measured: the first version of this test
#     "failed" against the old binary for that reason, not for the right one.)
#   - a non-zero exit from run_xlings propagates out of the substitution.
first_index_source() {
  local out
  out="$(run_xlings "$1" "$ROOT_DIR" index list --json 2>/dev/null || true)"
  printf '%s\n' "$out" | awk -F'"' '/"name":/ && !seen { print $4; seen = 1 }'
}

# ── 4. `index use` on an entry ALREADY in the file must not reorder it ──
run_xlings "$HOME_DIR" "$ROOT_DIR" index use xim latest >/dev/null 2>&1 || true
FIRST="$(first_index_source "$HOME_DIR")"
[[ "$FIRST" == "ordersub" ]] \
  || errors+=("in-place 'index use' reordered the file: first source is '$FIRST'")

# ── 5. THE second differential: materialising a MISSING default entry ──
#
# Config inserts a missing default at the FRONT of the effective list;
# `index use` appended it to the file. So pinning the default index moved it out
# of position 0 -- and position used to decide which entry WAS the default
# index. One documented command, permanently, and this is how the reported home
# got into its state.
MAT_HOME="$(runtime_home_dir index_repo_materialise_home)"
rm -rf "$MAT_HOME"
mkdir -p "$MAT_HOME/subos/default/bin"
cp "$(find_xlings_bin)" "$MAT_HOME/xlings"
cat > "$MAT_HOME/.xlings.json" <<EOF
{ "mirror": "GLOBAL", "index_repos": [ { "name": "ordersub", "url": "file://$SUB_DIR" } ] }
EOF
BEFORE="$(first_index_source "$MAT_HOME")"
[[ "$BEFORE" == "xim" ]] \
  || errors+=("setup: effective order did not start with the default ('$BEFORE')")

run_xlings "$MAT_HOME" "$ROOT_DIR" index use xim latest >/dev/null 2>&1 || true

AFTER="$(first_index_source "$MAT_HOME")"
[[ "$AFTER" == "xim" ]] \
  || errors+=("'index use xim' moved the default index: first source is now '$AFTER'")
rm -rf "$MAT_HOME"

if [[ ${#errors[@]} -gt 0 ]]; then
  printf '%s\n' "${errors[@]}" >&2
  fail "${#errors[@]} ordering defect(s)"
fi
log "PASS: index_repos order does not decide the default index"
