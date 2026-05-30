#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

SCENARIO_NAME="${SCENARIO_NAME:-xlings_res_cn}"
EXPECTED_RES_SERVER="${EXPECTED_RES_SERVER:-https://gitcode.com/xlings-res}"
HOME_NAME="${HOME_NAME:-${SCENARIO_NAME}_home}"
SCENARIO_DIR="$ROOT_DIR/tests/e2e/scenarios/$SCENARIO_NAME"
XLINGS_RES_INDEX_DIR="$ROOT_DIR/tests/e2e/fixtures/project_index"
HOME_DIR="$(runtime_home_dir "$HOME_NAME")"
[[ -d "$XLINGS_RES_INDEX_DIR/pkgs" ]] || fail "XLINGS_RES fixture index missing: $XLINGS_RES_INDEX_DIR"
CONFIG_BACKUP="$(prepare_scenario "$SCENARIO_DIR" "$HOME_DIR")"
cleanup() {
  restore_scenario "$SCENARIO_DIR" "$HOME_DIR" "$CONFIG_BACKUP"
}
trap cleanup EXIT
write_home_config "$HOME_DIR" "CN" "$XLINGS_RES_INDEX_DIR"

(
  cd "$SCENARIO_DIR" &&
  run_xlings "$HOME_DIR" "$SCENARIO_DIR" update
)

(
  cd "$SCENARIO_DIR" &&
  run_xlings "$HOME_DIR" "$SCENARIO_DIR" -y install projectrepo:ninja@1.12.1 2>&1
)

NINJA_ARCHIVE="$(ninja_archive_name 1.12.1)"
XPKG_DIR="$SCENARIO_DIR/.xlings/data/xpkgs/projectrepo-x-ninja/1.12.1"
RUNTIME_DIR="$SCENARIO_DIR/.xlings/data/runtimedir"
[[ -f "$RUNTIME_DIR/$NINJA_ARCHIVE" ]] \
  || fail "ninja XLINGS_RES archive missing from project-local runtimedir"

[[ -x "$XPKG_DIR/ninja" ]] \
  || fail "ninja payload missing from project-local xpkgs"

[[ -x "$XPKG_DIR/ninja" ]] \
  || fail "ninja payload binary missing after install"

(
  cd "$SCENARIO_DIR" &&
  run_xlings "$HOME_DIR" "$SCENARIO_DIR" use ninja 1.12.1 >/dev/null
)
NINJA_VER="$(
  cd "$SCENARIO_DIR" &&
  env XLINGS_HOME="$HOME_DIR" "$SCENARIO_DIR/.xlings/subos/_/bin/ninja" --version
)"
assert_contains "$NINJA_VER" "1.12.1" \
  "ninja shim did not execute the installed XLINGS_RES payload"

log "PASS: $SCENARIO_NAME scenario"
