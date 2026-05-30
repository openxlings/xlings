#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

SCENARIO_DIR="$ROOT_DIR/tests/e2e/scenarios/global_fallback"
HOME_DIR="$(runtime_home_dir global_fallback_home)"
PROJECT_INDEX_DIR="$ROOT_DIR/tests/e2e/fixtures/project_index"
CONFIG_BACKUP="$(prepare_scenario "$SCENARIO_DIR" "$HOME_DIR")"
cleanup() {
  restore_scenario "$SCENARIO_DIR" "$HOME_DIR" "$CONFIG_BACKUP"
}
trap cleanup EXIT
write_home_config "$HOME_DIR" "GLOBAL" "$PROJECT_INDEX_DIR"

(
  cd "$SCENARIO_DIR" &&
  run_xlings "$HOME_DIR" "$SCENARIO_DIR" config
) || true

(
  cd "$SCENARIO_DIR" &&
  run_xlings "$HOME_DIR" "$SCENARIO_DIR" update
)

(
  cd "$SCENARIO_DIR" &&
  run_xlings "$HOME_DIR" "$SCENARIO_DIR" -y install
)

[[ -f "$HOME_DIR/data/xpkgs/xim-x-node/22.17.1/bin/node" ]] \
  || fail "global fallback install did not land in isolated global xpkgs"

if [[ -d "$SCENARIO_DIR/.xlings/data" ]]; then
  fail "global fallback scenario should not create project-local data dir"
fi

# Verify node installed by checking payload binary
[[ -f "$HOME_DIR/data/xpkgs/xim-x-node/22.17.1/bin/node" ]] \
  || fail "global fallback node payload binary missing"

log "PASS: global_fallback scenario"
