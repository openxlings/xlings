#!/usr/bin/env bash
# E2E test: keeper CLI surface (M4 + M5).
#
# Validates the CLI plumbing that's safe to test without an actual
# bwrap/proot install:
#   1. `xlings subos stop <name>` is a no-op when no keeper exists
#   2. `subos use --keep + --no-keep` is rejected (mutually exclusive)
#   3. `--ttl <bad>` is rejected with clear error
#   4. `--ttl <int>` parses
#   5. After manually writing a fake .keeper.pid for a stale PID,
#      `subos stop` cleans the state files
#
# The real auto-spawn-on-sandbox + nsenter integration with bwrap is
# wired in keeper.cppm but the spawning side is invoked by sandbox
# entry (use_sandbox_mode_), which requires a working bwrap install
# and runs in CI matrix tests. This script covers the CLI surface
# + state-file lifecycle in isolation.
#
# Refs: .agents/docs/subos-as-xpkg-design-2026-05-16.md (M4, M5)
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/subos_xpkg_keeper"
HOME_DIR="$RUNTIME_DIR/home"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

mkdir -p "$HOME_DIR/subos/default/bin"
cp "$(find_xlings_bin)" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "activeSubos": "default",
  "subos": { "default": { "dir": "" } }
}
EOF

log "Creating test subos..."
run_xlings "$HOME_DIR" "$ROOT_DIR" subos new k-test --storage tmpfs >/dev/null 2>&1 \
    || fail "subos new k-test failed"

# ─── Test 1: `subos stop` is a no-op when no keeper ──────────────────
log "Test 1: subos stop with no keeper running"
run_xlings "$HOME_DIR" "$ROOT_DIR" subos stop k-test >/dev/null 2>&1 \
    || fail "subos stop should be no-op for non-existing keeper"
log "  ok: subos stop is no-op when no keeper exists"

# ─── Test 2: --keep + --no-keep rejected ─────────────────────────────
log "Test 2: --keep and --no-keep mutually exclusive"
set +e
ERR_OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" subos use k-test --keep --no-keep --cmd "true" 2>&1)"
RC=$?
set -e
[[ "$RC" -ne 0 ]] || fail "--keep + --no-keep should error"
grep -qi "mutually exclusive\|incompatible" <<< "$ERR_OUT" \
    || fail "error should mention mutual exclusion (got: $ERR_OUT)"
log "  ok: --keep + --no-keep rejected"

# ─── Test 3: --ttl with non-integer rejected ─────────────────────────
log "Test 3: --ttl <non-integer> rejected"
set +e
ERR_OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" subos use k-test --ttl xyz --cmd "true" 2>&1)"
RC=$?
set -e
[[ "$RC" -ne 0 ]] || fail "--ttl xyz should error"
grep -qi "integer\|ttl" <<< "$ERR_OUT" \
    || fail "ttl error should mention integer (got: $ERR_OUT)"
log "  ok: --ttl <non-int> rejected"

# ─── Test 4: --ttl <int> parses ──────────────────────────────────────
log "Test 4: --ttl <int> + --no-keep accepted (shell-level path)"
# Use shell-level (no --sandbox) so no actual keeper spawn happens.
# The flags should still parse cleanly.
run_xlings "$HOME_DIR" "$ROOT_DIR" subos use k-test --ttl 600 --no-keep --cmd "echo ok" 2>&1 \
    | grep -qF "ok" || fail "--ttl + --no-keep should parse and exec cmd"
log "  ok: --ttl <int> + --no-keep parsed cleanly"

# ─── Test 5: subos stop cleans stale .keeper.pid ─────────────────────
log "Test 5: subos stop cleans state files (stale PID scenario)"
PID_FILE="$HOME_DIR/subos/k-test/.keeper.pid"
LU_FILE="$HOME_DIR/subos/k-test/.keeper.lastused"
echo "999999" > "$PID_FILE"      # PID that almost certainly doesn't exist
echo "1234567890" > "$LU_FILE"
run_xlings "$HOME_DIR" "$ROOT_DIR" subos stop k-test >/dev/null 2>&1 \
    || fail "subos stop with stale pid should succeed"
[[ ! -f "$PID_FILE" ]] || fail "subos stop should remove .keeper.pid"
[[ ! -f "$LU_FILE" ]]  || fail "subos stop should remove .keeper.lastused"
log "  ok: subos stop cleans state files"

log "PASS: keeper CLI surface works end-to-end (M4 + M5)"
