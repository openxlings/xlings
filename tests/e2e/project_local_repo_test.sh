#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

SCENARIO_DIR="$ROOT_DIR/tests/e2e/scenarios/local_repo"
HOME_DIR="$(runtime_home_dir local_repo_home)"
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

[[ -L "$SCENARIO_DIR/.xlings/data/projectrepo" || -d "$SCENARIO_DIR/.xlings/data/projectrepo/pkgs" ]] \
  || fail "projectrepo index was not materialized in project-local data dir"

(
  cd "$SCENARIO_DIR" &&
  run_xlings "$HOME_DIR" "$SCENARIO_DIR" -y install projectrepo:node@22.17.1 projectrepo:node@20.19.0
)

[[ -f "$SCENARIO_DIR/.xlings/.xlings.json" ]] \
  || fail "project runtime state file was not created"
grep -q '"versions"' "$SCENARIO_DIR/.xlings/.xlings.json" \
  || fail "project runtime state file missing versions"
grep -q '22.17.1' "$SCENARIO_DIR/.xlings/.xlings.json" \
  || fail "project runtime state file missing installed version"
cmp -s "$CONFIG_BACKUP" "$SCENARIO_DIR/.xlings.json" \
  || fail "project manifest .xlings.json should remain unchanged after install"

[[ -x "$SCENARIO_DIR/.xlings/data/xpkgs/projectrepo-x-node/22.17.1/bin/node" ]] \
  || fail "node 22.17.1 payload missing from project-local xpkgs"
[[ -x "$SCENARIO_DIR/.xlings/data/xpkgs/projectrepo-x-node/20.19.0/bin/node" ]] \
  || fail "node 20.19.0 payload missing from project-local xpkgs"

(
  cd "$SCENARIO_DIR" &&
  run_xlings "$HOME_DIR" "$SCENARIO_DIR" use node 20.19.0 >/dev/null
)
grep -q '"workspace"' "$SCENARIO_DIR/.xlings/.xlings.json" \
  || fail "project runtime state file missing workspace after use"
NODE_VER_20="$(
  cd "$SCENARIO_DIR" &&
  env XLINGS_HOME="$HOME_DIR" "$SCENARIO_DIR/.xlings/subos/_/bin/node" --version
)"
[[ "$NODE_VER_20" == "v20.19.0" ]] || fail "node shim did not switch to 20.19.0"

(
  cd "$SCENARIO_DIR" &&
  run_xlings "$HOME_DIR" "$SCENARIO_DIR" use node 22.17.1 >/dev/null
)
NODE_VER_22="$(
  cd "$SCENARIO_DIR" &&
  env XLINGS_HOME="$HOME_DIR" "$SCENARIO_DIR/.xlings/subos/_/bin/node" --version
)"
[[ "$NODE_VER_22" == "v22.17.1" ]] || fail "node shim did not switch back to 22.17.1"

log "PASS: local_repo scenario"
