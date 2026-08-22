#!/usr/bin/env bash
# E2E test: verify that shims recover project context via XLINGS_PROJECT_DIR
# when CWD is outside the project directory.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

SCENARIO_DIR="$ROOT_DIR/tests/e2e/scenarios/local_repo"
HOME_DIR="$(runtime_home_dir shim_project_context_home)"
PROJECT_INDEX_DIR="$ROOT_DIR/tests/e2e/fixtures/project_index"
CONFIG_BACKUP="$(prepare_scenario "$SCENARIO_DIR" "$HOME_DIR")"
cleanup() {
  restore_scenario "$SCENARIO_DIR" "$HOME_DIR" "$CONFIG_BACKUP"
}
trap cleanup EXIT
write_home_config "$HOME_DIR" "GLOBAL" "$PROJECT_INDEX_DIR"

# --- Install node in project context (from project dir) ---
(
  cd "$SCENARIO_DIR" &&
  run_xlings "$HOME_DIR" "$SCENARIO_DIR" update
)
INSTALL_OUT="$(
  cd "$SCENARIO_DIR" &&
  run_xlings "$HOME_DIR" "$SCENARIO_DIR" -y install
)"
echo "$INSTALL_OUT"

# --- Verify: project subos bin has the node shim ---
PROJECT_BIN="$SCENARIO_DIR/.xlings/subos/_/bin"
[[ -e "$PROJECT_BIN/node" ]] || fail "project node shim missing after install"

# --- Test 1: With XLINGS_PROJECT_DIR set, shim works from outside project ---
OUTSIDE_DIR="$(mktemp -d)"
trap 'rm -rf "$OUTSIDE_DIR"; cleanup' EXIT

NODE_VER="$(
  cd "$OUTSIDE_DIR" &&
  env XLINGS_HOME="$HOME_DIR" XLINGS_PROJECT_DIR="$SCENARIO_DIR" \
    "$PROJECT_BIN/node" --version 2>&1
)" || true
echo "node --version (with XLINGS_PROJECT_DIR): $NODE_VER"
[[ "$NODE_VER" == "v22.17.1" ]] || fail "shim with XLINGS_PROJECT_DIR did not resolve expected version (got: $NODE_VER)"

# --- Test 2: Without XLINGS_PROJECT_DIR, shim fails from outside project ---
#
# HOME points at an empty directory, not the caller's.
#
# It used to be passed through, and on any machine whose real `~/.xlings` has
# node active the shim resolved THERE and answered a version -- so the "should
# have failed" assertion could not fail, on the one kind of machine that
# actually develops xlings. Green in CI for the wrong reason: CI's HOME simply
# has no xlings home to fall back to. Measured 2026-08-02: v24.15.0 from the
# developer's real home, identical on the released 2026.8.1.2 binary.
EMPTY_HOME="$OUTSIDE_DIR/empty-home"
mkdir -p "$EMPTY_HOME"
set +e
NODE_ERR="$(
  cd "$OUTSIDE_DIR" &&
  env -i HOME="$EMPTY_HOME" PATH="$PATH" XLINGS_HOME="$HOME_DIR" \
    "$PROJECT_BIN/node" --version 2>&1
)"
NODE_RC=$?
set -e
echo "node --version (without XLINGS_PROJECT_DIR): $NODE_ERR"
[[ $NODE_RC -ne 0 ]] || fail "shim without XLINGS_PROJECT_DIR should have failed"
# Assert that XLINGS reported this, not node.
#
# The old assertion looked for the literal "xlings:" -- one of three
# inconsistent prefixes the same condition used to carry ("xlings: ",
# "[xlings:use] ", none), all of which 2026.8.22.1 collapsed into one wording
# with no prefix at all. The prefix was only ever a proxy for "the shim spoke",
# so assert the two things that actually mean that: xlings's own severity
# marker, and the diagnostic this condition is supposed to produce. Neither can
# come from node.
assert_contains "$NODE_ERR" "[error]" \
  "expected xlings shim error without XLINGS_PROJECT_DIR (got: $NODE_ERR)"
assert_contains "$NODE_ERR" "is not installed in this subos" \
  "expected the not-in-subos diagnostic (got: $NODE_ERR)"

log "PASS: shim project context via XLINGS_PROJECT_DIR"
