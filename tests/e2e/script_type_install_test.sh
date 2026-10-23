#!/usr/bin/env bash
# E2E test: script-type package install/shim-creation/uninstall
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

HOME_DIR="$(runtime_home_dir script_type_install_home)"

cleanup() { rm -rf "$HOME_DIR"; }
trap cleanup EXIT
cleanup  # start fresh

# Clear stale index cache (it stores absolute paths from prior E2E runs)
rm -f "$FIXTURE_INDEX_DIR/.xlings-index-cache.json"

# ── 1. Set up XLINGS_HOME ──
write_home_config "$HOME_DIR" "GLOBAL"

# ── 2. Initialize home (creates subos, bin directories, etc.) ──
run_xlings "$HOME_DIR" "$ROOT_DIR" self init

# ── 3. Install script-type package xpkg-helper ──
log "Installing xpkg-helper (script type, no custom hook)..."
INSTALL_OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" install xpkg-helper -y 2>&1)" || true
echo "$INSTALL_OUT"

# ── 4. Verify .lua file was copied to install_dir ──
# The namespace prefix (xim-x-) is added by the catalog store-name logic
INSTALL_DIR="$HOME_DIR/data/xpkgs/xim-x-xpkg-helper/0.0.1"
[[ -f "$INSTALL_DIR/xpkg-helper.lua" ]] \
  || fail "xpkg-helper.lua not copied to $INSTALL_DIR"

# ── 5. Verify shim was created ──
SHIM_PATH="$HOME_DIR/subos/default/bin/xpkg-helper"
[[ -e "$SHIM_PATH" ]] \
  || fail "xpkg-helper shim not created at $SHIM_PATH"

# ── 6. Verify xvm registered (via .xlings.json versions/workspace) ──
grep -q "xpkg-helper" "$HOME_DIR/.xlings.json" \
  || fail "xpkg-helper not registered in .xlings.json"

# ── 7. Verify alias contains "xlings script" ──
grep -q "xlings script" "$HOME_DIR/.xlings.json" \
  || fail "'xlings script' alias not found in .xlings.json"

# ── 8. Verify xlings script can execute the script directly ──
log "Testing xlings script direct execution..."
SCRIPT_PATH="$INSTALL_DIR/xpkg-helper.lua"
SCRIPT_OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" script "$SCRIPT_PATH" 2>&1)" || true
echo "$SCRIPT_OUT"
assert_contains "$SCRIPT_OUT" "XPackage Helper Tools" \
  "xlings script did not execute xpkg_main correctly"

# ── 9. Verify xpkg-helper can export itself via xlings script ──
log "Testing xpkg-helper export (xpkg-helper xpkg-helper)..."
EXPORT_DIR="$HOME_DIR/export_test"
mkdir -p "$EXPORT_DIR"
EXPORT_OUT="$(cd "$EXPORT_DIR" && run_xlings "$HOME_DIR" "$ROOT_DIR" script "$SCRIPT_PATH" xpkg-helper 2>&1)" || true
echo "$EXPORT_OUT"
# xpkg-helper exports to $PWD/xim-x-xpkg-helper@0.0.1/
EXPORTED="$EXPORT_DIR/xim-x-xpkg-helper@0.0.1"
[[ -d "$EXPORTED" ]] \
  || fail "xpkg-helper export dir not created at $EXPORTED"
[[ -f "$EXPORTED/xpkg-helper.lua" ]] \
  || fail "xpkg-helper.lua not found in exported dir"
log "Export verified: $EXPORTED"
rm -rf "$EXPORT_DIR"

# ── 10. Uninstall xpkg-helper ──
log "Uninstalling xpkg-helper..."
REMOVE_OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" remove xpkg-helper -y 2>&1)" || true
echo "$REMOVE_OUT"

# ── 11. Verify shim was removed ──
[[ ! -e "$SHIM_PATH" ]] \
  || fail "shim not removed after uninstall"

# ── 12. Verify install_dir was cleaned up ──
[[ ! -d "$INSTALL_DIR" ]] \
  || fail "install_dir not removed after uninstall"

# ── 13. Regression scenario: canonical bootstrap layout ──
# `write_home_config` places the binary at <home>/xlings (legacy bootstrap
# layout). Real-world `xlings self install` puts it at <home>/bin/xlings
# (canonical). The script default_config used to look only at the legacy
# path on Unix, silently skipping shim creation in the canonical layout.
# This scenario re-runs the install with the binary ONLY at the canonical
# location to pin that fix.
log "Regression: install with canonical <home>/bin/xlings layout"
rm -rf "$HOME_DIR"
mkdir -p "$HOME_DIR/bin" "$HOME_DIR/subos/default/bin"
cp "$(find_xlings_bin)" "$HOME_DIR/bin/xlings"
[[ ! -f "$HOME_DIR/xlings" ]] || \
  fail "regression setup: legacy bootstrap path must be absent"

cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$FIXTURE_INDEX_DIR" }
  ]
}
EOF

env XLINGS_HOME="$HOME_DIR" "$HOME_DIR/bin/xlings" --verbose self init >/dev/null 2>&1
env XLINGS_HOME="$HOME_DIR" "$HOME_DIR/bin/xlings" --verbose install xpkg-helper -y >/dev/null 2>&1 \
  || fail "regression: xpkg-helper install failed under canonical layout"

[[ -e "$HOME_DIR/subos/default/bin/xpkg-helper" ]] \
  || fail "regression: shim not created under canonical layout (the bug fixed by this PR)"

log "Regression PASS: canonical layout shim creation works"

# ── 14. A home whose path holds a space: the SHIM runs the script ──
# The alias a script package is registered with names the script's path; the
# shim reads it as words, so the path is quoted there (mcpp-community/mcpp#832:
# `failed to load script: package file not found: /tmp/mc`).
SPACED="$(runtime_home_dir script_type_install_home) with space"
rm -rf "$SPACED"
write_home_config "$SPACED" "GLOBAL"
run_xlings "$SPACED" "$ROOT_DIR" self init >/dev/null 2>&1
run_xlings "$SPACED" "$ROOT_DIR" install xpkg-helper -y >/dev/null 2>&1 \
  || fail "spaced home: xpkg-helper install failed"
SPACED_OUT="$(cd /tmp && env -u XLINGS_PROJECT_DIR XLINGS_HOME="$SPACED" \
  "$SPACED/subos/default/bin/xpkg-helper" 2>&1)" || true
echo "$SPACED_OUT"
assert_contains "$SPACED_OUT" "XPackage Helper Tools" \
  "spaced home: the shim did not run the script"
rm -rf "$SPACED"
log "Spaced home PASS: the shim of a script package runs it"

log "PASS: script-type package install/shim/uninstall flow"
