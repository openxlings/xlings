#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

SCENARIO_DIR="$ROOT_DIR/tests/e2e/scenarios/project_home"
HOME_DIR="$(runtime_home_dir project_home_home)"
PROJECT_INDEX_DIR="$ROOT_DIR/tests/e2e/fixtures/project_index"
CONFIG_BACKUP="$(prepare_scenario "$SCENARIO_DIR" "$HOME_DIR")"
cleanup() {
  restore_scenario "$SCENARIO_DIR" "$HOME_DIR" "$CONFIG_BACKUP"
}
trap cleanup EXIT
write_home_config "$HOME_DIR" "GLOBAL" "$PROJECT_INDEX_DIR"

(cd "$SCENARIO_DIR" && run_xlings "$HOME_DIR" "$SCENARIO_DIR" config) || true

(cd "$SCENARIO_DIR" && run_xlings "$HOME_DIR" "$SCENARIO_DIR" update)

(cd "$SCENARIO_DIR" && run_xlings "$HOME_DIR" "$SCENARIO_DIR" -y install)

PROJECT_STATE="$SCENARIO_DIR/.xlings/.xlings.json"
[[ -f "$PROJECT_STATE" ]] || fail "project home runtime state file missing"
grep -q '"workspace"' "$PROJECT_STATE" \
  || fail "project home runtime state missing workspace"
grep -q '"versions"' "$PROJECT_STATE" \
  || fail "project home runtime state missing versions"
grep -q '22.17.1' "$PROJECT_STATE" \
  || fail "project home runtime state missing installed node version"
if grep -q '"index_repos"' "$PROJECT_STATE"; then
  fail "project home runtime state should not duplicate declarative index_repos"
fi

cmp -s "$CONFIG_BACKUP" "$SCENARIO_DIR/.xlings.json" \
  || fail "project manifest .xlings.json should remain unchanged"
if grep -q '"versions"' "$SCENARIO_DIR/.xlings.json"; then
  fail "project manifest .xlings.json should remain declarative and not gain versions"
fi

[[ -x "$SCENARIO_DIR/.xlings/subos/_/bin/node" ]] \
  || fail "project home anonymous subos node shim missing"
NODE_VER="$({
  cd "$SCENARIO_DIR"
  env XLINGS_HOME="$HOME_DIR" "$SCENARIO_DIR/.xlings/subos/_/bin/node" --version
})"
[[ "$NODE_VER" == "v22.17.1" ]] || fail "project home node shim did not resolve expected version"

log "PASS: project_home scenario"
