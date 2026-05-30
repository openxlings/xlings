#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

SCENARIO_DIR="$ROOT_DIR/tests/e2e/scenarios/legacy_config"
HOME_DIR="$(runtime_home_dir legacy_config_home)"
LEGACY_INDEX_DIR="$ROOT_DIR/tests/e2e/fixtures/project_index"
[[ -d "$LEGACY_INDEX_DIR/pkgs" ]] || fail "legacy fixture index missing: $LEGACY_INDEX_DIR"

# Clean up any previous run
rm -rf "$HOME_DIR" "$SCENARIO_DIR/.xlings" "$SCENARIO_DIR/.xlings.json"

cleanup() {
  rm -rf "$HOME_DIR" "$SCENARIO_DIR/.xlings" "$SCENARIO_DIR/.xlings.json"
}
trap cleanup EXIT

write_home_config "$HOME_DIR" "GLOBAL" "$LEGACY_INDEX_DIR"

# Verify no .xlings.json exists before test
[[ ! -f "$SCENARIO_DIR/.xlings.json" ]] \
  || fail ".xlings.json should not exist before legacy config test"

# Verify config.xlings exists
[[ -f "$SCENARIO_DIR/config.xlings" ]] \
  || fail "config.xlings fixture missing"

(
  cd "$SCENARIO_DIR" &&
  run_xlings "$HOME_DIR" "$SCENARIO_DIR" update
)

# Run xlings install (no arguments) — should detect config.xlings
INSTALL_OUT="$(
  cd "$SCENARIO_DIR" &&
  run_xlings "$HOME_DIR" "$SCENARIO_DIR" install 2>&1
)"
echo "$INSTALL_OUT"

# Verify .xlings.json was generated from legacy config
[[ -f "$SCENARIO_DIR/.xlings.json" ]] \
  || fail ".xlings.json was not generated from config.xlings"

# Verify generated .xlings.json contains workspace with node
grep -q '"node"' "$SCENARIO_DIR/.xlings.json" \
  || fail "generated .xlings.json should contain node in workspace"
grep -q '"22.17.1"' "$SCENARIO_DIR/.xlings.json" \
  || fail "generated .xlings.json should contain version 22.17.1"

# Verify node was actually installed from the offline fixture index.
[[ -x "$HOME_DIR/data/xpkgs/xim-x-node/22.17.1/bin/node" ]] \
  || fail "node payload missing after legacy config install"

log "PASS: legacy_config scenario"
