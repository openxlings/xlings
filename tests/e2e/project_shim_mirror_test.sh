#!/usr/bin/env bash
# E2E test: verify that project-context `xlings install` mirrors shims to
# global subos bin so that tools are reachable via PATH.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

SCENARIO_DIR="$ROOT_DIR/tests/e2e/scenarios/local_repo"
HOME_DIR="$(runtime_home_dir shim_mirror_home)"
PROJECT_INDEX_DIR="$ROOT_DIR/tests/e2e/fixtures/project_index"
CONFIG_BACKUP="$(prepare_scenario "$SCENARIO_DIR" "$HOME_DIR")"
cleanup() {
  restore_scenario "$SCENARIO_DIR" "$HOME_DIR" "$CONFIG_BACKUP"
}
trap cleanup EXIT
write_home_config "$HOME_DIR" "GLOBAL" "$PROJECT_INDEX_DIR"

# --- Record what the global subos bin already carries ---
#
# It used to `rm -rf` this directory "to avoid cache interference". That wipe
# is why this test could never see openxlings/xlings#582: with the directory
# empty there is nothing for a project-scope install to remove, so the question
# "does it remove what it does not own" was never asked here. The test that
# owned this area deleted the evidence in its own setup.
#
# So: keep whatever is there, remember it, and assert at the end that the ADD
# this test is about did not come with a silent REMOVE. The removal direction
# has its own test (project_scope_preserves_global_shims_test.sh, E2E-114);
# this row is the cheap guard that keeps this test honest about its own scope.
GLOBAL_BIN="$HOME_DIR/subos/default/bin"
mkdir -p "$GLOBAL_BIN"
GLOBAL_BEFORE="$(ls "$GLOBAL_BIN" 2>/dev/null | sort | tr '\n' ' ')"

# --- Pre-check: no node shim in global subos bin ---
[[ ! -e "$GLOBAL_BIN/node" ]] || fail "global node shim already exists before test"

# --- Scenario uses .xlings.json with workspace: {"projectrepo:node": "22.17.1"} ---
# `xlings install` reads workspace, installs the package, then calls cmd_use()
# which should create shims in both project and global subos bin.
(
  cd "$SCENARIO_DIR" &&
  run_xlings "$HOME_DIR" "$SCENARIO_DIR" update
)
INSTALL_OUT="$(
  cd "$SCENARIO_DIR" &&
  run_xlings "$HOME_DIR" "$SCENARIO_DIR" -y install
)"
echo "$INSTALL_OUT"

# --- Verify: project subos bin has the shim ---
PROJECT_BIN="$SCENARIO_DIR/.xlings/subos/_/bin"
[[ -e "$PROJECT_BIN/node" ]] || fail "project node shim missing after install"

# --- Verify: global subos bin now also has the shim ---
[[ -e "$GLOBAL_BIN/node" ]] || fail "global node shim was NOT mirrored after project install"
[[ -L "$GLOBAL_BIN/node" ]] || fail "global node shim is not a symlink"

# --- Verify: global shim resolves to xlings binary ---
RESOLVED="$(realpath "$GLOBAL_BIN/node")"
EXPECTED="$(realpath "$HOME_DIR/xlings")"
[[ "$RESOLVED" = "$EXPECTED" ]] || \
  fail "global node shim resolves to '$RESOLVED', expected '$EXPECTED'"

# --- Verify: npm/npx bindings also mirrored (node has npm/npx bindings) ---
for binding in npm npx; do
  [[ -e "$GLOBAL_BIN/$binding" ]] || fail "global $binding binding shim was NOT mirrored"
  [[ -L "$GLOBAL_BIN/$binding" ]] || fail "global $binding binding shim is not a symlink"
done

# --- Verify: pre-existing global shim is not overwritten ---
# Create a marker file, then run install again — it should remain untouched.
echo "marker" > "$GLOBAL_BIN/node"
(
  cd "$SCENARIO_DIR" &&
  run_xlings "$HOME_DIR" "$SCENARIO_DIR" -y install
) >/dev/null 2>&1
MARKER="$(cat "$GLOBAL_BIN/node")"
[[ "$MARKER" = "marker" ]] || fail "global node shim was overwritten (should preserve existing)"

# --- Verify: the mirror ADDED without silently REMOVING ---
#
# Everything present before the project install must still be present. The
# table is derived and rebuilt rather than audited, so "add one name" and
# "rebuild from a set that happens to be missing the others" look the same
# from the add side alone (#582).
GLOBAL_AFTER="$(ls "$GLOBAL_BIN" 2>/dev/null | sort | tr '\n' ' ')"
for entry in $GLOBAL_BEFORE; do
  case " $GLOBAL_AFTER " in
    *" $entry "*) ;;
    *) fail "project install removed a pre-existing global bin entry '$entry'
   before: $GLOBAL_BEFORE
   after:  $GLOBAL_AFTER" ;;
  esac
done
log "  ok: no pre-existing global entry was removed"

log "PASS: project shim mirror to global subos bin"
