#!/usr/bin/env bash
# E2E test: install a type="subos" package.
#
# Validates:
#   1. xlings install subos:py-demo@1.0.0 succeeds (resolver routes type=subos
#      through the new dispatch, default install hook lays down skeleton)
#   2. xpkgs/<name>/<ver>/ layout matches traditional xpkg path (no special prefix)
#   3. Default install synthesizes `.xlings.json` with empty workspace when
#      tarball doesn't carry one
#   4. Default config registers via xvm so `xlings list` shows the package
#
# Refs: .agents/docs/subos-as-xpkg-design-2026-05-16.md (M1)
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/subos_xpkg_install"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
SCENARIO_FIXTURES="$ROOT_DIR/tests/e2e/fixtures/subos_xpkg"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

mkdir -p "$HOME_DIR/subos/default/bin"

# Private copy of fixture index, neutralised + injected with our subos pkg
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"

mkdir -p "$LOCAL_INDEX_DIR/pkgs/p"
cp "$SCENARIO_FIXTURES/py-demo.lua" "$LOCAL_INDEX_DIR/pkgs/p/py-demo.lua"

# Seed XLINGS_HOME pointing at the private index
cp "$(find_xlings_bin)" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$LOCAL_INDEX_DIR" }
  ]
}
EOF

run_xlings "$HOME_DIR" "$ROOT_DIR" self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

log "Installing type='subos' package: subos:py-demo@1.0.0 ..."
INSTALL_OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" install subos:py-demo@1.0.0 -y 2>&1)" || {
    echo "$INSTALL_OUT"
    fail "xlings install subos:py-demo failed"
}
echo "$INSTALL_OUT"

# 1. Standard xpkgs path: <namespace>-x-<name>/<ver>/  — same convention as
# xim-x-foo, scode-x-foo. The "type=subos" dispatch must NOT introduce
# any special prefix beyond what namespace already provides.
INSTALL_DIR="$HOME_DIR/data/xpkgs/subos-x-py-demo/1.0.0"
[[ -d "$INSTALL_DIR" ]] || fail "subos:py-demo install dir not found at $INSTALL_DIR"
log "  ok: install dir = $INSTALL_DIR"

# 2. Default install created bin/ skeleton
[[ -d "$INSTALL_DIR/bin" ]] || fail "bin/ skeleton missing from default install"
log "  ok: bin/ skeleton present"

# 3. Default install synthesized .xlings.json
[[ -f "$INSTALL_DIR/.xlings.json" ]] || fail ".xlings.json missing from default install"
log "  ok: .xlings.json synthesized"

# Verify .xlings.json shape (has workspace key)
if command -v python3 &>/dev/null; then
    python3 -c "
import json, sys
d = json.load(open('$INSTALL_DIR/.xlings.json'))
ws = d.get('workspace')
if ws is None:
    print('FAIL: .xlings.json missing workspace key', file=sys.stderr)
    sys.exit(1)
print('  ok: workspace key present, entries:', list(ws.keys()))
" || fail ".xlings.json workspace validation failed"
fi

# 4. xvm registration — xlings list should mention py-demo
LIST_OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" list 2>&1 || true)"
if ! grep -q "py-demo" <<< "$LIST_OUT"; then
    echo "$LIST_OUT"
    fail "py-demo not visible in xlings list (xvm registration broken)"
fi
log "  ok: py-demo registered in xvm (visible via xlings list)"

log "PASS: subos-as-xpkg install path works end-to-end (M1)"
