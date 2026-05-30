#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

SCENARIO_DIR="$ROOT_DIR/tests/e2e/scenarios/project_workspace_platform"
HOME_DIR="$(runtime_home_dir project_workspace_platform_home)"
PROJECT_INDEX_DIR="$ROOT_DIR/tests/e2e/fixtures/project_index"
CONFIG_BACKUP="$(prepare_scenario "$SCENARIO_DIR" "$HOME_DIR")"
cleanup() {
  restore_scenario "$SCENARIO_DIR" "$HOME_DIR" "$CONFIG_BACKUP"
}
trap cleanup EXIT
write_home_config "$HOME_DIR" "GLOBAL" "$PROJECT_INDEX_DIR"

case "$(uname -s)" in
  Linux)
    EXPECTED_NODE_VERSION="20.19.0"
    ;;
  Darwin)
    EXPECTED_NODE_VERSION="22.17.1"
    ;;
  MINGW*|MSYS*|CYGWIN*)
    EXPECTED_NODE_VERSION="22.17.1"
    ;;
  *)
    EXPECTED_NODE_VERSION="22.17.1"
    ;;
esac
EXPECTED_NODE_OUTPUT="v${EXPECTED_NODE_VERSION}"

(cd "$SCENARIO_DIR" && run_xlings "$HOME_DIR" "$SCENARIO_DIR" update)

(cd "$SCENARIO_DIR" && run_xlings "$HOME_DIR" "$SCENARIO_DIR" -y install)

PROJECT_STATE="$SCENARIO_DIR/.xlings/.xlings.json"
[[ -f "$PROJECT_STATE" ]] || fail "project runtime state file missing"
grep -q "$EXPECTED_NODE_VERSION" "$PROJECT_STATE" \
  || fail "project runtime state file did not persist resolved platform version"
if grep -q '"default"' "$PROJECT_STATE"; then
  fail "project runtime state should store resolved workspace, not platform branches"
fi

NODE_VER="$({
  cd "$SCENARIO_DIR"
  env XLINGS_HOME="$HOME_DIR" "$SCENARIO_DIR/.xlings/subos/_/bin/node" --version
})"
[[ "$NODE_VER" == "$EXPECTED_NODE_OUTPUT" ]] \
  || fail "platform-aware workspace resolved unexpected node shim version"

cmp -s "$CONFIG_BACKUP" "$SCENARIO_DIR/.xlings.json" \
  || fail "project manifest should remain unchanged after platform-aware install"

log "PASS: project_workspace_platform scenario"
